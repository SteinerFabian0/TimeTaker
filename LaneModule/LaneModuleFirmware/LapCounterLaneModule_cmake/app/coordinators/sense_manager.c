/*
 * sense_manager.c
 *
 * @brief  Sampling, detection and the event/raw FIFOs. Rules in
 *         sense_manager.h.
 */

#include "sense_manager.h"
#include "lane_behavior.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "stm32_hal.h"
#include "lane_status.h"
#include "lane_protocol.h"
#include "lane_regs.h"
#include "lane_detect.h"
#include "event_fifo.h"
#include "lane_sampler.h"
#include "lane_timer.h"
#include "vcnl4040.h"
#include "lane_trace.h"
#include "irq_guard.h"

static laneConfig_t       senseConfig;
static bool               senseConfigWritten;
static laneSamplingMode_e senseMode;
static bool               senseParked;

static laneDetect_t senseDetect;
static eventFifo_t  senseEvents;
static rawFifo_t    senseRaw;
static laneSignal_t senseSignal;

static bool     senseSinglePending;
static bool     senseSingleStarted;
static bool     senseHoldoffRate;
static bool     senseFaultReported;
static uint32_t senseRawCountdown;
static uint32_t senseSignalTraceMs;

static const char *const senseEventNames[] = { "?", "CROSSING", "BLOCKED", "CLEARED", "SENSOR_FAULT" };

static void senseDetectConfig(const laneConfig_t *c, laneDetectConfig_t *d)
{
    d->thrDropPermille = c->thrDropPermille;
    d->hystPermille    = c->hystPermille;
    d->mergeGapUs      = c->mergeGapUs;
    d->blockTimeoutUs  = c->blockTimeoutUs;
    d->minDwellUs      = (uint32_t)c->minDwell10us * 10U;
    d->holdoffUs       = c->holdoffUs;
    d->saturationRaw   = VCNL4040_FULL_SCALE;
}

static vcnl4040Settings_t senseSensorSettings(void)
{
    vcnl4040Settings_t s;
    s.irCurrent     = senseConfig.irCurrent;
    s.psIntegration = senseConfig.psIntegration;
    s.psMultiPulse  = senseConfig.psMultiPulse;
    return s;
}

static uint32_t senseConversionUs(void)
{
    return laneRegsConversionUs(senseConfig.psIntegration, senseConfig.psMultiPulse);
}

static uint32_t senseCurrentPeriod(void)
{
    return (senseHoldoffRate && senseConfig.holdoffSamplePeriodUs != 0U)
           ? senseConfig.holdoffSamplePeriodUs : senseConfig.samplePeriodUs;
}

laneStatus_e senseManagerInit(void)
{
    laneDetectConfig_t dc;

    laneRegsDefaultConfig(&senseConfig);
    senseConfigWritten = false;
    senseMode          = LANE_SAMPLING_OFF;
    senseParked        = false;
    memset(&senseSignal, 0, sizeof(senseSignal));

    senseDetectConfig(&senseConfig, &dc);
    LANE_ASSERT_RETURN(laneDetectInit(&senseDetect, &dc));
    eventFifoInit(&senseEvents);
    rawFifoInit(&senseRaw);

    LANE_ASSERT_RETURN(laneSamplerInit());
    LANE_ASSERT_RETURN(vcnl4040Init());
    return LANE_OK;
}

void senseManagerPushEvent(uint8_t type, uint8_t flags, uint32_t tEntryLocal,
                           uint32_t dwellUs, uint16_t depth)
{
    laneEvent_t ev;
    uint64_t    entryMb = 0U;
    uint64_t    exitMb  = 0U;

    memset(&ev, 0, sizeof(ev));
    uint8_t syncFlags = laneTimerToMainboard(tEntryLocal, &entryMb);
    (void)laneTimerToMainboard(tEntryLocal + dwellUs, &exitMb);

    uint64_t dwellMb = (exitMb > entryMb) ? (exitMb - entryMb) : 0U;
    uint64_t dwell10 = dwellMb / 10U;

    ev.type      = type;
    ev.flags     = (uint8_t)(flags | syncFlags);
    ev.tEntryUs  = entryMb;
    ev.dwell10us = (uint16_t)((dwell10 > 0xFFFFU) ? 0xFFFFU : dwell10);
    ev.depth     = depth;

    uint32_t     guard  = irqGuardEnter();
    laneStatus_e status = eventFifoPush(&senseEvents, &ev);
    irqGuardExit(guard);

    laneTrace(LANE_TRACE_EVENTS, "EV seq=%u %s flags=%x dwell=%uus depth=%u%s",
              (uint32_t)ev.seq, senseEventNames[(type <= 4U) ? type : 0U], (uint32_t)ev.flags,
              (uint32_t)dwellMb, (uint32_t)depth, (status == LANE_OK) ? "" : " DROPPED");
    laneTraceU64Line(LANE_TRACE_EVENTS, "   tEntry", entryMb);
}

static void senseHandleSample(const laneSample_t *s)
{
    uint64_t sampleMb = 0U;
    (void)laneTimerToMainboard(s->tLocal, &sampleMb);

    uint32_t guard = irqGuardEnter();
    senseSignal.raw      = s->raw;
    senseSignal.sampleUs = sampleMb;
    irqGuardExit(guard);

    senseFaultReported = false;

    if (LANE_TRACE_ON(LANE_TRACE_SAMPLES)) {
        laneTrace(LANE_TRACE_SAMPLES, "s %u %u", s->tLocal, (uint32_t)s->raw);
    }

    if (senseMode == LANE_SAMPLING_OFF) {
        return;   /* single sample: SIGNAL only */
    }

    laneDetectEvent_t e;
    if (laneDetectSample(&senseDetect, s->tLocal, s->raw, &e)) {
        senseManagerPushEvent(e.type, e.flags, e.tEntry, e.dwellUs, e.depth);
    }

    if (senseMode == LANE_SAMPLING_RAW) {
        if (senseRawCountdown <= 1U) {
            uint32_t decim = senseConfig.rawDecimation;
            senseRawCountdown = decim;
            uint32_t guardRaw = irqGuardEnter();
            rawFifoPush(&senseRaw, sampleMb, s->periodUs * decim, s->raw);
            irqGuardExit(guardRaw);
        } else {
            senseRawCountdown--;
        }
    }
}

static void senseUpdateSignal(uint32_t nowMs)
{
    uint32_t guard = irqGuardEnter();
    senseSignal.baseline = laneDetectBaseline(&senseDetect);
    senseSignal.thrLow   = senseDetect.thrLow;
    senseSignal.thrHigh  = senseDetect.thrHigh;
    senseSignal.noise    = laneDetectNoise(&senseDetect);
    irqGuardExit(guard);

    if (LANE_TRACE_ON(LANE_TRACE_SIGNAL) && senseMode != LANE_SAMPLING_OFF
        && (uint32_t)(nowMs - senseSignalTraceMs) >= 100U) {
        senseSignalTraceMs = nowMs;
        laneTrace(LANE_TRACE_SIGNAL, "SIG raw=%u base=%u low=%u high=%u noise=%u st=%u",
                  (uint32_t)senseSignal.raw, (uint32_t)senseSignal.baseline,
                  (uint32_t)senseSignal.thrLow, (uint32_t)senseSignal.thrHigh,
                  (uint32_t)senseSignal.noise, (uint32_t)senseDetect.state);
    }
}

laneStatus_e senseManagerRun(uint32_t nowMs)
{
    laneStatus_e result = vcnl4040Run(nowMs);

    bool wantActive = !senseParked && (senseMode != LANE_SAMPLING_OFF || senseSinglePending);
    if (wantActive) {
        vcnl4040Settings_t s = senseSensorSettings();
        (void)vcnl4040Activate(&s);
    } else {
        vcnl4040Shutdown();
    }

    /* Sensor failing under the sampler: recover, report once. */
    if (vcnl4040SampleErrorRun() >= VCNL4040_SAMPLE_ERRORS_MAX
        || (wantActive && vcnl4040State() == VCNL4040_STATE_FAULT)) {
        if (laneSamplerRunning()) {
            laneSamplerStop();
            vcnl4040RequestRecovery();
        }
        if (!senseFaultReported) {
            senseFaultReported = true;
            senseManagerPushEvent(LANE_EVT_SENSOR_FAULT, 0U, laneTimerNow(), 0U, 0U);
            result = LANE_ERR_SENSOR_FAULT;
        }
    }

    bool active = vcnl4040Settled();

    if (senseMode != LANE_SAMPLING_OFF) {
        if (active && !laneSamplerRunning()) {
            senseHoldoffRate = false;
            laneDetectBreak(&senseDetect);
            laneStatus_e s = laneSamplerStart(senseCurrentPeriod(), senseConfig.samplePhaseUs,
                                              senseConversionUs(), false);
            if (s != LANE_OK && result == LANE_OK) {
                result = s;
            }
        }
    } else if (senseSinglePending) {
        if (active && !senseSingleStarted) {
            senseSingleStarted = laneSamplerStart(senseConfig.samplePeriodUs, 0U,
                                                  senseConversionUs(), true) == LANE_OK;
        } else if (senseSingleStarted && !laneSamplerRunning()) {
            senseSinglePending = false;
            senseSingleStarted = false;
        }
    } else if (laneSamplerRunning()) {
        laneSamplerStop();
    }

    laneSample_t sample;
    while (laneSamplerPop(&sample)) {
        senseHandleSample(&sample);
    }

    /* Holdoff watch rate. */
    if (senseMode != LANE_SAMPLING_OFF && senseConfig.holdoffSamplePeriodUs != 0U) {
        bool slow = laneDetectInHoldoff(&senseDetect, laneTimerNow());
        if (slow != senseHoldoffRate) {
            senseHoldoffRate = slow;
            laneSamplerSetPeriod(senseCurrentPeriod());
            laneTrace(LANE_TRACE_EVENTS, "holdoff rate %s (%u us)", slow ? "on" : "off",
                      senseCurrentPeriod());
        }
    }

    senseUpdateSignal(nowMs);
    return result;
}

laneStatus_e senseManagerApplyConfig(const laneConfig_t *cfg)
{
    laneDetectConfig_t dc;

    senseDetectConfig(cfg, &dc);
    LANE_RETURN_IF_ERR(laneDetectSetConfig(&senseDetect, &dc));

    /* Stopped here, restarted by senseManagerRun once the sensor carries the
     * new codes: the sampler never runs on half-applied settings. */
    laneSamplerStop();
    senseConfig        = *cfg;
    senseConfigWritten = true;
    senseHoldoffRate   = false;
    senseRawCountdown  = 0U;

    laneTrace(LANE_TRACE_EVENTS, "CONFIG period=%u phase=%u merge=%u block=%u holdoff=%u",
              cfg->samplePeriodUs, cfg->samplePhaseUs, cfg->mergeGapUs, cfg->blockTimeoutUs,
              cfg->holdoffUs);
    return LANE_OK;
}

void senseManagerGetConfig(laneConfig_t *out)
{
    *out = senseConfig;
}

bool senseManagerConfigWritten(void)
{
    return senseConfigWritten;
}

void senseManagerResetConfig(void)
{
    laneDetectConfig_t dc;

    (void)senseManagerSetMode(LANE_SAMPLING_OFF);
    laneRegsDefaultConfig(&senseConfig);
    senseDetectConfig(&senseConfig, &dc);
    (void)laneDetectSetConfig(&senseDetect, &dc);
    senseConfigWritten = false;
}

laneStatus_e senseManagerSetMode(laneSamplingMode_e mode)
{
    if (senseParked && mode != LANE_SAMPLING_OFF) {
        return LANE_ERR_PARKED;
    }
    if (mode == senseMode) {
        return LANE_OK;
    }

    if (mode == LANE_SAMPLING_OFF) {
        laneSamplerStop();
    }
    if (senseMode == LANE_SAMPLING_RAW || mode == LANE_SAMPLING_RAW) {
        uint32_t guard = irqGuardEnter();
        rawFifoFlush(&senseRaw);
        irqGuardExit(guard);
        senseRawCountdown = 0U;
    }

    laneTrace(LANE_TRACE_EVENTS, "sampling %u -> %u", (uint32_t)senseMode, (uint32_t)mode);
    senseMode = mode;
    return LANE_OK;
}

laneSamplingMode_e senseManagerMode(void)
{
    return senseMode;
}

laneStatus_e senseManagerCalibrate(void)
{
    if (senseParked) {
        return LANE_ERR_PARKED;
    }
    laneDetectCalibrate(&senseDetect);
    laneTrace(LANE_TRACE_EVENTS, "calibrate");
    return LANE_OK;
}

laneStatus_e senseManagerSingleSample(void)
{
    if (senseParked) {
        return LANE_ERR_PARKED;
    }
    if (senseMode == LANE_SAMPLING_OFF) {
        senseSinglePending = true;
        senseSingleStarted = false;
    }
    /* While sampling, SIGNAL already carries the latest sample. */
    return LANE_OK;
}

void senseManagerPark(void)
{
    (void)senseManagerSetMode(LANE_SAMPLING_OFF);
    senseSinglePending = false;
    senseParked        = true;
}

uint16_t senseManagerStatusFlags(void)
{
    uint16_t flags = 0U;

    if (vcnl4040IsOk()) {
        flags |= LANE_STATUS_SENSOR_OK;
    }
    if (senseConfigWritten) {
        flags |= LANE_STATUS_CONFIG_VALID;
    }
    if (laneDetectIsCalibrated(&senseDetect) && senseMode != LANE_SAMPLING_OFF) {
        flags |= LANE_STATUS_CALIBRATED;
    }
    if (senseMode != LANE_SAMPLING_OFF && laneDetectInHoldoff(&senseDetect, laneTimerNow())) {
        flags |= LANE_STATUS_IN_HOLDOFF;
    }
    if (senseMode != LANE_SAMPLING_OFF && laneDetectIsBlocked(&senseDetect)) {
        flags |= LANE_STATUS_BLOCKED;
    }
    if (eventFifoOverflowed(&senseEvents)) {
        flags |= LANE_STATUS_FIFO_OVERFLOW;
    }
    return flags;
}

void senseManagerGetSignal(laneSignal_t *out)
{
    uint32_t guard = irqGuardEnter();
    *out = senseSignal;
    irqGuardExit(guard);
}

eventFifo_t *senseManagerEventFifo(void)
{
    return &senseEvents;
}

rawFifo_t *senseManagerRawFifo(void)
{
    return &senseRaw;
}

void senseManagerClearCounters(void)
{
    uint32_t guard = irqGuardEnter();
    eventFifoClearCounters(&senseEvents);
    irqGuardExit(guard);
}
