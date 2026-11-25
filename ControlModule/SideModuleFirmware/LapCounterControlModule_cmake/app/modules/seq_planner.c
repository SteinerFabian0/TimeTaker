/*
 * Start sequence planner. See seq_planner.h.
 */

#include "seq_planner.h"

#include <stdint.h>
#include <string.h>

#define SEQ_PLANNER_US_PER_MS  1000U

static void put(seqPlanLane_t *l, uint8_t action, uint16_t param, uint64_t atUs)
{
    l->act[l->count].action = action;
    l->act[l->count].param  = param;
    l->act[l->count].atUs   = atUs;
    l->count++;
}

ctrlStatus_e seqPlannerPlan(const seqPlanParams_t *params, uint8_t presentMask, uint64_t nowUs,
                            uint32_t randomWord, seqPlan_t *plan)
{
    if (params->lights == 0U || params->lights > SEQ_PLANNER_MAX_LIGHTS || params->stepMs == 0U
        || params->holdMinMs > params->holdMaxMs) {
        return CTRL_ERR_INVALID_ARG;
    }
    if (params->leadMs != 0U && params->leadMs < SEQ_PLANNER_MIN_LEAD_MS) {
        return CTRL_ERR_TOO_LATE;
    }
    uint8_t present[LANE_MAX_LANES];
    uint8_t presentCount = 0U;
    for (uint8_t i = 0U; i < LANE_MAX_LANES; i++) {
        if ((presentMask & (1U << i)) != 0U) {
            present[presentCount++] = (uint8_t)(i + 1U);
        }
    }
    if (presentCount == 0U) {
        return CTRL_ERR_LANE_ABSENT;
    }

    const uint8_t  n      = (presentCount < params->lights) ? presentCount : params->lights;
    const uint32_t leadMs = (params->leadMs == 0U) ? SEQ_PLANNER_MIN_LEAD_MS : params->leadMs;
    const uint32_t riseMs = (params->pulseRiseMs == 0U) ? SEQ_PLANNER_DEFAULT_PULSE_MS : params->pulseRiseMs;
    const uint8_t  k      = params->attentionPulses;

    /* Hold drawn in µs, so Tout is not even predictable to the millisecond.
     * The range fits u32: 65535 ms = 65 535 000 µs. Modulo bias < 2^-6. */
    const uint32_t holdMinUs = (uint32_t)params->holdMinMs * SEQ_PLANNER_US_PER_MS;
    const uint32_t spanUs    = ((uint32_t)params->holdMaxMs - params->holdMinMs) * SEQ_PLANNER_US_PER_MS + 1U;
    const uint32_t holdUs    = holdMinUs + (randomWord % spanUs);

    const uint64_t leadUs   = (uint64_t)leadMs * SEQ_PLANNER_US_PER_MS;
    const uint64_t riseUs   = (uint64_t)riseMs * SEQ_PLANNER_US_PER_MS;
    const uint64_t stepUs   = (uint64_t)params->stepMs * SEQ_PLANNER_US_PER_MS;
    const uint64_t pulsesUs = 2ULL * k * riseUs;   /* <= 510 * 65 535 000 µs */
    const uint64_t totalUs  = leadUs + pulsesUs + (uint64_t)(n - 1U) * stepUs + holdUs;
    if (nowUs > UINT64_MAX - totalUs) {
        return CTRL_ERR_INVALID_ARG;   /* the axis would wrap: never, unless the clock is broken */
    }

    memset(plan, 0, sizeof(*plan));
    plan->t0Us     = nowUs + leadUs;
    plan->litCount = n;
    plan->holdUs   = holdUs;
    plan->firstLane = present[0];
    const uint64_t tTrough = plan->t0Us + pulsesUs;          /* T0 + 2kP */
    plan->tLitUs = tTrough + (uint64_t)(n - 1U) * stepUs;
    plan->toutUs = plan->tLitUs + holdUs;

    for (uint8_t i = 0U; i < LANE_MAX_LANES; i++) {
        if ((presentMask & (1U << i)) != 0U && params->armSampling != 0U) {
            put(&plan->lane[i], LANE_ACT_SAMPLING_ON, 0U, plan->t0Us);
        }
    }
    for (uint8_t idx = 0U; idx < n; idx++) {
        const uint8_t  lane = present[idx];
        seqPlanLane_t *l    = &plan->lane[lane - 1U];
        const uint8_t  j    = idx;   /* 0 = first lane (start of the chain) */
        plan->litMask |= (uint8_t)(1U << (lane - 1U));
        if (k != 0U) {
            put(l, LANE_ACT_LED_PULSE, (uint16_t)riseMs, plan->t0Us);
            if (j == 0U) {
                put(l, LANE_ACT_LED_ON, 1000U, plan->t0Us + pulsesUs - riseUs);   /* last peak */
            } else {
                put(l, LANE_ACT_LED_OFF, 0U, tTrough);
                put(l, LANE_ACT_LED_ON, 1000U, tTrough + (uint64_t)j * stepUs);
            }
        } else {
            put(l, LANE_ACT_LED_ON, 1000U, plan->t0Us + (uint64_t)j * stepUs);
        }
        put(l, LANE_ACT_LED_OFF, 0U, plan->toutUs);
    }
    return CTRL_OK;
}
