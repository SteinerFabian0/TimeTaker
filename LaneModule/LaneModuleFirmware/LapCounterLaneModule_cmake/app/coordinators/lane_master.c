/*
 * lane_master.c
 *
 * @brief  Top-level coordinator for the lane module firmware.
 *
 */

#include "lane_master.h"
#include "main.h"
#include "lane_status.h"
#include "lane_behavior.h"
#include "lane_protocol.h"
#include "lane_fw_version.h"

#include "stm32_hal.h"
#include "mx_gpio_default.h"
#include "sense_manager.h"
#include "bus_manager.h"
#include "indicator_led.h"
#include "lane_timer.h"
#include "lane_chain.h"
#include "lane_bus.h"
#include "lane_sampler.h"
#include "vcnl4040.h"
#include "sys_power.h"
#include "fault_guard.h"
#include "lane_trace.h"
#include "irq_guard.h"

#include <stdbool.h>
#include <stdint.h>


/* Release number + git revision (cmake/version.cmake), so a .bin or a SWD
 * read of the chip names its build. The ID register carries the numbers;
 * the string stays for SWD and the .map. */
__attribute__((used)) static const char laneFwVersion[] = LANE_FW_VERSION_STRING;
static const char *volatile laneFwVersionRef;

/* Production values of lane_behavior.h. Warnings, so a release image still
 * builds for the bench. Add a check per new switch. */
#ifdef RELEASE
    #if LANE_FW_TREE_DIRTY
    #warning "release build from uncommitted changes - its revision cannot be rebuilt"
    #endif
    #if LANE_DEBUG_DURING_LOW_POWER
    #warning "release build keeps SWD through Sleep/Stop1 - see lane_behavior.h"
    #endif
    #if LANE_ENABLE_INIT_DONE_FLASH
    #warning "release build flashes the LED at boot - see lane_behavior.h"
    #endif
    #if LANE_ENABLE_SWO_TRACE
    #warning "release build traces over SWO - see lane_behavior.h"
    #endif
    #if LANE_ENABLE_SCOPE_MARKER
    #warning "release build drives the TP6 scope marker - see lane_behavior.h"
    #endif
    #if LANE_ENABLE_BENCH_HOOKS
    #warning "release build has the bench fault hooks - see lane_behavior.h"
    #endif
    #if LANE_BENCH_STANDALONE
    #warning "release build samples standalone without a controller - see lane_behavior.h"
    #endif
    #if LANE_DISABLE_WATCHDOG
    #warning "release build runs without the IWDG - see lane_behavior.h"
    #endif
    #if LANE_DISABLE_BUS_WATCHDOG
    #warning "release build ignores a quiet controller - see lane_behavior.h"
    #endif
#endif


#if LANE_ENABLE_INIT_DONE_FLASH
/* Non-blocking: laneRun() plays it out while the rest of the lane starts. */
static const uint32_t laneInitFlashPeriodMs  = 160U;
static const uint16_t laneInitFlashOnPermille = 500U;
static const uint16_t laneInitFlashCount      = 4U;
#endif

#if LANE_BENCH_STANDALONE
/* The LED shows the detector: a short flash per CROSSING, lit while BLOCKED. */
static const uint32_t laneStandaloneFlashMs = 80U;
#endif

#define LANE_HEALTH_TRACE_MS  1000U


#if LANE_ENABLE_ASSERTS
volatile uint8_t      laneAssertTrapArmed  = 0U;
volatile laneStatus_e laneAssertLastStatus = LANE_OK;
volatile uint32_t     laneAssertCount      = 0U;

void laneAssertRecord(laneStatus_e status)
{
    laneAssertLastStatus = status;
    laneAssertCount++;
}
#endif

#if LANE_ENABLE_BENCH_HOOKS
/*
 * Set from Live Watch; laneRun() acts once and clears it (BENCH_TESTS.md):
 *   1 HardFault (bad address)       2 hang in the loop (IWDG bites)
 *   3 faultGuardFatal(INVALID_STATE)  4 single sample into SIGNAL
 *   5 sensor recovery (bus clear)   6 hot-join request now
 *   7 sampling ON (local time)      8 sampling OFF
 */
volatile uint32_t laneBenchRequest;
#endif

static laneStatus_e laneLastError = LANE_OK;
static uint32_t     laneErrorCount;
static laneState_e  laneState     = LANE_STATE_UNENUMERATED;
static bool         laneParked;
static uint32_t     laneHealthMs;
static faultGuardBootInfo_t laneBoot;

static const char *const laneStateNames[] = {
    "UNENUMERATED", "ENUMERATED", "IDLE", "SAMPLING", "SLEEP", "PARKED",
};


/*
 * The single error policy point. Debug: record and (if armed) trap. Release:
 * record for STATUS and keep running degraded; the lane has a channel back,
 * so the mainboard learns what failed instead of watching a reboot loop.
 * faultGuardFatal() is for truly unrecoverable states only; nothing in the
 * lane reaches it today (every failure has a degraded mode).
 */
static void laneErrorHandler(laneStatus_e statusCode)
{
    if (statusCode == LANE_OK) {
        return;
    }

    laneLastError = statusCode;
    laneErrorCount++;
    laneTrace(LANE_TRACE_EVENTS, "ERROR %d", (int32_t)statusCode);

    LANE_ASSERT(statusCode);
}

void laneMasterRecordError(laneStatus_e statusCode)
{
    if (statusCode != LANE_OK) {
        laneLastError = statusCode;
        laneErrorCount++;
    }
}

laneStatus_e laneMasterGetLastError(void)
{
    return laneLastError;
}

uint32_t laneMasterGetErrorCount(void)
{
    return laneErrorCount;
}


/* =========================================================================
 *          HAL EXTI CALLBACK: the one owner, dispatching per line
 * ========================================================================= */

void HAL_EXTI_TriggerCallback(hal_exti_handle_t *hexti, hal_exti_trigger_t trigger)
{
    (void)trigger;
    if (hexti == mx_gpio_default_exti0_gethandle()) {
        vcnl4040OnInt();
    } else if (hexti == mx_gpio_default_exti12_gethandle()) {
        laneChainOnEdge();
    }
}


/* =========================================================================
 *                          STATE AND STATUS
 * ========================================================================= */

static laneState_e laneDeriveState(void)
{
    if (laneParked) {
        return LANE_STATE_PARKED;
    }
    if (busManagerSleeping()) {
        return LANE_STATE_SLEEP;
    }
    if (senseManagerMode() != LANE_SAMPLING_OFF) {
        return LANE_STATE_SAMPLING;
    }
    if (!busManagerHasAddress()) {
        return LANE_STATE_UNENUMERATED;
    }
    return senseManagerConfigWritten() ? LANE_STATE_IDLE : LANE_STATE_ENUMERATED;
}

static void lanePublishStatus(void)
{
    laneStatus_t s;
    syncTime_t   sync;
    uint32_t     now = laneTimerNow();

    laneTimerGetSync(&sync);

    s.state           = (uint8_t)laneState;
    s.samplingMode    = (uint8_t)senseManagerMode();
    s.flags           = senseManagerStatusFlags();
    s.fifoCount       = 0U;   /* patched live by the read */
    s.schedulePending = busManagerSchedulePending();
    s.nextEventSeq    = 0U;   /* patched live */
    s.droppedEvents   = 0U;   /* patched live */
    s.syncShifts      = busManagerSyncShifts();
    s.badWrites       = busManagerBadWrites();
    s.lateActions     = busManagerLateActions();
    s.lastError       = (int16_t)laneLastError;
    s.resetCause      = (uint8_t)laneBoot.resetCause;
    s.faultId         = (uint8_t)laneBoot.lastFaultId;
    s.uptimeMs        = HAL_GetTick();

    if (syncTimeIsLocked(&sync, now)) {
        s.flags |= LANE_STATUS_SYNC_LOCKED;
    } else if (sync.hasPeriod) {
        s.flags |= LANE_STATUS_SYNC_LOST;
    }
    if (sync.aligned) {
        s.flags |= LANE_STATUS_SYNC_ALIGNED;
    }
    if (busManagerBusIdleSafe()) {
        s.flags |= LANE_STATUS_BUS_IDLE_SAFE;
    }

    busManagerPublishStatus(&s);
}

static void laneHealthTrace(uint32_t nowMs)
{
    if (!LANE_TRACE_ON(LANE_TRACE_EVENTS) || (uint32_t)(nowMs - laneHealthMs) < LANE_HEALTH_TRACE_MS) {
        return;
    }
    laneHealthMs = nowMs;

    syncTime_t     sync;
    laneBusStats_t bus;
    laneTimerGetSync(&sync);
    laneBusGetStats(&bus);

    laneTrace(LANE_TRACE_EVENTS,
              "HB %s addr=0x%x idx=%u chain=%u/%u sync=%u/%u edges=%u samples=%u ovr=%u err=%u "
              "i3c w=%u r=%u e=%u sensor=%u",
              laneStateNames[(uint32_t)laneState], (uint32_t)laneBusAddress(),
              (uint32_t)busManagerLaneIndex(),
              (uint32_t)chainLogicChainIn(laneChainLogic()), (uint32_t)laneChainOut(),
              (uint32_t)syncTimeIsLocked(&sync, laneTimerNow()), (uint32_t)sync.aligned,
              sync.edgesAccepted, laneSamplerCount(), laneSamplerOverruns(), laneSamplerErrors(),
              bus.writes, bus.reads, bus.errors, (uint32_t)vcnl4040State());
}


/* =========================================================================
 *                             BENCH HOOKS
 * ========================================================================= */

#if LANE_ENABLE_BENCH_HOOKS
static void laneBenchRun(void)
{
    uint32_t req = laneBenchRequest;
    if (req == 0U) {
        return;
    }
    laneBenchRequest = 0U;
    laneTrace(LANE_TRACE_EVENTS, "BENCH request %u", req);

    switch (req) {
    case 1U:
        *(volatile uint32_t *)0xFFFFFFF0UL = 0U;   /* bus fault -> HardFault */
        break;
    case 2U:
        while (1) { }                              /* the IWDG must bite */
    case 3U:
        faultGuardFatal(LANE_ERR_INVALID_STATE);
    case 4U:
        laneErrorHandler(senseManagerSingleSample());
        break;
    case 5U:
        vcnl4040RequestRecovery();
        break;
    case 6U:
        laneErrorHandler(laneBusRequestHotJoin());
        break;
    case 7U:
        laneErrorHandler(senseManagerSetMode(LANE_SAMPLING_ON));
        break;
    case 8U:
        laneErrorHandler(senseManagerSetMode(LANE_SAMPLING_OFF));
        break;
    default:
        break;
    }
}
#endif

#if LANE_BENCH_STANDALONE
static void laneStandaloneLed(void)
{
    static uint16_t lastSeq;
    static bool     blocked;
    const eventFifo_t *f = senseManagerEventFifo();

    bool nowBlocked = (senseManagerStatusFlags() & LANE_STATUS_BLOCKED) != 0U;
    if (eventFifoNextSeq(f) != lastSeq) {
        lastSeq = eventFifoNextSeq(f);
        (void)indicatorLedBlink(2U * laneStandaloneFlashMs, 500U, 1U);
    } else if (nowBlocked != blocked && !indicatorLedIsBlinking()) {
        (void)indicatorLedSet(nowBlocked);
    }
    blocked = nowBlocked;

    /* Nobody reads the FIFO: keep it from filling up. */
    if (eventFifoCount(f) > EVENT_FIFO_DEPTH / 2U) {
        uint32_t guard = irqGuardEnter();
        eventFifoFlush(senseManagerEventFifo());
        irqGuardExit(guard);
    }
}
#endif


/* =========================================================================
 *                           INIT AND LOOP
 * ========================================================================= */

void laneInit(void)
{
    laneFwVersionRef = laneFwVersion;

    /* First: reset cause, breadcrumb, boot-loop guard. */
    faultGuardInit();
    faultGuardGetBootInfo(&laneBoot);
    laneParked = laneBoot.parked;

    /* Guide §8.1: before anything reads SENS_INT. */
    sysPowerInit();

    laneTrace(LANE_TRACE_EVENTS, "BOOT TimeTaker lane %s", LANE_FW_VERSION_STRING);
    laneTrace(LANE_TRACE_EVENTS, "reset cause=%u flags=0x%x fault=%u fatal=%d pc=0x%x abnormal=%u%s",
              (uint32_t)laneBoot.resetCause, laneBoot.resetFlags, (uint32_t)laneBoot.lastFaultId,
              laneBoot.lastFatalStatus, laneBoot.lastFaultPc, laneBoot.abnormalResets,
              laneParked ? " PARKED" : "");

    /* LED next: it is the only thing a bench board can show. */
    laneErrorHandler(indicatorLedInit());
    laneErrorHandler(laneTimerInit());
    laneErrorHandler(senseManagerInit());
    laneErrorHandler(busManagerInit(HAL_GetTick()));

    if (laneParked) {
        senseManagerPark();
        busManagerPark();
    }

#if !LANE_DISABLE_WATCHDOG
    faultGuardStartWatchdog();
#endif

#if LANE_BENCH_STANDALONE
    busManagerSetStandalone(true);
    if (!laneParked) {
        laneErrorHandler(senseManagerSetMode(LANE_SAMPLING_ON));
    }
    laneTrace(LANE_TRACE_EVENTS, "STANDALONE: sampling on local time with the default CONFIG");
#endif

#if LANE_ENABLE_INIT_DONE_FLASH
    if (!laneParked) {
        laneErrorHandler(indicatorLedBlink(laneInitFlashPeriodMs, laneInitFlashOnPermille,
                                           laneInitFlashCount));
    }
#endif
}

void laneRun(void)
{
    uint32_t nowMs = HAL_GetTick();

    faultGuardTick();

    laneErrorHandler(busManagerRun(nowMs));
    laneErrorHandler(senseManagerRun(nowMs));
    laneErrorHandler(indicatorLedRun(nowMs));

#if LANE_BENCH_STANDALONE
    laneStandaloneLed();
#endif

    laneState_e state = laneDeriveState();
    if (state != laneState) {
        laneTrace(LANE_TRACE_EVENTS, "STATE %s -> %s", laneStateNames[(uint32_t)laneState],
                  laneStateNames[(uint32_t)state]);
        laneState = state;
    }
    lanePublishStatus();
    laneHealthTrace(nowMs);

#if LANE_ENABLE_BENCH_HOOKS
    laneBenchRun();
#endif

    laneTraceFlush();
    sysPowerIdle();
}
