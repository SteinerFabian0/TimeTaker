/*
 * Start sequence planner: the start sequence as pure arithmetic
 * (SYSTEM_ARCHITECTURE §6.4, CONTROL_FIRMWARE_GUIDE §5.3).
 *
 *   P = pulseRiseMs (0 = 500), k = attentionPulses, n = lit lanes
 *       = min(lights, present), the first n lanes present in index order.
 *   T0       = now + lead           lead >= SEQ_PLANNER_MIN_LEAD_MS
 *   T0       every lit lane LED_PULSE(P) (and sampling ON with armSampling).
 *            Peaks at T0 + (2i+1)P, the last at T0 + (2k-1)P: there the first
 *            lane (lowest index of the n, start of the chain) gets LED_ON;
 *            every other lane gets LED_OFF at the following trough T0 + 2kP.
 *   Then the next lit lanes (ascending present index, toward the end of the
 *            chain) LED_ON at T0 + 2kP + j*step, j = 1..n-1.
 *   Tlit     = T0 + 2kP + (n-1)*step
 *   Tout     = Tlit + hold, hold = holdMin + random % (holdMax - holdMin + 1 ms)
 *            (µs resolution); LED_OFF on every lit lane at Tout.
 *   k = 0    no pulses: the first lane is on at T0, the others every step.
 *
 * Each lane gets at most SEQ_PLANNER_MAX_ACTIONS (5) actions, within the lane
 * queue depth (LANE_SCHEDULE_DEPTH, 8). Tout must never leave the control
 * module before it passes (EVT_LIGHTS_OUT).
 *
 * Pure logic: no HAL, no globals. Host-tested (test/host_seq_planner).
 */

#pragma once

#include <stdint.h>

#include "host_protocol.h"
#include "lane_protocol.h"

#define SEQ_PLANNER_MAX_LIGHTS      6U
#define SEQ_PLANNER_MIN_LEAD_MS     150U   /* also the default when leadMs = 0 */
#define SEQ_PLANNER_DEFAULT_PULSE_MS 500U
#define SEQ_PLANNER_MAX_ACTIONS     5U     /* PULSE, OFF/ON, ON, OFF, SAMPLING_ON */

typedef struct {
    uint8_t  lights;        /* 1..6 */
    uint8_t  armSampling;   /* 1 = SAMPLING_ON at T0 on every present lane */
    uint8_t  attentionPulses; /* off-on-off cycles, 0 = none */
    uint16_t pulseRiseMs;   /* 0 = SEQ_PLANNER_DEFAULT_PULSE_MS */
    uint16_t stepMs;        /* > 0 */
    uint16_t holdMinMs;
    uint16_t holdMaxMs;     /* >= holdMinMs */
    uint16_t leadMs;        /* 0 = SEQ_PLANNER_MIN_LEAD_MS */
} seqPlanParams_t;

typedef struct {
    uint8_t  action;        /* laneAction_e */
    uint16_t param;
    uint64_t atUs;
} seqPlanAction_t;

typedef struct {
    uint8_t         count;
    seqPlanAction_t act[SEQ_PLANNER_MAX_ACTIONS];   /* in time order */
} seqPlanLane_t;

typedef struct {
    uint64_t t0Us;
    uint64_t tLitUs;                         /* all lit */
    uint64_t toutUs;
    uint32_t holdUs;
    uint8_t  litCount;                       /* n */
    uint8_t  litMask;                        /* bit (lane-1) of the lit lanes */
    uint8_t  firstLane;                      /* lowest lit lane (lights first), 1..6 */
    seqPlanLane_t lane[LANE_MAX_LANES];      /* index lane-1; count 0 = nothing */
} seqPlan_t;

/*
 * Plans a sequence for the lanes in presentMask (bit lane-1) at nowUs with one
 * random word.
 *   CTRL_ERR_INVALID_ARG   lights 0 or > 6, step 0, holdMin > holdMax, or a
 *                          time that overflows the u64 axis
 *   CTRL_ERR_TOO_LATE      a requested lead shorter than the minimum
 *   CTRL_ERR_LANE_ABSENT   no lane present
 */
ctrlStatus_e seqPlannerPlan(const seqPlanParams_t *params, uint8_t presentMask, uint64_t nowUs,
                            uint32_t randomWord, seqPlan_t *plan);
