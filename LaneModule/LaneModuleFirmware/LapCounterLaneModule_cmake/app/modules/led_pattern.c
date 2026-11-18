/*
 * Pure LED logic. See led_pattern.h.
 */

#include "led_pattern.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* (i / 20)^2.2 in 0.01 % of full scale, i = 0..20, one point per 50 levels. */
#define LED_GAMMA_POINTS      21U
#define LED_GAMMA_STEP        (LED_LEVEL_MAX / (LED_GAMMA_POINTS - 1U))
#define LED_GAMMA_FULL_SCALE  10000U

static const uint16_t ledGammaTable[LED_GAMMA_POINTS] = {
       0,   14,   63,  154,  290,  474,  707,  993, 1332, 1726, 2176,
    2684, 3250, 3876, 4563, 5310, 6121, 6994, 7931, 8933, 10000,
};

/* (1 - cos(pi * i / 128)) / 2 in Q15, i = 0..128: one rise of the pulse. */
#define LED_PULSE_POINTS  128U
#define LED_PULSE_ONE_Q15 32768U

static const uint16_t ledPulseTable[LED_PULSE_POINTS + 1U] = {
    0, 5, 20, 44, 79, 123, 177, 241, 315, 398, 491, 593, 705, 827, 958, 1098, 1247, 1406,
    1573, 1749, 1935, 2128, 2331, 2542, 2761, 2989, 3224, 3468, 3719, 3978, 4244, 4518,
    4799, 5087, 5381, 5682, 5990, 6304, 6624, 6950, 7282, 7619, 7961, 8308, 8661, 9018,
    9379, 9745, 10114, 10487, 10864, 11245, 11628, 12014, 12403, 12794, 13188, 13583, 13980,
    14378, 14778, 15179, 15580, 15982, 16384, 16786, 17188, 17589, 17990, 18390, 18788,
    19185, 19580, 19974, 20365, 20754, 21140, 21523, 21904, 22281, 22654, 23023, 23389,
    23750, 24107, 24460, 24807, 25149, 25486, 25818, 26144, 26464, 26778, 27086, 27387,
    27681, 27969, 28250, 28524, 28790, 29049, 29300, 29544, 29779, 30007, 30226, 30437,
    30640, 30833, 31019, 31195, 31362, 31521, 31670, 31810, 31941, 32063, 32175, 32277,
    32370, 32453, 32527, 32591, 32645, 32689, 32724, 32748, 32763, 32768
};

bool ledPulseRiseValid(uint32_t riseMs)
{
    return riseMs >= LED_PULSE_RISE_MIN_MS && riseMs <= LED_PULSE_RISE_MAX_MS;
}

uint16_t ledPulseLevel(uint64_t tUs, uint32_t riseMs, uint16_t fullLevel)
{
    if (riseMs == 0U) {
        return 0U;
    }
    if (fullLevel > LED_LEVEL_MAX) {
        fullLevel = (uint16_t)LED_LEVEL_MAX;
    }

    const uint64_t riseUs = (uint64_t)riseMs * 1000U;
    uint64_t       pos    = tUs % (2U * riseUs);
    if (pos > riseUs) {
        pos = 2U * riseUs - pos;      /* falling half mirrors the rising half */
    }

    const uint64_t scaled = pos * LED_PULSE_POINTS;
    const uint32_t idx    = (uint32_t)(scaled / riseUs);
    uint32_t       q      = ledPulseTable[idx];
    if (idx < LED_PULSE_POINTS) {
        const uint32_t rem = (uint32_t)(scaled - (uint64_t)idx * riseUs);   /* < riseUs */
        q += (uint32_t)(((uint64_t)(ledPulseTable[idx + 1U] - q) * rem) / riseUs);
    }

    return (uint16_t)(((uint32_t)fullLevel * q + LED_PULSE_ONE_Q15 / 2U) / LED_PULSE_ONE_Q15);
}

uint32_t ledPatternDutyCounts(uint16_t level, uint32_t periodCounts)
{
    if (level == 0U) {
        return 0U;
    }
    if (level >= LED_LEVEL_MAX) {
        return periodCounts;
    }

    uint32_t seg  = level / LED_GAMMA_STEP;
    uint32_t frac = level % LED_GAMMA_STEP;
    uint32_t lo   = ledGammaTable[seg];
    uint32_t hi   = ledGammaTable[seg + 1U];
    uint32_t g    = lo + ((hi - lo) * frac) / LED_GAMMA_STEP;

    /* 64-bit: periodCounts can be a full 16-bit ARR + 1. */
    uint32_t counts = (uint32_t)(((uint64_t)g * periodCounts) / LED_GAMMA_FULL_SCALE);
    return (counts == 0U) ? 1U : counts;
}

laneStatus_e ledBlinkStart(ledBlink_t *blink, uint32_t nowMs, uint32_t periodMs,
                           uint16_t onPermille, uint16_t count)
{
    if (blink == NULL) {
        return LANE_ERR_INVALID_ARG;
    }
    if (periodMs < LED_BLINK_PERIOD_MIN_MS || periodMs > LED_BLINK_PERIOD_MAX_MS) {
        return LANE_ERR_INVALID_ARG;
    }
    if (onPermille == 0U || onPermille >= 1000U) {
        return LANE_ERR_INVALID_ARG;
    }

    uint32_t onMs = (periodMs * onPermille) / 1000U;
    /* A lit or dark phase shorter than one loop pass would be skipped. */
    if (onMs == 0U || onMs >= periodMs) {
        return LANE_ERR_INVALID_ARG;
    }

    blink->startMs  = nowMs;
    blink->periodMs = periodMs;
    blink->onMs     = onMs;
    blink->count    = count;
    blink->active   = true;
    return LANE_OK;
}

void ledBlinkStop(ledBlink_t *blink)
{
    blink->active = false;
}

bool ledBlinkIsOn(ledBlink_t *blink, uint32_t nowMs)
{
    if (!blink->active) {
        return false;
    }

    uint32_t elapsed = nowMs - blink->startMs;
    uint32_t cycle   = elapsed / blink->periodMs;

    if (blink->count != 0U && cycle >= blink->count) {
        blink->active = false;
        return false;
    }

    return (elapsed % blink->periodMs) < blink->onMs;
}
