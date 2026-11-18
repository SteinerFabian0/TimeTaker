/*
 * Start sequence planner. See seq_planner.h.
 */

#include "seq_planner.h"

#include <stdint.h>
#include <string.h>

#define SEQ_PLANNER_US_PER_MS  1000U

ctrlStatus_e seqPlannerPlan(const seqPlanParams_t *params, uint8_t presentCount, uint64_t nowUs,
                            uint32_t randomWord, seqPlan_t *plan)
{
    if (params->lights == 0U || params->lights > SEQ_PLANNER_MAX_LIGHTS || params->stepMs == 0U
        || params->holdMinMs > params->holdMaxMs) {
        return CTRL_ERR_INVALID_ARG;
    }
    if (params->leadMs != 0U && params->leadMs < SEQ_PLANNER_MIN_LEAD_MS) {
        return CTRL_ERR_TOO_LATE;
    }
    if (presentCount == 0U) {
        return CTRL_ERR_LANE_ABSENT;
    }

    const uint8_t  n      = (presentCount < params->lights) ? presentCount : params->lights;
    const uint32_t leadMs = (params->leadMs == 0U) ? SEQ_PLANNER_MIN_LEAD_MS : params->leadMs;

    /* Hold drawn in µs, so Tout is not even predictable to the millisecond.
     * The range fits u32: 65535 ms = 65 535 000 µs. Modulo bias < 2^-6. */
    const uint32_t holdMinUs = (uint32_t)params->holdMinMs * SEQ_PLANNER_US_PER_MS;
    const uint32_t spanUs    = ((uint32_t)params->holdMaxMs - params->holdMinMs) * SEQ_PLANNER_US_PER_MS + 1U;
    const uint32_t holdUs    = holdMinUs + (randomWord % spanUs);

    const uint64_t leadUs  = (uint64_t)leadMs * SEQ_PLANNER_US_PER_MS;
    const uint64_t stepUs  = (uint64_t)params->stepMs * SEQ_PLANNER_US_PER_MS;
    const uint64_t totalUs = leadUs + (uint64_t)n * stepUs + holdUs;
    if (nowUs > UINT64_MAX - totalUs) {
        return CTRL_ERR_INVALID_ARG;   /* the axis would wrap: never, unless the clock is broken */
    }

    memset(plan, 0, sizeof(*plan));
    plan->t0Us     = nowUs + leadUs;
    plan->litCount = n;
    plan->holdUs   = holdUs;
    for (uint8_t k = 0U; k < n; k++) {
        plan->onUs[k] = plan->t0Us + (uint64_t)k * stepUs;
    }
    plan->toutUs = plan->t0Us + (uint64_t)n * stepUs + holdUs;
    return CTRL_OK;
}
