/*
 * Panel LED driver for the control module: the two MCU-driven LEDs.
 *
 *   INDICATOR_LED_STATUS  D601, PB13, TIM8_CH2 PWM
 *   INDICATOR_LED_ACT     D602, PB8,  TIM1_CH1 PWM
 *
 * Both are 1 kHz PWM through 1 kΩ (CubeMX: prescaler 143, ARR 999, boot duty 0,
 * compare preload on). Init adopts the MX period untouched; the level is a
 * compiled default until someone sets one. The PWR and LANE LEDs are
 * hard-wired and the BLE LED belongs to the nRF.
 *
 * Port of the lane module's indicator_led, generalised to a table of
 * instances; the rules carry over unchanged:
 *   - Levels are perceptual (0..LED_LEVEL_MAX) through the gamma curve in
 *     led_pattern.
 *   - On/off is the channel plus counter, not the compare value. The level
 *     survives off/on.
 *   - Channel and counter are tracked apart, so a switch that failed halfway
 *     is finished by the retry instead of trapping the C5 HAL (which traps on
 *     starting a running unit or stopping an idle one).
 *   - TIM1 and TIM8 are advanced timers. HAL_TIM_OC_StartChannel() sets MOE
 *     and HAL_TIM_OC_StopChannel() clears it once the last channel is off.
 *
 * Owns: TIM1 and TIM8 and their pins. Nothing else may touch them; the pins
 *       are in alternate-function mode, so HAL_GPIO_WritePin() is ignored.
 * Context: every call is safe from an ISR (short irq_guard sections).
 * Low power: the timers stop with the core in Stop1; the pins hold their level.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ctrl_status.h"
#include "led_pattern.h"

typedef enum {
    INDICATOR_LED_STATUS = 0,
    INDICATOR_LED_ACT,
    INDICATOR_LED_COUNT
} indicatorLed_e;

/* Panel LEDs sit next to the driver's hands: a third of full brightness is
 * plenty and keeps a dark room comfortable. */
#define INDICATOR_LED_DEFAULT_LEVEL  350U

/*
 * Adopts the CubeMX PWM periods, programs the default level and leaves both
 * LEDs dark. Assumes mx_tim1_init() and mx_tim8_init() have run. Returns
 * CTRL_ERR_TIMER if a compare unit cannot be programmed.
 */
ctrlStatus_e indicatorLedInit(void);

/*
 * Sets the perceptual level, clamped to LED_LEVEL_MAX. Takes effect within one
 * PWM period and is safe while the LED is off (kept for the next enable).
 */
ctrlStatus_e indicatorLedSetLevel(indicatorLed_e led, uint16_t level);

/*
 * Lights the LED at its level (true) or turns it dark (false), cancelling any
 * blink. Returns CTRL_ERR_TIMER if the timer refuses the switch; the state is
 * then unchanged, so the next call retries.
 */
ctrlStatus_e indicatorLedSet(indicatorLed_e led, bool on);

/*
 * Starts a blink at the current level: periodMs per flash, onPermille of it
 * lit, count flashes (0 = until the next indicatorLedSet()). A finite blink
 * ends dark. See ledBlinkStart() for the limits.
 */
ctrlStatus_e indicatorLedBlink(indicatorLed_e led, uint32_t periodMs, uint16_t onPermille,
                               uint16_t count);

/*
 * Advances running blinks. Call once per super-loop pass with HAL_GetTick().
 */
ctrlStatus_e indicatorLedRun(uint32_t nowMs);

bool indicatorLedIsLit(indicatorLed_e led);
bool indicatorLedIsBlinking(indicatorLed_e led);
