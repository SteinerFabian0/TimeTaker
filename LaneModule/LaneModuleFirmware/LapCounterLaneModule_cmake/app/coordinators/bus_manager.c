/*
 * bus_manager.c
 *
 * @brief  Lane bus coordinator. Rules in bus_manager.h.
 */

#include "bus_manager.h"
#include "lane_behavior.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "stm32_hal.h"
#include "lane_fw_version.h"
#include "lane_master.h"
#include "sense_manager.h"
#include "lane_regs.h"
#include "lane_bus.h"
#include "lane_chain.h"
#include "chain_logic.h"
#include "schedule_queue.h"
#include "event_fifo.h"
#include "indicator_led.h"
#include "lane_timer.h"
#include "image_crc.h"
#include "fault_guard.h"
#include "lane_trace.h"
#include "irq_guard.h"

/* Popping due actions this much early absorbs the µs rounding of the
 * mainboard -> local -> mainboard round trip, so a just-due action is not
 * re-armed for 1 µs. */
#define BUS_SCHED_SLACK_US     2U
/* COMMAND RESET waits a pass so the controller sees the write complete. */
#define BUS_RESET_DELAY_MS     5U

static laneRegs_t      busRegs;
static scheduleQueue_t busSched;
static laneId_t        busId;
static laneStatus_t    busStatus;        /* published by lane_master */
static laneConfig_t    busConfig;        /* published copy for CONFIG reads */

static bool     busStandalone;
static bool     busParked;
static uint8_t  busLaneIndex;
static bool     busSleeping;
static bool     busIdleSafe;
static uint32_t busAddressMs;
static uint32_t busResetAtMs;
static bool     busResetPending;

static volatile uint16_t busLateActions;
static uint32_t          busSyncShiftBase;

/* Scheduled sampling actions: executed in the main loop. */
#define BUS_SAMPLING_NONE 0xFFU
static volatile uint8_t  busPendingSampling = BUS_SAMPLING_NONE;
static volatile uint32_t busSchedFired;
static volatile uint32_t busSchedLastLateUs;
static volatile uint8_t  busSchedLastAction;
static uint32_t          busSchedFiredTraced;

/* IBI policy. */
static uint32_t busIbiLastMs;
static uint16_t busIbiLastSeq;
static uint16_t busIbiLastFlags;
static int16_t  busIbiLastError;

static void busSchedAlarm(uint32_t dueLocal, uint32_t firedLocal);


/* =========================================================================
 *                          SCHEDULED ACTIONS
 * ========================================================================= */

/* Arms CH4 for the queue head. Any context. */
static void busSchedArm(void)
{
    uint32_t        guard = irqGuardEnter();
    scheduleEntry_t head;

    laneTimerCancelAlarm(LANE_TIMER_ALARM_SCHEDULE);
    if (scheduleQueuePeek(&busSched, &head)) {
        uint32_t local = 0U;
        if (laneTimerToLocal(head.atUs, &local) == LANE_OK) {
            /* LANE_ERR_ALARM_RANGE (> 18 min ahead) leaves it for a later pass. */
            (void)laneTimerArmAlarm(LANE_TIMER_ALARM_SCHEDULE, local, busSchedAlarm);
        }
    }
    irqGuardExit(guard);
}

/* TIM2 ISR (priority 1): the LED switch happens here, within the ISR
 * latency of the compare. */
static void busSchedExecute(const scheduleEntry_t *e)
{
    switch (e->action) {
    case LANE_ACT_LED_OFF:
        (void)indicatorLedSet(false);
        break;
    case LANE_ACT_LED_ON:
        if (e->param > 0U) {
            (void)indicatorLedSetLevel(e->param);
        }
        (void)indicatorLedSet(true);
        break;
    case LANE_ACT_LED_BLINK:
        (void)indicatorLedBlink(e->param, 500U, 0U);
        break;
    case LANE_ACT_SAMPLING_OFF:
        busPendingSampling = (uint8_t)LANE_SAMPLING_OFF;
        break;
    case LANE_ACT_SAMPLING_ON:
        busPendingSampling = (uint8_t)LANE_SAMPLING_ON;
        break;
    default:
        break;
    }
}

static void busSchedAlarm(uint32_t dueLocal, uint32_t firedLocal)
{
    (void)dueLocal;
    uint64_t        nowMb = 0U;
    uint64_t        late  = 0U;
    scheduleEntry_t e;

    (void)laneTimerToMainboard(firedLocal, &nowMb);

    while (scheduleQueuePopDue(&busSched, nowMb + BUS_SCHED_SLACK_US, &e, &late)) {
        busSchedExecute(&e);
        late = (late > BUS_SCHED_SLACK_US) ? late - BUS_SCHED_SLACK_US : 0U;
        if (late > LANE_SCHED_LATE_US && busLateActions < 0xFFFFU) {
            busLateActions++;
        }
        busSchedLastLateUs = (uint32_t)((late > 0xFFFFFFFFULL) ? 0xFFFFFFFFULL : late);
        busSchedLastAction = e.action;
        busSchedFired++;
    }
    busSchedArm();
}

static void busSchedCancelAll(void)
{
    uint32_t guard = irqGuardEnter();
    scheduleQueueCancelAll(&busSched);
    laneTimerCancelAlarm(LANE_TIMER_ALARM_SCHEDULE);
    irqGuardExit(guard);
}


/* =========================================================================
 *                      READ RESPONSES (I3C ISR, priority 2)
 * ========================================================================= */

static uint32_t busBuildRead(uint8_t reg, uint8_t *buf, uint32_t cap, uint8_t *offered)
{
    *offered = 0U;

    switch (reg) {
    case LANE_REG_ID:
        memcpy(buf, &busId, sizeof(busId));
        return sizeof(busId);

    case LANE_REG_LANE_ID: {
        laneIndex_t li = { busLaneIndex };
        memcpy(buf, &li, sizeof(li));
        return sizeof(li);
    }

    case LANE_REG_STATUS: {
        laneStatus_t s = busStatus;
        const eventFifo_t *f = senseManagerEventFifo();
        s.fifoCount     = eventFifoCount(f);
        s.nextEventSeq  = eventFifoNextSeq(f);
        s.droppedEvents = eventFifoDropped(f);
        memcpy(buf, &s, sizeof(s));
        return sizeof(s);
    }

    case LANE_REG_EVENT_FIFO:
        return eventFifoBuildRead(senseManagerEventFifo(), buf, cap, offered);

    case LANE_REG_SIGNAL: {
        laneSignal_t sig;
        senseManagerGetSignal(&sig);
        memcpy(buf, &sig, sizeof(sig));
        return sizeof(sig);
    }

    case LANE_REG_CONFIG:
        memcpy(buf, &busConfig, sizeof(busConfig));
        return sizeof(busConfig);

    case LANE_REG_RAW_FIFO:
        return rawFifoBuildRead(senseManagerRawFifo(), buf, cap, offered);

    default:
        return 0U;   /* write-only or unknown: nothing to read */
    }
}

static void busReadDone(uint8_t reg, uint8_t offered, uint32_t clockedOut)
{
    if (reg == LANE_REG_EVENT_FIFO) {
        (void)eventFifoCompleteRead(senseManagerEventFifo(), offered, clockedOut);
    } else if (reg == LANE_REG_RAW_FIFO) {
        (void)rawFifoCompleteRead(senseManagerRawFifo(), offered, clockedOut);
    }
}


/* =========================================================================
 *                           ENUMERATION
 * ========================================================================= */

static void busPublishConfig(void)
{
    laneConfig_t c;
    senseManagerGetConfig(&c);
    uint32_t guard = irqGuardEnter();
    busConfig = c;
    irqGuardExit(guard);
}

/* The lane is no longer enumerated: back to its boot defaults. */
static void busAddressGone(const char *why)
{
    busLaneIndex = 0U;
    busSleeping  = false;
    busIdleSafe  = false;
    if (!busStandalone) {
        senseManagerResetConfig();
        busPublishConfig();
        (void)indicatorLedSet(false);
        busSchedCancelAll();
    }
    laneChainApply();
    laneTrace(LANE_TRACE_EVENTS, "UNENUMERATED (%s), CHAIN_OUT low", why);
}

static void busRunChain(uint32_t nowMs)
{
    chainLogic_t *chain = laneChainLogic();

    switch (laneChainRun(nowMs)) {
    case CHAIN_EVENT_ROSE:
        laneTrace(LANE_TRACE_EVENTS, "CHAIN_IN high");
        break;
    case CHAIN_EVENT_FELL:
        laneTrace(LANE_TRACE_EVENTS, "CHAIN_IN low");
        if (laneBusHasAddress()) {
            laneBusReleaseAddress();
        }
        busAddressGone("CHAIN_IN fell");
        break;
    default:
        break;
    }

    uint32_t ev = laneBusTakeEvents();
    if ((ev & LANE_BUS_EV_ADDRESS) != 0U) {
        chainLogicAddressAssigned(chain);
        laneChainApply();
        busAddressMs = nowMs;
        laneTrace(LANE_TRACE_EVENTS, "ENUMERATED addr=0x%x, CHAIN_OUT %s",
                  (uint32_t)laneBusAddress(), laneChainOut() ? "high" : "low");
    }
    if ((ev & LANE_BUS_EV_ADDRESS_LOST) != 0U && chainLogicHasAddress(chain)) {
        chainLogicAddressLost(chain, nowMs);
        busAddressGone("RSTDAA");
    }
    if ((ev & LANE_BUS_EV_ERROR) != 0U) {
        laneBusStats_t st;
        laneBusGetStats(&st);
        laneTrace(LANE_TRACE_EVENTS, "I3C error codes=0x%x (#%u)", st.lastErrorCodes, st.errors);
    }

    /* No ENTDAA answers unless this lane is the one meant (chain_logic). */
    laneBusSetTargetEnabled(chainLogicTargetEnabled(chain, nowMs));

    if (chainLogicJoinDue(chain, nowMs)) {
        laneStatus_e s = laneBusRequestHotJoin();
        chainLogicJoinSent(chain, nowMs);
        laneTrace(LANE_TRACE_EVENTS, "hot-join request (%d)", (int32_t)s);
        if (s != LANE_OK && s != LANE_ERR_INVALID_STATE) {
            laneMasterRecordError(s);
        }
    }
}


/* =========================================================================
 *                          REGISTER WRITES
 * ========================================================================= */

static void busReject(laneStatus_e why)
{
    laneRegsReject(&busRegs, why);
    laneMasterRecordError(why);
}

static void busWriteLed(const laneLed_t *l)
{
    switch (l->mode) {
    case LANE_LED_OFF:
        LANE_ASSERT(indicatorLedSet(false));
        break;
    case LANE_LED_ON:
        LANE_ASSERT(indicatorLedSetLevel(l->level));
        LANE_ASSERT(indicatorLedSet(true));
        break;
    default:
        LANE_ASSERT(indicatorLedSetLevel(l->level));
        LANE_ASSERT(indicatorLedBlink(l->blinkPeriodMs, l->blinkOnPermille, l->blinkCount));
        break;
    }
}

static void busWriteSchedule(const laneSchedule_t *s)
{
    if (s->op == (uint8_t)LANE_SCHED_CANCEL_ALL) {
        busSchedCancelAll();
        laneTrace(LANE_TRACE_EVENTS, "SCHED cancel-all");
        return;
    }

    uint32_t     guard  = irqGuardEnter();
    laneStatus_e status = scheduleQueueAdd(&busSched, s->atUs, s->action, s->param);
    irqGuardExit(guard);

    if (status != LANE_OK) {
        busReject(status);
        return;
    }
    busSchedArm();
    laneTrace(LANE_TRACE_EVENTS, "SCHED add action=%u param=%u", (uint32_t)s->action, (uint32_t)s->param);
    laneTraceU64Line(LANE_TRACE_EVENTS, "   at", s->atUs);
}

static void busWriteCommand(const laneCommand_t *c, uint32_t nowMs)
{
    laneStatus_e status = LANE_OK;

    switch (c->cmd) {
    case LANE_CMD_SAMPLING:
        busSleeping = false;
        busIdleSafe = false;   /* the controller's re-push clears BUS_IDLE_SAFE */
        status = senseManagerSetMode((laneSamplingMode_e)c->arg);
        break;
    case LANE_CMD_CALIBRATE:
        status = senseManagerCalibrate();
        break;
    case LANE_CMD_SINGLE_SAMPLE:
        status = senseManagerSingleSample();
        break;
    case LANE_CMD_SLEEP:
        busSleeping = c->arg != (uint8_t)LANE_SLEEP_AWAKE;
        if (busSleeping) {
            (void)senseManagerSetMode(LANE_SAMPLING_OFF);
            LANE_ASSERT(indicatorLedSet(false));
        }
        laneTrace(LANE_TRACE_EVENTS, "sleep %u", (uint32_t)c->arg);
        break;
    case LANE_CMD_RELEASE_CHAIN:
        laneBusReleaseAddress();
        chainLogicAddressLost(laneChainLogic(), nowMs);
        laneBusSetTargetEnabled(false);   /* off for the rejoin hold */
        busAddressGone("RELEASE_CHAIN");
        break;
    case LANE_CMD_RESET:
        busResetPending = true;
        busResetAtMs    = nowMs + BUS_RESET_DELAY_MS;
        break;
    case LANE_CMD_CLEAR_COUNTERS: {
        syncTime_t st;
        laneTimerGetSync(&st);
        busSyncShiftBase = st.realignShifts;
        laneRegsClearCounters(&busRegs);
        senseManagerClearCounters();
        busLateActions = 0U;
        break;
    }
    default:
        status = LANE_ERR_REG_VALUE;   /* lane_regs let it through: cannot happen */
        break;
    }

    if (status != LANE_OK) {
        busReject(status);
    }
}

static void busWriteSyncAlign(const laneSyncAlign_t *a, uint32_t rxLocal)
{
    syncTime_t st;
    laneTimerGetSync(&st);

    if (a->periodUs != st.nominalUs) {
        /* Resets lock first (lane_protocol.h); the alignment itself has to
         * wait for the next SYNC_ALIGN once the new period is measured. */
        LANE_ASSERT(laneTimerSetSyncPeriod(a->periodUs));
    }

    laneStatus_e status = laneTimerSyncAlign(a->tick, a->phaseUs, rxLocal);
    if (status != LANE_OK) {
        busReject(status);
        return;
    }
    busSchedArm();
}

static void busHandleFrame(const laneBusFrame_t *f, uint32_t nowMs)
{
    laneRegsWrite_t w;

    if (f->overflow) {
        busReject(LANE_ERR_REG_LENGTH);
        return;
    }
    if (laneRegsDecode(&busRegs, f->data, f->len, &w) != LANE_OK) {
        if (f->len > 0U) {
            laneMasterRecordError(busRegs.lastReject);
            laneTrace(LANE_TRACE_EVENTS, "write reg=0x%x len=%u rejected (%d)",
                      (uint32_t)f->data[0], (uint32_t)f->len, (int32_t)busRegs.lastReject);
        }
        return;
    }
    if (w.payloadLen == 0U) {
        return;   /* read pointer only */
    }

    if (busParked && (w.reg == LANE_REG_LED || w.reg == LANE_REG_SCHEDULE
                      || (w.reg == LANE_REG_COMMAND && w.u.command.cmd != LANE_CMD_RESET
                          && w.u.command.cmd != LANE_CMD_CLEAR_COUNTERS))) {
        busReject(LANE_ERR_PARKED);
        return;
    }

    switch (w.reg) {
    case LANE_REG_LANE_ID:
        busLaneIndex = w.u.laneIndex.laneIndex;
        laneTrace(LANE_TRACE_EVENTS, "LANE_ID = %u", (uint32_t)busLaneIndex);
        break;
    case LANE_REG_CONFIG: {
        laneStatus_e s = senseManagerApplyConfig(&w.u.config);
        if (s != LANE_OK) {
            busReject(LANE_ERR_REG_VALUE);
        }
        busPublishConfig();
        break;
    }
    case LANE_REG_LED:
        busWriteLed(&w.u.led);
        break;
    case LANE_REG_SCHEDULE:
        busWriteSchedule(&w.u.schedule);
        break;
    case LANE_REG_COMMAND:
        busWriteCommand(&w.u.command, nowMs);
        break;
    case LANE_REG_SYNC_ALIGN:
        busWriteSyncAlign(&w.u.syncAlign, f->rxLocal);
        break;
    default:
        break;
    }
}


/* =========================================================================
 *                             PUBLIC
 * ========================================================================= */

static void busFillId(void)
{
    uint32_t major = 0U;
    uint32_t minor = 0U;
    uint32_t patch = 0U;

    memset(&busId, 0, sizeof(busId));
    busId.boardType  = LANE_BOARD_TYPE_LANE;
    busId.protoMajor = LANE_PROTO_MAJOR;
    busId.protoMinor = LANE_PROTO_MINOR;

    major = LANE_FW_VERSION_MAJOR;
    minor = LANE_FW_VERSION_MINOR;
    patch = LANE_FW_VERSION_PATCH;
    busId.fwMajor  = (uint8_t)major;
    busId.fwMinor  = (uint8_t)minor;
    busId.fwPatch  = (uint8_t)patch;
    busId.fwDirty  = (uint8_t)LANE_FW_TREE_DIRTY;
    busId.fwGitRev = LANE_FW_GIT_REV;
    busId.imageCrc = imageCrcOfRunningImage();
    memcpy(busId.uid, (const void *)UID_BASE, sizeof(busId.uid));
}

laneStatus_e busManagerInit(uint32_t nowMs)
{
    laneRegsInit(&busRegs);
    scheduleQueueInit(&busSched);
    memset(&busStatus, 0, sizeof(busStatus));
    busPublishConfig();
    busFillId();

    laneTrace(LANE_TRACE_EVENTS, "ID fw=%u.%u.%u rev=%x dirty=%u crc=%x",
              (uint32_t)busId.fwMajor, (uint32_t)busId.fwMinor, (uint32_t)busId.fwPatch,
              busId.fwGitRev, (uint32_t)busId.fwDirty, busId.imageCrc);

    LANE_ASSERT_RETURN(laneChainInit(nowMs));
    LANE_ASSERT_RETURN(laneBusInit(busBuildRead, busReadDone));
    return LANE_OK;
}

static void busRunIbi(uint32_t nowMs)
{
    const eventFifo_t *f = senseManagerEventFifo();
    uint8_t  payload = 0U;
    uint16_t seq     = eventFifoNextSeq(f);
    bool     fresh   = false;

    if (eventFifoCount(f) > 0U) {
        payload |= LANE_IBI_EVENTS;
        fresh   |= seq != busIbiLastSeq;
    }
    if (rawFifoHasChunk(senseManagerRawFifo())) {
        payload |= LANE_IBI_RAW;
    }
    if (busStatus.flags != busIbiLastFlags || busStatus.lastError != busIbiLastError) {
        payload |= LANE_IBI_STATUS;
        fresh    = true;
    }

    if (payload == 0U || !laneBusIbiReady()) {
        return;
    }
    if (!fresh && (uint32_t)(nowMs - busIbiLastMs) < LANE_BUS_IBI_REPEAT_MS) {
        return;
    }

    if (laneBusRequestIbi(payload) == LANE_OK) {
        busIbiLastMs    = nowMs;
        busIbiLastSeq   = seq;
        busIbiLastFlags = busStatus.flags;
        busIbiLastError = busStatus.lastError;
    }
}

static void busRunWatchdog(uint32_t nowMs)
{
#if !LANE_DISABLE_BUS_WATCHDOG
    if (busStandalone || !laneBusHasAddress()) {
        return;
    }

    uint32_t last = laneBusLastTrafficMs();
    if ((int32_t)(busAddressMs - last) > 0) {
        last = busAddressMs;
    }

    /* The flag stays until COMMAND SAMPLING (lane_protocol.h): clearing it
     * on the next frame would hide it from the controller, whose 10 ms
     * EVENT_FIFO polls come long before its 1 s STATUS read. */
    if (!busIdleSafe && (uint32_t)(nowMs - last) > LANE_BUS_IDLE_SAFE_MS) {
        busIdleSafe = true;
        (void)senseManagerSetMode(LANE_SAMPLING_OFF);
        (void)indicatorLedSet(false);
        busSchedCancelAll();
        laneTrace(LANE_TRACE_EVENTS, "bus quiet %u ms: safe defaults", nowMs - last);
    }
#else
    (void)nowMs;
#endif
}

laneStatus_e busManagerRun(uint32_t nowMs)
{
    busRunChain(nowMs);

    laneBusFrame_t frame;
    while (laneBusPopFrame(&frame)) {
        busHandleFrame(&frame, nowMs);
    }

    /* Scheduled sampling actions land here from the TIM2 ISR. */
    uint8_t pending = busPendingSampling;
    if (pending != BUS_SAMPLING_NONE) {
        busPendingSampling = BUS_SAMPLING_NONE;
        if (!busParked) {
            (void)senseManagerSetMode((laneSamplingMode_e)pending);
        }
    }
    if (busSchedFired != busSchedFiredTraced) {
        busSchedFiredTraced = busSchedFired;
        laneTrace(LANE_TRACE_EVENTS, "SCHED fired action=%u late=%uus",
                  (uint32_t)busSchedLastAction, busSchedLastLateUs);
    }

    /* A queued action waiting for SYNC alignment or for the 18 min horizon. */
    if (scheduleQueueCount(&busSched) > 0U && !laneTimerAlarmArmed(LANE_TIMER_ALARM_SCHEDULE)) {
        busSchedArm();
    }

    busRunWatchdog(nowMs);
    if (LANE_BUS_IBI_ENABLE != 0U) {
        busRunIbi(nowMs);
    }

    if (busResetPending && (int32_t)(nowMs - busResetAtMs) >= 0) {
        laneTrace(LANE_TRACE_EVENTS, "COMMAND RESET");
        faultGuardCommandedReset();
    }
    return LANE_OK;
}

void busManagerPublishStatus(const laneStatus_t *status)
{
    uint32_t guard = irqGuardEnter();
    busStatus = *status;
    irqGuardExit(guard);
}

void busManagerSetStandalone(bool on)
{
    busStandalone = on;
}

void busManagerPark(void)
{
    busParked = true;
    busSchedCancelAll();
}

bool busManagerHasAddress(void)
{
    return laneBusHasAddress();
}

uint8_t busManagerLaneIndex(void)
{
    return busLaneIndex;
}

bool busManagerSleeping(void)
{
    return busSleeping;
}

bool busManagerBusIdleSafe(void)
{
    return busIdleSafe;
}

uint16_t busManagerBadWrites(void)
{
    return laneRegsBadWrites(&busRegs);
}

uint16_t busManagerLateActions(void)
{
    return busLateActions;
}

uint16_t busManagerSyncShifts(void)
{
    syncTime_t st;
    laneTimerGetSync(&st);
    /* A SYNC period change re-initialises sync_time and its count. */
    uint32_t shifts = (st.realignShifts >= busSyncShiftBase)
                      ? st.realignShifts - busSyncShiftBase : st.realignShifts;
    return (uint16_t)((shifts > 0xFFFFU) ? 0xFFFFU : shifts);
}

uint8_t busManagerSchedulePending(void)
{
    uint32_t guard = irqGuardEnter();
    uint8_t  n     = scheduleQueueCount(&busSched);
    irqGuardExit(guard);
    return n;
}
