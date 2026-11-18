/*
 * Sampling schedule. See lane_sampler.h.
 */

#include "lane_sampler.h"
#include "lane_behavior.h"

#include <stdbool.h>
#include <stdint.h>

#include "stm32_hal.h"
#include "mx_gpio_default.h"
#include "lane_regs.h"
#include "lane_timer.h"
#include "sync_time.h"
#include "vcnl4040.h"
#include "irq_guard.h"

typedef enum {
    SAMPLER_PHASE_TRIGGER = 0,   /* next alarm fires the trigger    */
    SAMPLER_PHASE_READ,          /* next alarm reads the result     */
} samplerPhase_e;

static volatile bool     samplerRunning;
static volatile uint32_t samplerGen;       /* bumps on start/stop: stale ops are ignored */
static uint32_t          samplerOpGen;
static samplerPhase_e    samplerPhase;
static bool              samplerOneShot;

static uint32_t samplerPeriodUs;
static uint32_t samplerPhaseUs;
static uint32_t samplerConvUs;
static uint32_t samplerLatencyUs;

/* Grid state. */
static bool     samplerGridValid;
static bool     samplerGridAligned;       /* grid on the mainboard axis */
static uint32_t samplerGridPeriod;
static uint64_t samplerNextMb;
static uint32_t samplerNextLocal;
static uint32_t samplerTriggerLocal;

static laneSample_t      samplerRing[LANE_SAMPLER_RING];
static volatile uint32_t samplerHead;     /* written by the consumer */
static volatile uint32_t samplerTail;     /* written by the producer */

static volatile uint32_t samplerCount;
static volatile uint32_t samplerOverruns;
static volatile uint32_t samplerErrors;
static volatile uint32_t samplerDrops;

static void samplerAlarm(uint32_t dueLocal, uint32_t firedLocal);

static void samplerMarker(bool active)
{
#if LANE_ENABLE_SCOPE_MARKER
    HAL_GPIO_WritePin(CS_PORT, CS_PIN, active ? CS_ACTIVE_STATE : CS_INACTIVE_STATE);
#else
    (void)active;
#endif
}

/* Whether the mainboard axis can be used for the grid right now. */
static bool samplerAxisAligned(uint32_t nowLocal, uint64_t *nowMb)
{
    uint8_t flags = laneTimerToMainboard(nowLocal, nowMb);
    return (flags & (SYNC_TIME_FLAG_NO_LOCK | SYNC_TIME_FLAG_UNALIGNED | SYNC_TIME_FLAG_EXTRAPOLATED)) == 0U;
}

/* Picks the next due instant on the grid; ISR or main loop. */
static void samplerScheduleNext(void)
{
    uint32_t now    = laneTimerNow();
    uint64_t nowMb  = 0U;
    bool     align  = samplerAxisAligned(now, &nowMb);
    bool     regrid = !samplerGridValid || align != samplerGridAligned
                      || samplerGridPeriod != samplerPeriodUs;

    if (align) {
        uint32_t local = 0U;
        if (!regrid) {
            samplerNextMb += samplerPeriodUs;
            regrid = laneTimerToLocal(samplerNextMb, &local) != LANE_OK
                     || (int32_t)(local - now) < 0;   /* fell behind: skip ahead */
        }
        if (regrid) {
            uint64_t k = (nowMb + LANE_SAMPLER_START_LEAD_US) / samplerPeriodUs + 1U;
            samplerNextMb = k * samplerPeriodUs + samplerPhaseUs;
            if (laneTimerToLocal(samplerNextMb, &local) != LANE_OK) {
                local = now + LANE_SAMPLER_START_LEAD_US;
            }
        }
        samplerNextLocal = local;
    } else {
        if (!regrid) {
            samplerNextLocal += samplerPeriodUs;
            regrid = (int32_t)(samplerNextLocal - now) < 0;
        }
        if (regrid) {
            samplerNextLocal = now + LANE_SAMPLER_START_LEAD_US + samplerPhaseUs;
        }
    }

    samplerGridValid   = true;
    samplerGridAligned = align;
    samplerGridPeriod  = samplerPeriodUs;
    samplerPhase       = SAMPLER_PHASE_TRIGGER;

    if (laneTimerArmAlarm(LANE_TIMER_ALARM_SAMPLE, samplerNextLocal, samplerAlarm) != LANE_OK) {
        samplerErrors++;
        samplerRunning = false;
    }
}

static void samplerPush(uint32_t tLocal, uint16_t raw)
{
    uint32_t tail = samplerTail;
    uint32_t next = (tail + 1U) % LANE_SAMPLER_RING;

    if (next == samplerHead) {
        samplerDrops++;
        return;
    }
    samplerRing[tail].tLocal   = tLocal;
    samplerRing[tail].raw      = raw;
    samplerRing[tail].periodUs = samplerPeriodUs;
    __DMB();
    samplerTail = next;
}

/* Ends the current sample (any outcome) and moves on. */
static void samplerFinish(void)
{
    samplerMarker(false);
    if (samplerOneShot) {
        samplerRunning = false;
        samplerGen++;
        return;
    }
    samplerScheduleNext();
}

static void samplerOnRead(laneStatus_e status, uint16_t value)
{
    if (samplerOpGen != samplerGen || !samplerRunning) {
        samplerMarker(false);
        return;
    }
    if (status == LANE_OK) {
        samplerCount++;
        samplerPush(samplerTriggerLocal + samplerLatencyUs, value);
    } else {
        samplerErrors++;
    }
    samplerFinish();
}

static void samplerOnTrigger(laneStatus_e status, uint16_t value)
{
    (void)value;
    if (samplerOpGen != samplerGen || !samplerRunning) {
        samplerMarker(false);
        return;
    }
    if (status != LANE_OK) {
        samplerErrors++;
        samplerFinish();
        return;
    }

    samplerPhase = SAMPLER_PHASE_READ;
    if (laneTimerArmAlarm(LANE_TIMER_ALARM_SAMPLE, samplerTriggerLocal + samplerConvUs,
                          samplerAlarm) != LANE_OK) {
        samplerErrors++;
        samplerFinish();
    }
}

/* TIM2 ISR, priority 1. */
static void samplerAlarm(uint32_t dueLocal, uint32_t firedLocal)
{
    (void)firedLocal;
    if (!samplerRunning) {
        return;
    }

    samplerOpGen = samplerGen;

    if (samplerPhase == SAMPLER_PHASE_TRIGGER) {
        samplerTriggerLocal = dueLocal;
        samplerMarker(true);
        laneStatus_e s = vcnl4040Trigger(samplerOnTrigger);
        if (s != LANE_OK) {
            if (s == LANE_ERR_I2C_BUSY) {
                samplerOverruns++;
            } else {
                samplerErrors++;
            }
            samplerMarker(false);
            samplerScheduleNext();
        }
    } else {
        laneStatus_e s = vcnl4040ReadPs(samplerOnRead);
        if (s != LANE_OK) {
            samplerErrors++;
            samplerFinish();
        }
    }
}

laneStatus_e laneSamplerInit(void)
{
    samplerRunning = false;
    samplerHead    = 0U;
    samplerTail    = 0U;
    samplerMarker(false);
    return LANE_OK;
}

laneStatus_e laneSamplerStart(uint32_t periodUs, uint32_t phaseUs, uint32_t convUs, bool oneShot)
{
    if (periodUs == 0U || phaseUs >= periodUs) {
        return LANE_ERR_INVALID_ARG;
    }
    if (vcnl4040State() != VCNL4040_STATE_ACTIVE) {
        return LANE_ERR_SENSOR_NOT_READY;
    }

    laneSamplerStop();

    int32_t latency = (int32_t)(LANE_REGS_I2C_TRIGGER_US + convUs / 2U) + LANE_SAMPLER_LATENCY_TRIM_US;

    uint32_t guard = irqGuardEnter();
    samplerPeriodUs  = periodUs;
    samplerPhaseUs   = phaseUs;
    samplerConvUs    = convUs;
    samplerLatencyUs = (latency > 0) ? (uint32_t)latency : 0U;
    samplerOneShot   = oneShot;
    samplerGridValid = false;
    samplerGen++;
    samplerRunning   = true;
    samplerScheduleNext();
    irqGuardExit(guard);

    return samplerRunning ? LANE_OK : LANE_ERR_TIMER;
}

void laneSamplerStop(void)
{
    uint32_t guard = irqGuardEnter();
    samplerRunning = false;
    samplerGen++;
    laneTimerCancelAlarm(LANE_TIMER_ALARM_SAMPLE);
    irqGuardExit(guard);
    samplerMarker(false);
}

void laneSamplerSetPeriod(uint32_t periodUs)
{
    if (periodUs == 0U || periodUs <= samplerPhaseUs) {
        return;
    }
    uint32_t guard = irqGuardEnter();
    samplerPeriodUs = periodUs;
    irqGuardExit(guard);
}

bool laneSamplerRunning(void)
{
    return samplerRunning;
}

uint32_t laneSamplerPeriod(void)
{
    return samplerPeriodUs;
}

bool laneSamplerPop(laneSample_t *out)
{
    uint32_t head = samplerHead;
    if (head == samplerTail) {
        return false;
    }
    __DMB();
    *out = samplerRing[head];
    samplerHead = (head + 1U) % LANE_SAMPLER_RING;
    return true;
}

uint32_t laneSamplerCount(void)
{
    return samplerCount;
}

uint32_t laneSamplerOverruns(void)
{
    return samplerOverruns;
}

uint32_t laneSamplerErrors(void)
{
    return samplerErrors;
}

uint32_t laneSamplerRingDrops(void)
{
    return samplerDrops;
}
