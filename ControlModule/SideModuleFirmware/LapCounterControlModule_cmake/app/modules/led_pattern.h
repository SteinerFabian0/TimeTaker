/*
 * Pure LED logic: the gamma curve and the blink pattern clock.
 *
 * No hardware and no HAL calls, so it is host-tested (test/host_led). The
 * driver (indicator_led) owns the timers and asks this module what to output.
 *
 * Levels are perceptual, 0..LED_LEVEL_MAX: equal steps look like equal
 * brightness steps to the eye. The gamma curve turns a level into a duty.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ctrl_status.h"

#define LED_LEVEL_MAX            1000U

/* Blink limits. The period floor is two super-loop latencies with margin: the
 * blink clock is polled from ctrlRun(), so a shorter period would alias. The
 * ceiling keeps every product below in 32-bit range. */
#define LED_BLINK_PERIOD_MIN_MS  20U
#define LED_BLINK_PERIOD_MAX_MS  60000U

/*
 * Duty for a perceptual level, in counts of a PWM period of periodCounts.
 * Gamma 2.2, piecewise linear over 21 points. Monotonic; 0 maps to 0, levels
 * above LED_LEVEL_MAX clamp, and any level > 0 gets at least one count so a
 * dim "on" never renders as dark.
 */
uint32_t ledPatternDutyCounts(uint16_t level, uint32_t periodCounts);

typedef struct {
    uint32_t startMs;
    uint32_t periodMs;
    uint32_t onMs;
    uint16_t count;     /* flashes to show; 0 = until stopped */
    bool     active;
} ledBlink_t;

/*
 * Starts a blink at nowMs, lit first. onPermille is the lit share of each
 * period (1..999; 0 and 1000 are steady states, use off/on for those). count
 * is the number of flashes, 0 for endless. Returns CTRL_ERR_INVALID_ARG and
 * leaves the blink untouched on a bad parameter.
 */
ctrlStatus_e ledBlinkStart(ledBlink_t *blink, uint32_t nowMs, uint32_t periodMs,
                           uint16_t onPermille, uint16_t count);

void ledBlinkStop(ledBlink_t *blink);

/*
 * Whether the LED should be lit at nowMs. A finite blink deactivates itself
 * after its last flash and reports dark from then on. Wrap-safe over the
 * 32-bit ms tick.
 */
bool ledBlinkIsOn(ledBlink_t *blink, uint32_t nowMs);
