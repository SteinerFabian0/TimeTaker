/*
 * Start-light LED on TIM1_CH1. See indicator_led.h for the ownership rules.
 */

#include "indicator_led.h"

#include <stdbool.h>
#include <stdint.h>

#include "stm32_hal.h"
#include "mx_tim1.h"
#include "lane_status.h"
#include "led_pattern.h"
#include "irq_guard.h"

static const hal_tim_channel_t indicatorLedChannel = HAL_TIM_CHANNEL_1;

/* Counts in one PWM period (ARR + 1), read back from MX at init. */
static uint32_t indicatorLedPeriodCounts;

/* Kept so a level set while dark still applies on the next enable. */
static uint16_t indicatorLedLevel;

/* Gate the start/stop calls: the new HAL traps on starting a running unit or
 * stopping an idle one. Tracked apart, so a switch that failed halfway (channel
 * done, counter refused) is finished by the retry instead of trapping on it. */
static bool indicatorLedChannelOn;
static bool indicatorLedCounterOn;

static ledBlink_t indicatorLedBlinkState;

static laneStatus_e indicatorLedWritePulse(uint16_t level)
{
    uint32_t pulse = ledPatternDutyCounts(level, indicatorLedPeriodCounts);

    /* Compare preload (enabled in mx_tim1_init) makes this glitch-free while
     * the PWM runs. */
    if (HAL_TIM_OC_SetCompareUnitPulse(mx_tim1_gethandle(),
                                       hal_tim_oc_channel_to_compare_unit(indicatorLedChannel),
                                       pulse) != HAL_OK) {
        return LANE_ERR_TIMER;
    }

    return LANE_OK;
}

/* Caller holds the irq guard. Each unit's flag only follows a HAL call that
 * took, so the next call retries exactly what is missing. */
static laneStatus_e indicatorLedOutput(bool on)
{
    hal_tim_handle_t *htim = mx_tim1_gethandle();

    if (on) {
        /* The channel start also sets MOE on TIM1; without it the pin stays
         * dark. */
        if (!indicatorLedChannelOn) {
            if (HAL_TIM_OC_StartChannel(htim, indicatorLedChannel) != HAL_OK) {
                return LANE_ERR_TIMER;
            }
            indicatorLedChannelOn = true;
        }
        if (!indicatorLedCounterOn) {
            if (HAL_TIM_Start(htim) != HAL_OK) {
                return LANE_ERR_TIMER;
            }
            indicatorLedCounterOn = true;
        }
    } else {
        /* Channel first: disabling the last channel drops the main output, which
         * parks the pin at once instead of freezing it wherever the counter
         * happened to stop. */
        if (indicatorLedChannelOn) {
            if (HAL_TIM_OC_StopChannel(htim, indicatorLedChannel) != HAL_OK) {
                return LANE_ERR_TIMER;
            }
            indicatorLedChannelOn = false;
        }
        if (indicatorLedCounterOn) {
            if (HAL_TIM_Stop(htim) != HAL_OK) {
                return LANE_ERR_TIMER;
            }
            indicatorLedCounterOn = false;
        }
    }

    return LANE_OK;
}

laneStatus_e indicatorLedInit(void)
{
    hal_tim_handle_t *htim = mx_tim1_gethandle();

    /* MX2 owns the period: adopt it, so the duty math follows any CubeMX
     * change of the PWM frequency. */
    indicatorLedPeriodCounts = HAL_TIM_GetPeriod(htim) + 1U;
    indicatorLedLevel        = INDICATOR_LED_DEFAULT_LEVEL;

    /* MX leaves the timer stopped, so the pin is already dark. */
    indicatorLedChannelOn = false;
    indicatorLedCounterOn = false;
    ledBlinkStop(&indicatorLedBlinkState);

    return indicatorLedWritePulse(indicatorLedLevel);
}

laneStatus_e indicatorLedSetLevel(uint16_t level)
{
    if (level > LED_LEVEL_MAX) {
        level = LED_LEVEL_MAX;
    }

    uint32_t guard = irqGuardEnter();
    indicatorLedLevel = level;
    laneStatus_e status = indicatorLedWritePulse(level);
    irqGuardExit(guard);

    return status;
}

laneStatus_e indicatorLedSet(bool on)
{
    uint32_t guard = irqGuardEnter();
    ledBlinkStop(&indicatorLedBlinkState);
    laneStatus_e status = indicatorLedOutput(on);
    irqGuardExit(guard);

    return status;
}

laneStatus_e indicatorLedToggle(void)
{
    uint32_t guard = irqGuardEnter();
    ledBlinkStop(&indicatorLedBlinkState);
    laneStatus_e status = indicatorLedOutput(!indicatorLedIsLit());
    irqGuardExit(guard);

    return status;
}

laneStatus_e indicatorLedBlink(uint32_t periodMs, uint16_t onPermille, uint16_t count)
{
    uint32_t     nowMs = HAL_GetTick();
    uint32_t     guard = irqGuardEnter();
    laneStatus_e status = ledBlinkStart(&indicatorLedBlinkState, nowMs, periodMs,
                                        onPermille, count);
    if (status == LANE_OK) {
        status = indicatorLedOutput(ledBlinkIsOn(&indicatorLedBlinkState, nowMs));
    }
    irqGuardExit(guard);

    return status;
}

laneStatus_e indicatorLedRun(uint32_t nowMs)
{
    laneStatus_e status = LANE_OK;

    uint32_t guard = irqGuardEnter();
    if (indicatorLedBlinkState.active) {
        /* A finite blink deactivates itself here and reports dark, so the LED
         * ends off. */
        status = indicatorLedOutput(ledBlinkIsOn(&indicatorLedBlinkState, nowMs));
    }
    irqGuardExit(guard);

    return status;
}

bool indicatorLedIsLit(void)
{
    return indicatorLedChannelOn && indicatorLedCounterOn;
}

bool indicatorLedIsBlinking(void)
{
    return indicatorLedBlinkState.active;
}
