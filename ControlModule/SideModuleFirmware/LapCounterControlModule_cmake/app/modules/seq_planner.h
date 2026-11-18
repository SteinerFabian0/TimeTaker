/*
 * Start sequence planner: the F1-style start as pure arithmetic
 * (SYSTEM_ARCHITECTURE §6.4, CONTROL_FIRMWARE_GUIDE §5.3).
 *
 *   T0       = now + lead           lead >= SEQ_PLANNER_MIN_LEAD_MS (bus time
 *                                   to write every lane twice, with retries)
 *   on[k]    = T0 + k * step        k = 0..n-1, the first n lanes present, in
 *                                   index order, n = min(lights, present)
 *   hold h   = holdMin + random % (holdMax - holdMin + 1 ms), µs resolution
 *   Tout     = T0 + n * step + h    every lit lane goes dark at Tout
 *
 * Nothing here knows the bus or the race: sequence_manager feeds it the time,
 * the lanes present and a TRNG word, and writes the plan to the lanes.
 * Tout must never leave the control module before it passes (EVT_LIGHTS_OUT).
 *
 * Pure logic: no HAL, no globals. Host-tested (test/host_seq_planner).
 */

#pragma once

#include <stdint.h>

#include "host_protocol.h"

#define SEQ_PLANNER_MAX_LIGHTS      6U
#define SEQ_PLANNER_MIN_LEAD_MS     150U   /* also the default when leadMs = 0 */

typedef struct {
    uint8_t  lights;        /* 1..6 */
    uint16_t stepMs;        /* > 0 */
    uint16_t holdMinMs;
    uint16_t holdMaxMs;     /* >= holdMinMs */
    uint16_t leadMs;        /* 0 = SEQ_PLANNER_MIN_LEAD_MS */
} seqPlanParams_t;

typedef struct {
    uint64_t t0Us;
    uint64_t onUs[SEQ_PLANNER_MAX_LIGHTS];   /* on[k] for k < litCount */
    uint64_t toutUs;
    uint32_t holdUs;
    uint8_t  litCount;                       /* n */
} seqPlan_t;

/*
 * Plans a sequence for presentCount lanes at nowUs with one random word.
 *   CTRL_ERR_INVALID_ARG   lights 0 or > 6, step 0, holdMin > holdMax, or a
 *                          time that overflows the u64 axis
 *   CTRL_ERR_TOO_LATE      a requested lead shorter than the minimum: the
 *                          start would already be inside the bus lead time
 *   CTRL_ERR_LANE_ABSENT   no lane present
 */
ctrlStatus_e seqPlannerPlan(const seqPlanParams_t *params, uint8_t presentCount, uint64_t nowUs,
                            uint32_t randomWord, seqPlan_t *plan);
