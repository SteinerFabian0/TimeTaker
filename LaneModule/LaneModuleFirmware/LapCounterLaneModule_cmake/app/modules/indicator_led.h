/*
 * Start-light LED driver for the lane module.
 *
 * Owns TIM1 channel 1 (PB8, LED_PWM), which drives the red 5 mm LED above the
 * lane through 82 Ω as a 1 kHz PWM. CubeMX (mx_tim1_init) is authoritative for
 * the PWM frequency: init adopts the MX-programmed period untouched (prescaler
 * 143, ARR 999, so 1000 counts). The CubeMX boot duty is 0 and is not adopted
 * as a brightness: the LED level is a compiled default until the mainboard
 * sets one.
 *
 * Levels are perceptual (0..LED_LEVEL_MAX) and go through the gamma curve in
 * led_pattern. On/off is the channel plus counter, not the compare value —
 * stopping the channel parks the pin dark at once (TIM1 also drops MOE once no
 * channel is left, see below), and the level survives across off/on.
 *
 * Nothing else may touch TIM1 or PB8: the pin is in alternate-function mode,
 * so a HAL_GPIO_WritePin() on it is silently ignored.
 *
 * TIM1 is an advanced timer: its outputs only drive with MOE set.
 * HAL_TIM_OC_StartChannel() sets MOE (LL_TIM_EnableAllOutputs) on every
 * break-capable instance, and HAL_TIM_OC_StopChannel() clears it once the last
 * channel is off; this driver relies on both.
 *
 * Context: indicatorLedSet() is safe to call from the TIM2 ISR, so a
 * scheduled action (start-light "off at tick T") switches the LED within the
 * ISR latency, without waiting for the next PWM period. Every call here takes
 * a short irq_guard section for that reason.
 *
 * Low power: TIM1 stops with the core in Stop1 and the pin holds whatever
 * level the PWM had. sys_power turns the LED off before any Stop entry.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lane_status.h"
#include "led_pattern.h"

/* The start light's job is to be seen; the mainboard dims it if it wants. */
#define INDICATOR_LED_DEFAULT_LEVEL  LED_LEVEL_MAX

/*
 * Adopts the CubeMX PWM period, programs the default level and leaves the LED
 * dark. Assumes mx_tim1_init() has already run. Returns LANE_ERR_TIMER if the
 * compare unit cannot be programmed.
 */
laneStatus_e indicatorLedInit(void);

/*
 * Sets the perceptual level, clamped to LED_LEVEL_MAX. Takes effect within one
 * PWM period (compare preload) and is safe to call while the LED is off — the
 * value is kept for the next enable. Level 0 keeps the LED dark while "on".
 */
laneStatus_e indicatorLedSetLevel(uint16_t level);

/*
 * Lights the LED at the current level (true) or turns it dark (false), and
 * cancels any running blink. Safe to call repeatedly in either state and from
 * the TIM2 ISR. Returns LANE_ERR_TIMER if the timer refuses the switch; the
 * state is then unchanged, so the next call retries.
 */
laneStatus_e indicatorLedSet(bool on);

/*
 * Flips between lit and dark (cancelling any blink). Level is unchanged.
 */
laneStatus_e indicatorLedToggle(void);

/*
 * Starts a blink at the current level: periodMs per flash, onPermille of it
 * lit, count flashes (0 = until the next indicatorLedSet()). A finite blink
 * ends dark. See ledBlinkStart() for the limits; on a bad parameter returns
 * LANE_ERR_INVALID_ARG and leaves the LED as it was.
 */
laneStatus_e indicatorLedBlink(uint32_t periodMs, uint16_t onPermille, uint16_t count);

/*
 * Advances a running blink. Call once per super-loop pass with HAL_GetTick().
 */
laneStatus_e indicatorLedRun(uint32_t nowMs);

/*
 * Starts the breathing pulse (LANE_ACT_LED_PULSE): rise time riseMs
 * (ledPulseRiseValid), the first frame rendered at tUs microseconds into the
 * pulse. The pulse peaks at the current level (indicatorLedSetLevel). It keeps
 * running until the next indicatorLedSet / Toggle / Blink, which ends it, and
 * only advances when indicatorLedPulseRender() is called. Safe from the TIM2
 * ISR. LANE_ERR_INVALID_ARG on a bad rise time (LED unchanged).
 */
laneStatus_e indicatorLedPulseStart(uint32_t riseMs, uint64_t tUs);

/* Renders the pulse at tUs since its start (no-op when none runs). Call about
 * every 10 ms. */
laneStatus_e indicatorLedPulseRender(uint64_t tUs);

/* Whether a pulse is running. */
bool indicatorLedPulseActive(void);

/* Whether the LED is currently driven (lit phase of a blink included). False
 * while a switch-on failed halfway; the next call completes it. */
bool indicatorLedIsLit(void);

/* Whether a blink is running. */
bool indicatorLedIsBlinking(void);
