/*
 * host_manager.c
 *
 * @brief  The control module's side of the host protocol. See host_manager.h.
 */

#include "host_manager.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "stm32_hal.h"
#include "ctrl_status.h"
#include "ctrl_behavior.h"
#include "ctrl_fw_version.h"
#include "host_protocol.h"
#include "ble_link_protocol.h"
#include "lane_protocol.h"
#include "host_session.h"
#include "event_log.h"
#include "bind_list.h"
#include "stream_budget.h"
#include "log_line.h"
#include "usb_cdc.h"
#include "nrf_link.h"
#include "sys_time.h"
#include "device_id.h"
#include "settings_store.h"
#include "boot_shared.h"
#include "ctrl_master.h"
#include "lane_manager.h"
#include "sequence_manager.h"

/* A REBOOT answer has to reach the host before the reset cuts USB. */
#define HOST_MANAGER_REBOOT_DELAY_MS  100U
#define HOST_MANAGER_KICK_REASON      0x13U   /* HCI "remote user terminated" */
#define HOST_MANAGER_BIND_PAGE        5U      /* BIND_LIST entries per response */

static eventLogSlot_t hostManagerLogSlots[HOST_MANAGER_EVENT_LOG_CAPACITY];
static eventLog_t     hostManagerLog;

static hostSession_t  hostManagerUsb;
static hostSession_t  hostManagerBle;
static streamBudget_t hostManagerBleBudget;

static bool     hostManagerRebootPending;
static uint32_t hostManagerRebootAtMs;

/* LANE_STATUS waiting for the bus. */
static hostSession_t *hostManagerStatusWaiter;

/* Bind policy state. */
static bindList_t hostManagerBinds;
static bool       hostManagerWindowOpen;
static uint32_t   hostManagerWindowEndMs;
static bool       hostManagerBleUnbound;          /* an UNBOUND BLE session is open */
static uint32_t   hostManagerBleUnboundSinceMs;
static uint8_t    hostManagerBleClientId[BIND_LIST_ID_BYTES];
static char       hostManagerBleClientName[BIND_LIST_NAME_BYTES];
static bool       hostManagerMismatchReported;
static int8_t     hostManagerBleRssi = 127;


/* --- Transports ---------------------------------------------------------- */

static const hostTransport_t hostManagerUsbIo = {
    .read       = usbCdcRead,
    .writeSpace = usbCdcWriteSpace,
    .write      = usbCdcWrite,
    .flush      = usbCdcFlush,
};

static const hostTransport_t hostManagerBleIo = {
    .read       = nrfLinkRead,
    .writeSpace = nrfLinkWriteSpace,
    .write      = nrfLinkWrite,
    .flush      = nrfLinkFlush,
};

static uint8_t hostManagerOtherSessions(const hostSession_t *self)
{
    const hostSession_t *other = (self == &hostManagerUsb) ? &hostManagerBle : &hostManagerUsb;
    return hostSessionIsOpen(other) ? 1U : 0U;
}


/* --- Logging and streams ----------------------------------------------------- */

void hostManagerPublishStream(uint8_t msgId, const void *payload, uint16_t len)
{
    if (hostSessionIsOpen(&hostManagerUsb)) {
        (void)hostSessionSendStream(&hostManagerUsb, msgId, payload, len);
    }
    if (hostSessionIsOpen(&hostManagerBle)
        && streamBudgetTake(&hostManagerBleBudget, HAL_GetTick(), (uint32_t)len + HP_HEADER_BYTES)) {
        (void)hostSessionSendStream(&hostManagerBle, msgId, payload, len);
    }
}

void hostManagerLog(uint8_t level, const logLine_t *line)
{
#if CTRL_ENABLE_DBG_LOG
    struct __attribute__((packed)) {
        hpDbgLog_t head;
        char       text[LOG_LINE_MAX];
    } msg;
    memset(&msg.head, 0, sizeof(msg.head));
    msg.head.level = level;
    msg.head.sysUs = sysTimeNowUs();
    memcpy(msg.text, line->text, line->len);
    hostManagerPublishStream(HP_MSG_DBG_LOG, &msg, (uint16_t)(HP_DBG_LOG_BYTES + line->len));
#else
    (void)level;
    (void)line;
#endif
}

static void hostManagerLogKv(uint8_t level, const char *head, const char *key, int32_t v)
{
    logLine_t l;
    logLineInit(&l, head);
    if (key != NULL) {
        logLineKv(&l, key, v);
    }
    hostManagerLog(level, &l);
}


/* --- Bind policy ------------------------------------------------------------- */

static uint16_t hostManagerWindowLeftS(void)
{
    if (!hostManagerWindowOpen) {
        return 0U;
    }
    const int32_t leftMs = (int32_t)(hostManagerWindowEndMs - HAL_GetTick());
    return (leftMs <= 0) ? 0U : (uint16_t)(((uint32_t)leftMs + 999U) / 1000U);
}

static void hostManagerPublishBind(uint8_t change, const uint8_t *clientId)
{
    hpEvtBindChanged_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.change  = change;
    evt.count   = hostManagerBinds.count;
    evt.windowS = hostManagerWindowLeftS();
    if (clientId != NULL) {
        memcpy(evt.clientIdPrefix, clientId, sizeof(evt.clientIdPrefix));
    }
    (void)hostManagerPublishEvent(HP_MSG_EVT_BIND_CHANGED, &evt, sizeof(evt));
    hostManagerLogKv(HP_LOG_INFO, "BIND CHANGE", "kind", change);
}

static void hostManagerSendUiState(void)
{
    uint8_t session = BLE_UI_SESSION_NONE;
    if (hostSessionIsOpen(&hostManagerBle)) {
        session = (hostSessionGetBindState(&hostManagerBle) == (uint8_t)HP_BIND_UNBOUND)
                  ? (uint8_t)BLE_UI_SESSION_UNBOUND : (uint8_t)BLE_UI_SESSION_BOUND;
    }
    const bleCtrlUiState_t ui = { .session = session, .pairWindow = hostManagerWindowOpen ? 1U : 0U };
    (void)nrfLinkSendCtrl(BLE_CTRL_UI_STATE, &ui, sizeof(ui));
}

static void hostManagerSendCue(uint8_t kind)
{
    const bleCtrlCue_t cue = { .kind = kind };
    (void)nrfLinkSendCtrl(BLE_CTRL_CUE, &cue, sizeof(cue));
}

static void hostManagerBindsChanged(void)
{
    settingsStoreSetBinds(&hostManagerBinds);   /* written when allowed, at most every 10 s */
}

/* Adds a client; publishes ADDED (and REMOVED for an evicted one). Returns
 * false for a client that can never be bound (zero id). */
static bool hostManagerBindAdd(const uint8_t *clientId, const char *name)
{
    uint8_t evicted[BIND_LIST_ID_BYTES];
    const bindListAdd_e r = bindListAdd(&hostManagerBinds, clientId, name, BIND_LIST_NAME_BYTES, evicted);
    if (r == BIND_LIST_REFUSED) {
        return false;
    }
    if (r == BIND_LIST_EVICTED) {
        hostManagerPublishBind(HP_BIND_CHANGE_REMOVED, evicted);
    }
    if (r != BIND_LIST_TOUCHED) {
        hostManagerPublishBind(HP_BIND_CHANGE_ADDED, clientId);
    }
    hostManagerBindsChanged();
    return true;
}

/* The open UNBOUND BLE session becomes BOUND now (window opened live). */
static void hostManagerBindBleLive(void)
{
    if (!hostManagerBleUnbound || !hostSessionIsOpen(&hostManagerBle)) {
        return;
    }
    if (hostManagerBindAdd(hostManagerBleClientId, hostManagerBleClientName)) {
        hostManagerBleUnbound = false;
        hostSessionSetBindState(&hostManagerBle, (uint8_t)HP_BIND_BOUND);
        hostManagerSendCue(BLE_CUE_BIND_ACCEPTED);
    }
}

static void hostManagerWindowSet(uint32_t seconds)
{
    const bool wasOpen = hostManagerWindowOpen;
    if (seconds == 0U) {
        hostManagerWindowOpen = false;
        if (wasOpen) {
            hostManagerPublishBind(HP_BIND_CHANGE_WINDOW_CLOSED, NULL);
        }
    } else {
        hostManagerWindowOpen  = true;
        hostManagerWindowEndMs = HAL_GetTick() + seconds * 1000U;
        hostManagerPublishBind(HP_BIND_CHANGE_WINDOW_OPENED, NULL);
        hostManagerBindBleLive();
    }
    hostManagerSendUiState();
}

static void hostManagerBindClearAll(void)
{
    bindListClear(&hostManagerBinds);
    hostManagerBindsChanged();
    hostManagerWindowOpen = false;
    hostManagerPublishBind(HP_BIND_CHANGE_CLEARED, NULL);
    hostManagerSendCue(BLE_CUE_BINDS_CLEARED);
    if (hostSessionIsOpen(&hostManagerBle)) {
        hostSessionSetBindState(&hostManagerBle, (uint8_t)HP_BIND_UNBOUND);
        hostManagerBleUnbound        = true;
        hostManagerBleUnboundSinceMs = HAL_GetTick();
    }
    hostManagerSendUiState();
}

static uint8_t hostManagerUsbHello(hostSession_t *self, const hpHelloReq_t *hello)
{
    (void)self;
    /* Plugging the laptop in once binds that browser for BLE later. */
    if (hello->clientKind == (uint8_t)HP_CLIENT_WEBAPP) {
        (void)hostManagerBindAdd(hello->clientId, hello->clientName);
    }
    return (uint8_t)HP_BIND_TRUSTED;
}

static uint8_t hostManagerBleHello(hostSession_t *self, const hpHelloReq_t *hello)
{
    (void)self;
    memcpy(hostManagerBleClientId, hello->clientId, sizeof(hostManagerBleClientId));
    memcpy(hostManagerBleClientName, hello->clientName, sizeof(hostManagerBleClientName));
    hostManagerBleUnbound = false;

    uint8_t state = (uint8_t)HP_BIND_UNBOUND;
    if (bindListTouch(&hostManagerBinds, hello->clientId)) {
        hostManagerBindsChanged();
        state = (uint8_t)HP_BIND_BOUND;
    } else if (hostManagerWindowOpen && hostManagerBindAdd(hello->clientId, hello->clientName)) {
        hostManagerSendCue(BLE_CUE_BIND_ACCEPTED);
        state = (uint8_t)HP_BIND_BOUND;
    } else {
        hostManagerBleUnbound        = true;
        hostManagerBleUnboundSinceMs = HAL_GetTick();
    }

    /* The session is open once this returns; UI_STATE goes out after it. */
    const bleCtrlUiState_t ui = {
        .session    = (state == (uint8_t)HP_BIND_BOUND) ? (uint8_t)BLE_UI_SESSION_BOUND : (uint8_t)BLE_UI_SESSION_UNBOUND,
        .pairWindow = hostManagerWindowOpen ? 1U : 0U,
    };
    (void)nrfLinkSendCtrl(BLE_CTRL_UI_STATE, &ui, sizeof(ui));
    hostManagerLogKv(HP_LOG_INFO, "BLE HELLO", "bind", state);
    return state;
}

static void hostManagerBindRun(void)
{
    if (hostManagerWindowOpen && (int32_t)(HAL_GetTick() - hostManagerWindowEndMs) >= 0) {
        hostManagerWindowSet(0U);
    }
    if (hostManagerBleUnbound && hostSessionIsOpen(&hostManagerBle)
        && HAL_GetTick() - hostManagerBleUnboundSinceMs >= HOST_MANAGER_UNBOUND_GRACE_MS) {
        hostManagerBleUnbound = false;
        const bleCtrlKick_t kick = { .reason = HOST_MANAGER_KICK_REASON };
        (void)nrfLinkSendCtrl(BLE_CTRL_KICK, &kick, sizeof(kick));
        hostManagerPublishBind(HP_BIND_CHANGE_REJECTED, hostManagerBleClientId);
    }
}


/* --- nRF CTRL events ------------------------------------------------------------ */

static void hostManagerPublishBleLink(uint8_t state, const bleCtrlLinkUp_t *up, uint8_t reason)
{
    hpEvtBleLink_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.state  = state;
    evt.reason = reason;
    evt.sysUs  = sysTimeNowUs();
    if (up != NULL) {
        evt.phy           = up->txPhy;
        evt.attMtu        = up->attMtu;
        evt.intervalUnits = up->intervalUnits;
    }
    (void)hostManagerPublishEvent(HP_MSG_EVT_BLE_LINK, &evt, sizeof(evt));
}

static void hostManagerBleEvents(void)
{
    nrfLinkEvent_t ev;
    while (nrfLinkTakeEvent(&ev)) {
        switch (ev.kind) {
        case NRF_LINK_EV_HELLO:
            hostManagerLogKv(HP_LOG_INFO, "BLE NRF_HELLO", "major", ev.body.hello.linkMajor);
            if (ev.body.hello.linkMajor != BLE_LINK_MAJOR) {
                if (!hostManagerMismatchReported) {
                    hostManagerMismatchReported = true;
                    hostManagerPublishBleLink(HP_BLE_LINK_VERSION_MISMATCH, NULL, 0U);
                }
            } else {
                hostManagerMismatchReported = false;
            }
            {
                const bleCtrlAdvertise_t adv = { .on = settingsStoreData()->device.bleEnabled != 0U ? 1U : 0U };
                (void)nrfLinkSendCtrl(BLE_CTRL_ADVERTISE, &adv, sizeof(adv));
            }
            hostManagerSendUiState();
            break;

        case NRF_LINK_EV_LINK_UP:
            streamBudgetSetRate(&hostManagerBleBudget,
                                streamBudgetBleRate(ev.body.linkUp.attMtu, ev.body.linkUp.intervalUnits),
                                STREAM_BUDGET_BLE_MAX);
            hostManagerPublishBleLink(HP_BLE_LINK_UP, &ev.body.linkUp, 0U);
            break;

        case NRF_LINK_EV_LINK_DOWN:
            hostSessionReset(&hostManagerBle);
            hostManagerBleUnbound = false;
            if (hostManagerStatusWaiter == &hostManagerBle) {
                hostManagerStatusWaiter = NULL;
            }
            hostManagerPublishBleLink(HP_BLE_LINK_DOWN, NULL, ev.body.linkDown.hciReason);
            break;

        case NRF_LINK_EV_BUTTON: {
            const hpEvtButton_t evt = {
                .button = (uint8_t)HP_BUTTON_BLE_BIND,
                .press  = ev.body.button.press,
                .sysUs  = sysTimeNowUs(),
            };
            (void)hostManagerPublishEvent(HP_MSG_EVT_BUTTON, &evt, sizeof(evt));
            if (ev.body.button.press == BLE_PRESS_LONG) {
                hostManagerWindowSet(HOST_MANAGER_BIND_WINDOW_S);
            } else if (ev.body.button.press == BLE_PRESS_VERY_LONG) {
                hostManagerBindClearAll();
            }
            break;
        }

        case NRF_LINK_EV_STATS:
            hostManagerBleRssi = ev.body.stats.rssi;
            break;

        default:
            break;
        }
    }
}

uint8_t hostManagerBleLinkState(void)
{
    bleCtrlNrfHello_t hello;
    if (!nrfLinkGetHello(&hello)) {
        return (uint8_t)HP_BLE_LINK_DOWN;
    }
    if (!nrfLinkVersionOk()) {
        return (uint8_t)HP_BLE_LINK_VERSION_MISMATCH;
    }
    return nrfLinkIsConnected() ? (uint8_t)HP_BLE_LINK_UP : (uint8_t)HP_BLE_LINK_DOWN;
}


/* --- Requests past the session layer -------------------------------------- */

#define HOST_MANAGER_NEED(len, want) \
    do { \
        if ((len) != (want)) { \
            return CTRL_ERR_BAD_LENGTH; \
        } \
    } while (0)

static ctrlStatus_e hostManagerNodeList(uint8_t *rsp, uint16_t rspCap, uint16_t *rspLen)
{
    hpNodeInfo_t nodes[2U + LANE_MAX_LANES];
    uint8_t      n = 0U;

    nodes[n++] = (hpNodeInfo_t){
        .nodeClass  = (uint8_t)HP_NODE_CTRL,
        .index      = 0U,
        .boardType  = (uint8_t)HP_BOARD_CONTROL,
        .state      = (uint8_t)HP_NODE_STATE_OK,
        .fwMajor    = (uint8_t)CTRL_FW_VERSION_MAJOR,
        .fwMinor    = (uint8_t)CTRL_FW_VERSION_MINOR,
        .fwPatch    = (uint8_t)CTRL_FW_VERSION_PATCH,
        .protoMinor = (uint8_t)HP_PROTO_MINOR,
        .fwGitRev   = (uint32_t)CTRL_FW_GIT_REV,
        .imageCrc   = ctrlMasterImageCrc(),
    };

    /* The BLE bridge: ABSENT until its NRF_HELLO, FAULT on a link MAJOR mismatch. */
    bleCtrlNrfHello_t hello;
    memset(&hello, 0, sizeof(hello));
    const bool seen = nrfLinkGetHello(&hello);
    nodes[n++] = (hpNodeInfo_t){
        .nodeClass  = (uint8_t)HP_NODE_BLE,
        .index      = 0U,
        .boardType  = (uint8_t)HP_BOARD_BLE_BRIDGE,
        .state      = !seen ? (uint8_t)HP_NODE_STATE_ABSENT
                            : (nrfLinkVersionOk() ? (uint8_t)HP_NODE_STATE_OK : (uint8_t)HP_NODE_STATE_FAULT),
        .fwMajor    = hello.fwMajor,
        .fwMinor    = hello.fwMinor,
        .fwPatch    = hello.fwPatch,
        .protoMinor = hello.linkMinor,
        .fwGitRev   = hello.fwGitRev,
        .imageCrc   = 0U,
    };

    for (uint8_t lane = 1U; lane <= LANE_MAX_LANES; lane++) {
        if (laneManagerNodeInfo(lane, &nodes[n])) {
            n++;
        }
    }

    const uint16_t len = (uint16_t)(HP_NODE_LIST_RSP_BYTES + n * HP_NODE_LIST_RSP_ITEM_BYTES);
    if (rspCap < len) {
        return CTRL_ERR_INVALID_STATE;
    }
    memcpy(rsp + HP_NODE_LIST_RSP_BYTES, nodes, (size_t)n * HP_NODE_LIST_RSP_ITEM_BYTES);
    *rspLen = len;
    return CTRL_OK;
}

static ctrlStatus_e hostManagerBindList(const uint8_t *req, uint16_t reqLen, uint8_t *rsp, uint16_t rspCap,
                                        uint16_t *rspLen)
{
    HOST_MANAGER_NEED(reqLen, HP_BIND_LIST_REQ_BYTES);
    hpBindListReq_t r;
    memcpy(&r, req, sizeof(r));
    if (r.start > hostManagerBinds.count) {
        return CTRL_ERR_INVALID_ARG;
    }
    uint8_t n = (uint8_t)(hostManagerBinds.count - r.start);
    if (n > HOST_MANAGER_BIND_PAGE) {
        n = HOST_MANAGER_BIND_PAGE;
    }
    const uint16_t len = (uint16_t)(HP_BIND_LIST_RSP_BYTES + n * HP_BIND_LIST_RSP_ITEM_BYTES);
    if (rspCap < len) {
        return CTRL_ERR_INVALID_STATE;
    }
    hpBindListRsp_t *out = (hpBindListRsp_t *)rsp;
    out->total   = hostManagerBinds.count;
    out->start   = r.start;
    out->windowS = hostManagerWindowLeftS();
    memcpy(rsp + HP_BIND_LIST_RSP_BYTES, &hostManagerBinds.entries[r.start], (size_t)n * HP_BIND_ENTRY_BYTES);
    *rspLen = len;
    return CTRL_OK;
}

static ctrlStatus_e hostManagerBindRemove(const uint8_t *req, uint16_t reqLen, uint16_t *rspLen)
{
    HOST_MANAGER_NEED(reqLen, HP_BIND_REMOVE_REQ_BYTES);
    hpBindRemoveReq_t r;
    memcpy(&r, req, sizeof(r));
    *rspLen = HP_BIND_REMOVE_RSP_BYTES;

    if (bindListIsZeroId(r.clientId)) {
        hostManagerBindClearAll();
        return CTRL_OK;
    }
    if (!bindListRemove(&hostManagerBinds, r.clientId)) {
        return CTRL_ERR_INVALID_ARG;
    }
    hostManagerBindsChanged();
    hostManagerPublishBind(HP_BIND_CHANGE_REMOVED, r.clientId);
    /* The BLE client just removed loses its rights at once. */
    if (hostSessionIsOpen(&hostManagerBle) && hostSessionGetBindState(&hostManagerBle) == (uint8_t)HP_BIND_BOUND
        && memcmp(hostSessionClientId(&hostManagerBle), r.clientId, BIND_LIST_ID_BYTES) == 0) {
        hostSessionSetBindState(&hostManagerBle, (uint8_t)HP_BIND_UNBOUND);
        hostManagerBleUnbound        = true;
        hostManagerBleUnboundSinceMs = HAL_GetTick();
        hostManagerSendUiState();
    }
    return CTRL_OK;
}

static ctrlStatus_e hostManagerBindAllow(const uint8_t *req, uint16_t reqLen, uint16_t *rspLen)
{
    HOST_MANAGER_NEED(reqLen, HP_BIND_ALLOW_REQ_BYTES);
    hpBindAllowReq_t r;
    memcpy(&r, req, sizeof(r));
    const uint32_t s = (r.windowS > HOST_MANAGER_BIND_WINDOW_MAX_S) ? HOST_MANAGER_BIND_WINDOW_MAX_S : r.windowS;
    hostManagerWindowSet(s);
    *rspLen = HP_BIND_ALLOW_RSP_BYTES;
    return CTRL_OK;
}

static ctrlStatus_e hostManagerReboot(const uint8_t *req, uint16_t reqLen, uint16_t *rspLen)
{
    HOST_MANAGER_NEED(reqLen, HP_REBOOT_REQ_BYTES);
    hpRebootReq_t r;
    memcpy(&r, req, sizeof(r));
    if (r.target == (uint8_t)HP_REBOOT_BOOTLOADER) {
#ifdef CTRL_APP_WITH_BOOTLOADER
        bootShared.request = BOOT_SHARED_ENTER_BOOT;   /* ctrl_boot stays and waits */
#else
        return CTRL_ERR_UNSUPPORTED;   /* standalone image: there is no bootloader in flash */
#endif
    } else if (r.target != (uint8_t)HP_REBOOT_APP) {
        return CTRL_ERR_INVALID_ARG;
    }
    hostManagerRebootPending = true;
    hostManagerRebootAtMs    = HAL_GetTick() + HOST_MANAGER_REBOOT_DELAY_MS;
    *rspLen = HP_REBOOT_RSP_BYTES;
    return CTRL_OK;
}

static ctrlStatus_e hostManagerStatus(uint16_t reqLen, uint8_t *rsp, uint16_t *rspLen)
{
    HOST_MANAGER_NEED(reqLen, HP_STATUS_REQ_BYTES);
    hpStatusRsp_t *r = (hpStatusRsp_t *)rsp;
    const uint32_t loopMax = ctrlMasterGetLoopMaxMs();
    const uint16_t writes  = settingsStoreWrites();
    const uint32_t faults  = ctrlMasterGetUsbFaults();
    r->lastError      = (int16_t)ctrlMasterGetLastError();
    r->loopMaxMs      = (loopMax > 0xFFFFU) ? 0xFFFFU : (uint16_t)loopMax;
    r->errorCount     = ctrlMasterGetErrorCount();
    r->uptimeMs       = HAL_GetTick();
    r->lanesPresent   = laneManagerPresentCount();
    r->lanesPowered   = laneManagerIsPowered() ? 1U : 0U;
    r->bleLink        = hostManagerBleLinkState();
    r->seqRunning     = sequenceManagerIsRunning() ? 1U : 0U;
    r->usbFaults      = (faults > 0xFFFFU) ? 0xFFFFU : (uint16_t)faults;
    r->settingsWrites = writes;
    r->logOldestSeq   = eventLogOldest(&hostManagerLog);
    r->logNextSeq     = eventLogNext(&hostManagerLog);
    *rspLen = HP_STATUS_RSP_BYTES;
    return CTRL_OK;
}

static ctrlStatus_e hostManagerSettingsSet(const uint8_t *req, uint16_t reqLen, uint16_t *rspLen)
{
    HOST_MANAGER_NEED(reqLen, HP_SETTINGS_SET_REQ_BYTES);
    hpSettingsSetReq_t r;
    memcpy(&r, req, sizeof(r));
    if (r.settings.bleEnabled > 1U) {
        return CTRL_ERR_INVALID_ARG;
    }
    memset(r.settings.reserved, 0, sizeof(r.settings.reserved));
    const bool bleChanged = r.settings.bleEnabled != settingsStoreData()->device.bleEnabled;
    settingsStoreSetDevice(&r.settings);
    if (bleChanged) {
        const bleCtrlAdvertise_t adv = { .on = r.settings.bleEnabled };
        (void)nrfLinkSendCtrl(BLE_CTRL_ADVERTISE, &adv, sizeof(adv));   /* 0 also kicks a central */
    }
    *rspLen = HP_SETTINGS_SET_RSP_BYTES;
    return CTRL_OK;
}

static ctrlStatus_e hostManagerLanePower(const uint8_t *req, uint16_t reqLen, uint8_t *rsp, uint16_t *rspLen)
{
    HOST_MANAGER_NEED(reqLen, HP_LANE_POWER_REQ_BYTES);
    hpLanePowerReq_t r;
    memcpy(&r, req, sizeof(r));
    if (r.on > 1U) {
        return CTRL_ERR_INVALID_ARG;
    }
    CTRL_RETURN_IF_ERR(laneManagerSetPower(r.on != 0U));
    hpLanePowerRsp_t *out = (hpLanePowerRsp_t *)rsp;
    out->powered = laneManagerIsPowered() ? 1U : 0U;
    *rspLen = HP_LANE_POWER_RSP_BYTES;
    return CTRL_OK;
}

static ctrlStatus_e hostManagerLaneConfigGet(const uint8_t *req, uint16_t reqLen, uint8_t *rsp, uint16_t *rspLen)
{
    HOST_MANAGER_NEED(reqLen, HP_LANE_CONFIG_GET_REQ_BYTES);
    hpLaneConfigGetRsp_t *out = (hpLaneConfigGetRsp_t *)rsp;
    out->lane = req[0];
    CTRL_RETURN_IF_ERR(laneManagerGetConfig(req[0], &out->config));
    *rspLen = HP_LANE_CONFIG_GET_RSP_BYTES;
    return CTRL_OK;
}

static ctrlStatus_e hostManagerLaneConfigSet(const uint8_t *req, uint16_t reqLen, uint16_t *rspLen)
{
    HOST_MANAGER_NEED(reqLen, HP_LANE_CONFIG_SET_REQ_BYTES);
    hpLaneConfigSetReq_t r;
    memcpy(&r, req, sizeof(r));
    if (r.save > 1U) {
        return CTRL_ERR_INVALID_ARG;
    }
    CTRL_RETURN_IF_ERR(laneManagerSetConfig(r.laneMask, &r.config, r.save != 0U));
    *rspLen = HP_LANE_CONFIG_SET_RSP_BYTES;
    return CTRL_OK;
}

static ctrlStatus_e hostManagerLaneLed(const uint8_t *req, uint16_t reqLen, uint16_t *rspLen)
{
    HOST_MANAGER_NEED(reqLen, HP_LANE_LED_REQ_BYTES);
    hpLaneLedReq_t r;
    memcpy(&r, req, sizeof(r));
    if (sequenceManagerIsRunning()) {
        return CTRL_ERR_SEQ_RUNNING;   /* the start lights are the sequence's */
    }
    if (r.mode > (uint8_t)HP_LED_BLINK || r.level > 1000U || r.blinkOnPermille > 1000U
        || (r.mode == (uint8_t)HP_LED_BLINK && r.blinkPeriodMs == 0U)) {
        return CTRL_ERR_INVALID_ARG;
    }
    const laneLed_t led = {
        .mode            = r.mode,
        .level           = r.level,
        .blinkPeriodMs   = r.blinkPeriodMs,
        .blinkOnPermille = r.blinkOnPermille,
        .blinkCount      = r.blinkCount,
    };
    CTRL_RETURN_IF_ERR(laneManagerSetLed(r.laneMask, &led));
    *rspLen = HP_LANE_LED_RSP_BYTES;
    return CTRL_OK;
}

static ctrlStatus_e hostManagerLaneStatus(hostSession_t *session, const uint8_t *req, uint16_t reqLen)
{
    HOST_MANAGER_NEED(reqLen, HP_LANE_STATUS_REQ_BYTES);
    if (hostManagerStatusWaiter != NULL || hostSessionHasDeferred(session)) {
        return CTRL_ERR_BUSY;
    }
    CTRL_RETURN_IF_ERR(laneManagerStatusRequest(req[0]));
    hostManagerStatusWaiter = session;
    hostSessionDefer(session);   /* answered from hostManagerRun() once the bus has it */
    return CTRL_OK;
}

static ctrlStatus_e hostManagerStartSequence(const uint8_t *req, uint16_t reqLen, uint8_t *rsp, uint16_t *rspLen)
{
    HOST_MANAGER_NEED(reqLen, HP_START_SEQUENCE_REQ_BYTES);
    hpStartSequenceReq_t r;
    memcpy(&r, req, sizeof(r));
    hpStartSequenceRsp_t *out = (hpStartSequenceRsp_t *)rsp;
    uint64_t t0 = 0U;
    CTRL_RETURN_IF_ERR(sequenceManagerStart(&r, &t0));
    out->t0Us = t0;
    *rspLen = HP_START_SEQUENCE_RSP_BYTES;
    return CTRL_OK;
}

static ctrlStatus_e hostManagerOnRequest(hostSession_t *session, uint8_t msgId,
                                         const uint8_t *req, uint16_t reqLen,
                                         uint8_t *rsp, uint16_t rspCap, uint16_t *rspLen)
{
    switch (msgId) {
    case HP_MSG_NODE_LIST:
        HOST_MANAGER_NEED(reqLen, HP_NODE_LIST_REQ_BYTES);
        return hostManagerNodeList(rsp, rspCap, rspLen);

    case HP_MSG_BIND_LIST:
        return hostManagerBindList(req, reqLen, rsp, rspCap, rspLen);

    case HP_MSG_BIND_REMOVE:
        return hostManagerBindRemove(req, reqLen, rspLen);

    case HP_MSG_BIND_ALLOW:   /* UNBOUND sessions never get here (host_session gate) */
        return hostManagerBindAllow(req, reqLen, rspLen);

    case HP_MSG_REBOOT:
        return hostManagerReboot(req, reqLen, rspLen);

    case HP_MSG_STATUS:
        return hostManagerStatus(reqLen, rsp, rspLen);

    case HP_MSG_SETTINGS_GET:
        HOST_MANAGER_NEED(reqLen, HP_SETTINGS_GET_REQ_BYTES);
        ((hpSettingsGetRsp_t *)rsp)->settings = settingsStoreData()->device;
        *rspLen = HP_SETTINGS_GET_RSP_BYTES;
        return CTRL_OK;

    case HP_MSG_SETTINGS_SET:
        return hostManagerSettingsSet(req, reqLen, rspLen);

    case HP_MSG_LANE_SCAN:
        HOST_MANAGER_NEED(reqLen, HP_LANE_SCAN_REQ_BYTES);
        if (sequenceManagerIsRunning()) {
            return CTRL_ERR_SEQ_RUNNING;
        }
        CTRL_RETURN_IF_ERR(laneManagerScan());
        *rspLen = HP_LANE_SCAN_RSP_BYTES;
        return CTRL_OK;

    case HP_MSG_LANE_POWER:
        return hostManagerLanePower(req, reqLen, rsp, rspLen);

    case HP_MSG_LANE_CONFIG_GET:
        return hostManagerLaneConfigGet(req, reqLen, rsp, rspLen);

    case HP_MSG_LANE_CONFIG_SET:
        return hostManagerLaneConfigSet(req, reqLen, rspLen);

    case HP_MSG_LANE_SAMPLING: {
        HOST_MANAGER_NEED(reqLen, HP_LANE_SAMPLING_REQ_BYTES);
        hpLaneSamplingReq_t r;
        memcpy(&r, req, sizeof(r));
        CTRL_RETURN_IF_ERR(laneManagerSetSampling(r.laneMask, r.mode));
        *rspLen = HP_LANE_SAMPLING_RSP_BYTES;
        return CTRL_OK;
    }

    case HP_MSG_LANE_LED:
        return hostManagerLaneLed(req, reqLen, rspLen);

    case HP_MSG_LANE_STATUS:
        return hostManagerLaneStatus(session, req, reqLen);

    case HP_MSG_LANE_RAW_STREAM: {
        HOST_MANAGER_NEED(reqLen, HP_LANE_RAW_STREAM_REQ_BYTES);
        hpLaneRawStreamReq_t r;
        memcpy(&r, req, sizeof(r));
        CTRL_RETURN_IF_ERR(laneManagerRawStream(r.lane, r.decimation));
        *rspLen = HP_LANE_RAW_STREAM_RSP_BYTES;
        return CTRL_OK;
    }

    case HP_MSG_LANE_CALIBRATE:
        HOST_MANAGER_NEED(reqLen, HP_LANE_CALIBRATE_REQ_BYTES);
        CTRL_RETURN_IF_ERR(laneManagerCalibrate(req[0]));
        *rspLen = HP_LANE_CALIBRATE_RSP_BYTES;
        return CTRL_OK;

    case HP_MSG_START_SEQUENCE:
        return hostManagerStartSequence(req, reqLen, rsp, rspLen);

    case HP_MSG_ABORT_SEQUENCE:
        HOST_MANAGER_NEED(reqLen, HP_ABORT_SEQUENCE_REQ_BYTES);
        CTRL_RETURN_IF_ERR(sequenceManagerAbort(HP_ABORT_HOST));
        *rspLen = HP_ABORT_SEQUENCE_RSP_BYTES;
        return CTRL_OK;

    case HP_MSG_UPDATE_BEGIN:
    case HP_MSG_UPDATE_CHUNK:
    case HP_MSG_UPDATE_END:
    case HP_MSG_UPDATE_ABORT:
    case HP_MSG_BOOT_APP:
        return CTRL_ERR_INVALID_STATE;   /* the bootloader's: REBOOT{BOOTLOADER} first */

    default:
        return CTRL_ERR_UNSUPPORTED;
    }
}


/* --- API --------------------------------------------------------------------- */

ctrlStatus_e hostManagerPublishEvent(uint8_t msgId, const void *event, uint16_t size)
{
    return eventLogAppend(&hostManagerLog, msgId, event, size, NULL);
}

ctrlStatus_e hostManagerInit(void)
{
    eventLogInit(&hostManagerLog, hostManagerLogSlots, HOST_MANAGER_EVENT_LOG_CAPACITY);
    hostManagerRebootPending = false;
    hostManagerStatusWaiter  = NULL;
    streamBudgetInit(&hostManagerBleBudget, STREAM_BUDGET_BLE_MIN, STREAM_BUDGET_BLE_MAX, HAL_GetTick());

    if (!bindListDeserialize(&hostManagerBinds, settingsStoreData()->binds, BIND_LIST_SERIAL_BYTES)) {
        bindListInit(&hostManagerBinds);
    }

    const hostSessionConfig_t usbCfg = {
        .transport     = (uint8_t)HP_TRANSPORT_USB,
        .bindState     = (uint8_t)HP_BIND_TRUSTED,   /* a cable is physical access */
        .bootId        = deviceIdBootId(),
        .log           = &hostManagerLog,
        .nowUs         = sysTimeNowUs,
        .otherSessions = hostManagerOtherSessions,
        .onRequest     = hostManagerOnRequest,
        .onHello       = hostManagerUsbHello,
    };
    hostSessionInit(&hostManagerUsb, &hostManagerUsbIo, &usbCfg);

    const hostSessionConfig_t bleCfg = {
        .transport     = (uint8_t)HP_TRANSPORT_BLE,
        .bindState     = (uint8_t)HP_BIND_UNBOUND,   /* decided at HELLO by the bind list */
        .bootId        = deviceIdBootId(),
        .log           = &hostManagerLog,
        .nowUs         = sysTimeNowUs,
        .otherSessions = hostManagerOtherSessions,
        .onRequest     = hostManagerOnRequest,
        .onHello       = hostManagerBleHello,
    };
    hostSessionInit(&hostManagerBle, &hostManagerBleIo, &bleCfg);

    const hpEvtBoot_t boot = {
        .resetCause = deviceIdResetCause(),
        .bootId     = deviceIdBootId(),
        .sysUs      = sysTimeNowUs(),
    };
    return hostManagerPublishEvent(HP_MSG_EVT_BOOT, &boot, sizeof(boot));
}

ctrlStatus_e hostManagerRun(void)
{
    if (usbCdcTakeLinkReset()) {
        hostSessionReset(&hostManagerUsb);
        if (hostManagerStatusWaiter == &hostManagerUsb) {
            hostManagerStatusWaiter = NULL;
        }
    }
    hostSessionRun(&hostManagerUsb);

    hostManagerBleEvents();
    if (nrfLinkTakeLinkReset()) {
        hostSessionReset(&hostManagerBle);
        hostManagerBleUnbound = false;
        if (hostManagerStatusWaiter == &hostManagerBle) {
            hostManagerStatusWaiter = NULL;
        }
    }
#if !CTRL_ENABLE_NRF_LINK_ECHO
    hostSessionRun(&hostManagerBle);
#endif
    hostManagerBindRun();

    /* A LANE_STATUS answer the bus has delivered. */
    hpLaneStatusRsp_t rsp;
    ctrlStatus_e      st;
    memset(&rsp, 0, sizeof(rsp));
    if (laneManagerStatusPoll(&rsp, &st) && hostManagerStatusWaiter != NULL) {
        (void)hostSessionCompleteDeferred(hostManagerStatusWaiter, st, (uint8_t *)&rsp, (uint16_t)sizeof(rsp));
        hostManagerStatusWaiter = NULL;
    }

    if (hostManagerRebootPending && (int32_t)(HAL_GetTick() - hostManagerRebootAtMs) >= 0) {
        NVIC_SystemReset();
    }
    return CTRL_OK;
}

bool hostManagerAnySessionOpen(void)
{
    return hostSessionIsOpen(&hostManagerUsb) || hostSessionIsOpen(&hostManagerBle);
}

bool hostManagerAnyLinkUp(void)
{
    return usbCdcIsMounted() || nrfLinkIsConnected();
}

uint32_t hostManagerTrafficCount(void)
{
    return hostManagerUsb.stats.framesIn + hostManagerUsb.stats.eventsSent
         + hostManagerBle.stats.framesIn + hostManagerBle.stats.eventsSent;
}
