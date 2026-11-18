/*
 * Pure LED logic: the gamma curve and the blink pattern clock.
 *
 * No hardware and no HAL calls, so it is host-tested (test/host_led). The
 * driver (indicator_led) owns TIM1 and asks this module what to output.
 *
 * Levels are perceptual, 0..LED_LEVEL_MAX: equal steps look like equal
 * brightness steps to the eye. The gamma curve turns a level into a duty.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lane_status.h"

#define LED_LEVEL_MAX            1000U

/* Blink limits. The period floor is two super-loop latencies with margin: the
 * blink clock is polled from laneRun(), so a shorter period would alias. The
 * ceiling keeps every product below in 32-bit range. */
#define LED_BLINK_PERIOD_MIN_MS  20U
#define LED_BLINK_PERIOD_MAX_MS  60000U

/* Pulse ("attention breathing") rise time limits, LANE_ACT_LED_PULSE param in
 * ms. The floor keeps a rise longer than a few 10 ms render steps; the ceiling
 * keeps the 64-bit phase maths trivially in range. 0 is rejected. */
#define LED_PULSE_RISE_MIN_MS    20U
#define LED_PULSE_RISE_MAX_MS    60000U

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
 * is the number of flashes, 0 for endless. Returns LANE_ERR_INVALID_ARG and
 * leaves the blink untouched on a bad parameter.
 */
laneStatus_e ledBlinkStart(ledBlink_t *blink, uint32_t nowMs, uint32_t periodMs,
                           uint16_t onPermille, uint16_t count);

void ledBlinkStop(ledBlink_t *blink);

/*
 * Whether the LED should be lit at nowMs. A finite blink deactivates itself
 * after its last flash and reports dark from then on. Wrap-safe over the
 * 32-bit ms tick.
 */
bool ledBlinkIsOn(ledBlink_t *blink, uint32_t nowMs);

/* Whether riseMs is a valid pulse rise time (LED_PULSE_RISE_MIN_MS..MAX_MS). */
bool ledPulseRiseValid(uint32_t riseMs);

/*
 * Perceptual level of the breathing pulse tUs microseconds after it started:
 *
 *     level = fullLevel * (1 - cos(pi * t / rise)) / 2     (rise = riseMs)
 *
 * a triangle-wave phase folded onto one raised cosine: dark at t = 0, full
 * level at t = rise, dark again at t = 2 * rise, repeating with period
 * 2 * rise forever (any tUs, no wrap issue). Integer only, no FPU: a 129-point
 * table of the half cosine in Q15 with linear interpolation (error < 1 count
 * of 32768, far below one of the 1000 levels), so the result is deterministic
 * and identical on every lane for the same tUs. The caller feeds it the
 * shared system time, which is what keeps six lanes breathing in phase.
 *
 * The result is a perceptual level (it goes through the gamma like any other).
 * fullLevel is clamped to LED_LEVEL_MAX; riseMs of 0 (invalid) returns 0.
 */
uint16_t ledPulseLevel(uint64_t tUs, uint32_t riseMs, uint16_t fullLevel);
