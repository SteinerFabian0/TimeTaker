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
