/*
 * TimeTaker BLE bridge: init order, the event loop, the bridge state.
 * BLE_FIRMWARE_GUIDE.md §8.
 *
 * Everything runs here, in one thread, woken by k_poll on one semaphore:
 *
 *   UART RX (ISR)  ─┐
 *   UI timer (ISR) ─┼─► wake ─► main loop: BLE events, STM frames, notify pump,
 *   BLE callbacks  ─┘                       credits, button/LED, advertising,
 *                                           STATS, watchdog
 *
 * The only work outside this loop is the NUS write path (BT RX thread ->
 * stm_link TX ring, never blocking) and the stm_ctrl reset sequences (system
 * work queue).
 *
 * The bridge never parses host frames and stores nothing (guide §1).
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_power.h>
#include <zephyr/dt-bindings/regulator/nrf5x.h>

#include "ble_link.h"
#include "ble_link_protocol.h"
#include "bind_ui.h"
#include "data_slots.h"
#include "fw_version.h"
#include "link_msg.h"
#include "stm_ctrl.h"
#include "stm_link.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* LDO only: the module's DCC pin is not connected (guide §3.1). */
BUILD_ASSERT(!IS_ENABLED(CONFIG_SOC_DCDC_NRF52X), "DC/DC must stay off on the MDBT42T-P");
BUILD_ASSERT(DT_PROP(DT_NODELABEL(reg), regulator_initial_mode) == NRF5X_REG_MODE_LDO,
             "board DTS must select the LDO regulator");

/* Longest sleep of the loop; the LED tick wakes it every 50 ms anyway. */
#define LOOP_MAX_SLEEP_MS     250
#define STATS_PERIOD_MS       1000U
#define ADV_RETRY_MS          100U

/* Kept in the image for SWD reads ("x/s fwVersionString"). */
__attribute__((used)) static const char fwVersionString[] = BLE_FW_VERSION_STRING;

static K_SEM_DEFINE(wakeSem, 0, 1);

static void wake(void)
{
    k_sem_give(&wakeSem);
}

/* --- State ------------------------------------------------------------------------- */

static dataSlots_t slots;

static struct {
    bool     connected;          /* our view: LINK_UP sent, LINK_DOWN not yet  */
    bool     advertiseWanted;    /* ADVERTISE from the STM; on after boot      */
    uint8_t  resetCause;         /* bleResetCause_e, for NRF_HELLO             */
    bool     stmHelloSeen;
    uint8_t  stmLinkMajor;
    uint8_t  stmLinkMinor;
    uint32_t versionMismatches;  /* STM_HELLO with another link MAJOR          */
    uint32_t stmDataDropped;     /* STM DATA not delivered to BLE              */
    uint32_t creditViolations;   /* STM DATA without a credit                  */
    uint32_t notifyFailures;
    uint32_t lastStatsMs;
    uint32_t lastAdvTryMs;
} st;

/* --- Watchdog (guide §10) ------------------------------------------------------- */

static const struct device *const wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static int wdtChannel = -1;

static void wdtStart(void)
{
    if (!device_is_ready(wdt)) {
        return;
    }
    const struct wdt_timeout_cfg cfg = {
        .window   = { .min = 0U, .max = CONFIG_TT_WDT_TIMEOUT_MS },
        .callback = NULL,
        .flags    = WDT_FLAG_RESET_SOC,
    };
    wdtChannel = wdt_install_timeout(wdt, &cfg);
    if (wdtChannel >= 0) {
        /* Paused while the debugger halts the CPU; it keeps running in sleep
         * (the loop wakes every 50 ms anyway). */
        (void)wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);
    }
}

static void wdtFeed(void)
{
    if (wdtChannel >= 0) {
        (void)wdt_feed(wdt, wdtChannel);
    }
}

static uint8_t readResetCause(void)
{
    const uint32_t reas = nrf_power_resetreas_get(NRF_POWER);
    nrf_power_resetreas_clear(NRF_POWER, reas);      /* so the next boot reads its own */
    return (uint8_t)linkResetCause(reas);
}

/* --- Messages to the STM -------------------------------------------------------- */

static void sendHello(void)
{
    bleCtrlNrfHello_t h = {
        .linkMajor  = BLE_LINK_MAJOR,
        .linkMinor  = BLE_LINK_MINOR,
        .fwMajor    = BLE_FW_VERSION_MAJOR,
        .fwMinor    = BLE_FW_VERSION_MINOR,
        .fwPatch    = BLE_FW_VERSION_PATCH,
        .credits    = slotsHelloCredits(&slots),
        .resetCause = st.resetCause,
        .reserved   = 0U,
        .fwGitRev   = (uint32_t)BLE_FW_GIT_REV,
    };
    bleLinkGetAddr(h.btAddr);
    if (stmLinkSendCtrl(BLE_CTRL_NRF_HELLO, &h, sizeof(h))) {
        slotsHelloSent(&slots);
    }
}

static void sendLinkUp(void)
{
    bleCtrlLinkUp_t up;
    if (bleLinkGetLinkUp(&up)) {
        (void)stmLinkSendCtrl(BLE_CTRL_LINK_UP, &up, sizeof(up));
    }
}

static void sendCredits(void)
{
    const uint8_t n = slotsOwed(&slots);
    if (n == 0U) {
        return;
    }
    const bleCtrlCredit_t c = { .count = n };
    if (stmLinkSendCtrl(BLE_CTRL_CREDIT, &c, sizeof(c))) {
        slotsCreditSent(&slots, n);
    }
}

/* Free slots minus the credits owed but not yet sent: those go out as CREDIT
 * messages, so counting them here too would hand the STM each one twice. */
static uint8_t statsSlotsFree(void)
{
    const uint8_t f = slotsFree(&slots);
    const uint8_t o = slotsOwed(&slots);
    return (o < f) ? (uint8_t)(f - o) : 0U;
}

static void sendStats(void)
{
    stmLinkCounters_t lc;
    stmLinkGetCounters(&lc);

    const bleCtrlStats_t s = {
        .rssi          = bleLinkRssi(),
        .slotsFree     = statsSlotsFree(),
        .dataDropped   = linkSat16(st.stmDataDropped + lc.txDropped),
        .linkCrcErrors = linkSat16(lc.badFrames + lc.badMessages),
        .uartErrors    = linkSat16(lc.uartErrors + lc.rxOverflows),
    };
    (void)stmLinkSendCtrl(BLE_CTRL_STATS, &s, sizeof(s));
}

/* --- Link state ------------------------------------------------------------------ */

static void refreshUi(void)
{
    bindUiSetLink(st.connected, !st.advertiseWanted);
}

static void linkUp(void)
{
    st.connected = true;
    stmCtrlSetBleIrq(true);
    sendLinkUp();
    st.lastStatsMs = k_uptime_get_32();
    refreshUi();
}

static void linkDown(uint8_t reason)
{
    st.connected = false;
    stmCtrlSetBleIrq(false);
    bleLinkNewSession();                  /* late sent-callbacks must not hit reused slots */
    slotsDropAll(&slots);                 /* every busy slot's credit is owed */
    const bleCtrlLinkDown_t d = { .hciReason = reason };
    (void)stmLinkSendCtrl(BLE_CTRL_LINK_DOWN, &d, sizeof(d));
    bindUiResetSession();
    refreshUi();
}

static void handleBleEvents(uint32_t ev)
{
    if ((ev & BLE_EVT_SENT) != 0U) {
        uint8_t counts[SLOTS_MAX] = { 0 };
        bleLinkTakeCompletions(counts);
        for (uint8_t i = 0U; i < SLOTS_MAX; i++) {
            for (uint8_t k = 0U; k < counts[i]; k++) {
                (void)slotsChunkSent(&slots, i);
            }
        }
    }

    /* A disconnect and a new connection may both have happened since the
     * last pass: close the old one first. */
    if ((ev & BLE_EVT_DISCONNECTED) != 0U && st.connected) {
        linkDown(bleLinkLastDisconnectReason());
    }
    if (!st.connected && bleLinkConnected()) {
        linkUp();
    } else if (st.connected && (ev & (BLE_EVT_PARAMS | BLE_EVT_NOTIFY_ON)) != 0U) {
        sendLinkUp();
    }
}

/* --- STM -> nRF ------------------------------------------------------------------ */

static void onStmData(const uint8_t *data, uint16_t len)
{
    if (st.connected && bleLinkNotifyEnabled()) {
        if (!slotsPut(&slots, data, len)) {
            st.creditViolations++;
            st.stmDataDropped++;
        }
        return;
    }
    /* Not connected, or notifications off: drop, and return the credit. */
    st.stmDataDropped++;
    if (!slotsDropIncoming(&slots)) {
        st.creditViolations++;
    }
}

static void onStmCtrl(const linkCtrlMsg_t *m)
{
    switch (m->type) {
    case BLE_CTRL_STM_HELLO:
        st.stmHelloSeen = true;
        st.stmLinkMajor = m->u.stmHello.linkMajor;
        st.stmLinkMinor = m->u.stmHello.linkMinor;
        if (st.stmLinkMajor != BLE_LINK_MAJOR) {
            /* The STM refuses DATA on a MAJOR mismatch and keeps CTRL; the
             * nRF still answers so the app can show both versions. */
            st.versionMismatches++;
        }
        sendHello();
        if (st.connected) {
            sendLinkUp();
        }
        break;

    case BLE_CTRL_UI_STATE:
        bindUiSetUiState(m->u.uiState.session, m->u.uiState.pairWindow != 0U);
        break;

    case BLE_CTRL_CUE:
        bindUiCue((m->u.cue.kind == BLE_CUE_BIND_ACCEPTED) ? LED_CUE_BIND_ACCEPTED
                                                           : LED_CUE_BINDS_CLEARED);
        break;

    case BLE_CTRL_KICK:
        if (st.connected) {
            (void)bleLinkKick(linkKickReason(m->u.kick.reason));
        }
        break;

    case BLE_CTRL_ADVERTISE:
        st.advertiseWanted = (m->u.advertise.on != 0U);
        if (!st.advertiseWanted) {
            (void)bleLinkAdvertiseStop();
            if (st.connected) {
                (void)bleLinkKick(LINK_KICK_DEFAULT_REASON);
            }
        }
        refreshUi();
        break;

    default:
        break;                           /* linkParse lets no other type through */
    }
}

static void handleStmRx(void)
{
    linkMsg_t m;
    int kind;

    while ((kind = stmLinkPoll(&m)) != 0) {
        if (kind == LINK_MSG_DATA) {
            onStmData(m.data, m.len);
        } else {
            onStmCtrl(&m.ctrl);
        }
    }
}

/* --- nRF -> BLE: notifications from the slots ------------------------------------ */

static void pumpNotifications(void)
{
    if (!st.connected) {
        return;
    }
    const uint16_t maxChunk    = bleLinkMaxChunk();
    const uint8_t  maxInFlight = bleLinkMaxInFlight();
    slotChunk_t c;

    while (slotsNextChunk(&slots, maxChunk, maxInFlight, &c)) {
        if (bleLinkNotify(c.data, c.len, c.slot) == 0) {
            slotsChunkQueued(&slots, &c);
        } else {
            /* Notifications off, or the link is going down: drop the rest of
             * the slot; its credit comes back once nothing is in flight. */
            slotsChunkFailed(&slots, &c);
            st.notifyFailures++;
            st.stmDataDropped++;
        }
    }
}

/* --- BLE -> STM: NUS writes (BT RX thread) ---------------------------------------- */

static void onBleWrite(const uint8_t *data, uint16_t len)
{
    /* One write = one DATA frame (guide §5.2). A write cannot exceed MTU - 3
     * = 244 B; split anyway, the DATA channel is a byte stream. Drops are
     * counted by stm_link (txDropped). */
    while (len > 0U) {
        const uint16_t n = (len < BLE_LINK_DATA_MAX) ? len : (uint16_t)BLE_LINK_DATA_MAX;
        (void)stmLinkSendData(data, n);
        data += n;
        len  -= n;
    }
}

/* --- Button ---------------------------------------------------------------------- */

static void handleButton(void)
{
    const btnEvent_e press = bindUiProcess();
    if (press == BTN_EVT_NONE) {
        return;
    }
    /* Reported even if the STM is silent; then it simply has no effect. */
    const bleCtrlButton_t b = { .press = (uint8_t)press };
    (void)stmLinkSendCtrl(BLE_CTRL_BUTTON, &b, sizeof(b));

#if defined(CONFIG_TT_WDT_SELFTEST)
    /* Bench test T10 only: a very long press hangs the loop on purpose. */
    if (press == BTN_EVT_VERY_LONG) {
        for (;;) {
        }
    }
#endif
}

/* --- Advertising ----------------------------------------------------------------- */

static void serviceAdvertising(bool recycled)
{
    if (!st.advertiseWanted || st.connected || bleLinkConnected() || bleLinkAdvertising()) {
        return;
    }
    const uint32_t now = k_uptime_get_32();
    if (!recycled && (now - st.lastAdvTryMs) < ADV_RETRY_MS) {
        return;
    }
    st.lastAdvTryMs = now;
    const int err = bleLinkAdvertiseStart();
    if (err != 0) {
        LOG_WRN("advertising start failed: %d (retrying)", err);
    }
}

/* --- Main ------------------------------------------------------------------------- */

static void fatalInit(const char *what, int err)
{
    /* No console to report to: let the watchdog reset and retry. */
    LOG_ERR("%s failed: %d", what, err);
    for (;;) {
        k_sleep(K_FOREVER);
    }
}

int main(void)
{
    st.resetCause      = readResetCause();
    st.advertiseWanted = true;

    /* Safe line states first: BLE_IRQ low, BOOT_REQ high, NRST released. */
    int err = stmCtrlInit();
    wdtStart();
    if (err != 0) {
        fatalInit("stm_ctrl", err);
    }

    slotsInit(&slots, CONFIG_TT_DATA_SLOTS);

    err = bindUiInit(wake);
    if (err != 0) {
        fatalInit("bind_ui", err);
    }
    err = stmLinkInit(wake);
    if (err != 0) {
        fatalInit("stm_link", err);
    }
    err = bleLinkInit(wake, onBleWrite);
    if (err != 0) {
        fatalInit("ble_link", err);
    }

    LOG_INF("TimeTaker BLE bridge %s, link %u.%u, reset cause %u",
            fwVersionString, BLE_LINK_MAJOR, BLE_LINK_MINOR, st.resetCause);

    sendHello();
    refreshUi();
    serviceAdvertising(true);

    struct k_poll_event wakeEvent =
        K_POLL_EVENT_INITIALIZER(K_POLL_TYPE_SEM_AVAILABLE, K_POLL_MODE_NOTIFY_ONLY, &wakeSem);

    for (;;) {
        (void)k_poll(&wakeEvent, 1, K_MSEC(LOOP_MAX_SLEEP_MS));
        wakeEvent.state = K_POLL_STATE_NOT_READY;
        (void)k_sem_take(&wakeSem, K_NO_WAIT);

        wdtFeed();

        const uint32_t bleEvents = bleLinkTakeEvents();
        handleBleEvents(bleEvents);
        handleStmRx();
        pumpNotifications();
        sendCredits();
        handleButton();
        serviceAdvertising((bleEvents & BLE_EVT_RECYCLED) != 0U);

        if (st.connected && (k_uptime_get_32() - st.lastStatsMs) >= STATS_PERIOD_MS) {
            st.lastStatsMs += STATS_PERIOD_MS;
            if ((k_uptime_get_32() - st.lastStatsMs) >= STATS_PERIOD_MS) {
                st.lastStatsMs = k_uptime_get_32();     /* no catch-up bursts */
            }
            sendStats();
        }
    }
    return 0;
}
