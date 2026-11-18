/*
 * Panel LEDs on TIM8_CH2 and TIM1_CH1. See indicator_led.h for the rules.
 */

#include "indicator_led.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "stm32_hal.h"
#include "mx_tim1.h"
#include "mx_tim8.h"
#include "ctrl_status.h"
#include "led_pattern.h"
#include "irq_guard.h"

typedef struct {
    hal_tim_handle_t *(*handle)(void);
    hal_tim_channel_t channel;
} indicatorLedHw_t;

static const indicatorLedHw_t indicatorLedHw[INDICATOR_LED_COUNT] = {
    [INDICATOR_LED_STATUS] = { mx_tim8_gethandle, HAL_TIM_CHANNEL_2 },
    [INDICATOR_LED_ACT]    = { mx_tim1_gethandle, HAL_TIM_CHANNEL_1 },
};

typedef struct {
    uint32_t   periodCounts;    /* ARR + 1, read back from MX at init */
    uint16_t   level;           /* kept so a level set while dark applies on enable */
    bool       channelOn;
    bool       counterOn;
    bool       settle;          /* a finished blink's dark switch failed: retry */
    ledBlink_t blink;
} indicatorLedState_t;

static indicatorLedState_t indicatorLedState[INDICATOR_LED_COUNT];

static bool indicatorLedValid(indicatorLed_e led)
{
    return (unsigned)led < (unsigned)INDICATOR_LED_COUNT;
}

static ctrlStatus_e indicatorLedWritePulse(indicatorLed_e led)
{
    const indicatorLedHw_t *hw = &indicatorLedHw[led];
    indicatorLedState_t    *st = &indicatorLedState[led];
    uint32_t pulse = ledPatternDutyCounts(st->level, st->periodCounts);

    /* Compare preload (enabled in MX) makes this glitch-free while running. */
    if (HAL_TIM_OC_SetCompareUnitPulse(hw->handle(),
                                       hal_tim_oc_channel_to_compare_unit(hw->channel),
                                       pulse) != HAL_OK) {
        return CTRL_ERR_TIMER;
    }
    return CTRL_OK;
}

/* Caller holds the irq guard. Each flag only follows a HAL call that took. */
static ctrlStatus_e indicatorLedOutput(indicatorLed_e led, bool on)
{
    const indicatorLedHw_t *hw   = &indicatorLedHw[led];
    indicatorLedState_t    *st   = &indicatorLedState[led];
    hal_tim_handle_t       *htim = hw->handle();

    if (on) {
        if (!st->channelOn) {
            if (HAL_TIM_OC_StartChannel(htim, hw->channel) != HAL_OK) {
                return CTRL_ERR_TIMER;
            }
            st->channelOn = true;
        }
        if (!st->counterOn) {
            if (HAL_TIM_Start(htim) != HAL_OK) {
                return CTRL_ERR_TIMER;
            }
            st->counterOn = true;
        }
    } else {
        /* Channel first: dropping the last channel clears MOE and parks the pin
         * at once instead of wherever the counter happened to stop. */
        if (st->channelOn) {
            if (HAL_TIM_OC_StopChannel(htim, hw->channel) != HAL_OK) {
                return CTRL_ERR_TIMER;
            }
            st->channelOn = false;
        }
        if (st->counterOn) {
            if (HAL_TIM_Stop(htim) != HAL_OK) {
                return CTRL_ERR_TIMER;
            }
            st->counterOn = false;
        }
    }
    return CTRL_OK;
}

ctrlStatus_e indicatorLedInit(void)
{
    ctrlStatus_e result = CTRL_OK;

    for (int i = 0; i < (int)INDICATOR_LED_COUNT; i++) {
        indicatorLed_e       led = (indicatorLed_e)i;
        indicatorLedState_t *st  = &indicatorLedState[led];

        /* MX owns the period: adopt it, so the duty math follows any CubeMX
         * change of the PWM frequency. MX leaves the timers stopped. */
        st->periodCounts = HAL_TIM_GetPeriod(indicatorLedHw[led].handle()) + 1U;
        st->level        = INDICATOR_LED_DEFAULT_LEVEL;
        st->channelOn    = false;
        st->counterOn    = false;
        ledBlinkStop(&st->blink);

        ctrlStatus_e status = indicatorLedWritePulse(led);
        if (status != CTRL_OK) {
            result = status;
        }
    }
    return result;
}

ctrlStatus_e indicatorLedSetLevel(indicatorLed_e led, uint16_t level)
{
    if (!indicatorLedValid(led)) {
        return CTRL_ERR_INVALID_ARG;
    }
    if (level > LED_LEVEL_MAX) {
        level = LED_LEVEL_MAX;
    }

    uint32_t guard = irqGuardEnter();
    indicatorLedState[led].level = level;
    ctrlStatus_e status = indicatorLedWritePulse(led);
    irqGuardExit(guard);
    return status;
}

ctrlStatus_e indicatorLedSet(indicatorLed_e led, bool on)
{
    if (!indicatorLedValid(led)) {
        return CTRL_ERR_INVALID_ARG;
    }

    uint32_t guard = irqGuardEnter();
    ledBlinkStop(&indicatorLedState[led].blink);
    indicatorLedState[led].settle = false;
    ctrlStatus_e status = indicatorLedOutput(led, on);
    irqGuardExit(guard);
    return status;
}

ctrlStatus_e indicatorLedBlink(indicatorLed_e led, uint32_t periodMs, uint16_t onPermille,
                               uint16_t count)
{
    if (!indicatorLedValid(led)) {
        return CTRL_ERR_INVALID_ARG;
    }

    uint32_t     nowMs  = HAL_GetTick();
    uint32_t     guard  = irqGuardEnter();
    ledBlink_t  *blink  = &indicatorLedState[led].blink;
    ctrlStatus_e status = ledBlinkStart(blink, nowMs, periodMs, onPermille, count);
    if (status == CTRL_OK) {
        indicatorLedState[led].settle = false;
        status = indicatorLedOutput(led, ledBlinkIsOn(blink, nowMs));
    }
    irqGuardExit(guard);
    return status;
}

ctrlStatus_e indicatorLedRun(uint32_t nowMs)
{
    ctrlStatus_e result = CTRL_OK;

    for (int i = 0; i < (int)INDICATOR_LED_COUNT; i++) {
        indicatorLed_e led   = (indicatorLed_e)i;
        uint32_t       guard = irqGuardEnter();
        indicatorLedState_t *st = &indicatorLedState[led];
        if (st->blink.active || st->settle) {
            /* A finite blink deactivates itself here and reports dark. If
             * that last switch fails, `settle` keeps retrying it: no blink is
             * left to do so, and the LED would stay lit. */
            ctrlStatus_e status = indicatorLedOutput(led, ledBlinkIsOn(&st->blink, nowMs));
            st->settle = (status != CTRL_OK) && !st->blink.active;
            if (status != CTRL_OK) {
                result = status;
            }
        }
        irqGuardExit(guard);
    }
    return result;
}

bool indicatorLedIsLit(indicatorLed_e led)
{
    return indicatorLedValid(led) && indicatorLedState[led].channelOn && indicatorLedState[led].counterOn;
}

bool indicatorLedIsBlinking(indicatorLed_e led)
{
    return indicatorLedValid(led) && indicatorLedState[led].blink.active;
}
