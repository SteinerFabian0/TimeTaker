/*
 * Host test for seq_planner.c: the start sequence plan (SYSTEM_ARCHITECTURE
 * §6.4, CONTROL_FIRMWARE_GUIDE §5.3). One broken rule per rejection.
 */

#include <stdbool.h>
#include <stdint.h>

#include "seq_planner.h"
#include "../test_check.h"

static seqPlanParams_t base(void)
{
    seqPlanParams_t p = { .lights = 6, .stepMs = 1000, .holdMinMs = 200, .holdMaxMs = 3000, .leadMs = 0 };
    return p;
}

static void testPlan(void)
{
    seqPlanParams_t p = base();
    seqPlan_t plan;
    const uint64_t now = 10000000ULL;
    check("plan: OK", seqPlannerPlan(&p, 6, now, 0U, &plan) == CTRL_OK);
    check("plan: default lead 150 ms", plan.t0Us == now + 150000ULL);
    check("plan: lane k on at T0 + k*step",
          plan.litCount == 6 && plan.onUs[0] == plan.t0Us && plan.onUs[5] == plan.t0Us + 5000000ULL);
    check("plan: random 0 -> hold = holdMin", plan.holdUs == 200000U);
    check("plan: Tout = T0 + n*step + hold", plan.toutUs == plan.t0Us + 6000000ULL + 200000ULL);

    seqPlannerPlan(&p, 6, now, 2800000U, &plan);
    check("plan: random at the top of the span -> hold = holdMax", plan.holdUs == 3000000U);
    seqPlannerPlan(&p, 6, now, 2800001U, &plan);
    check("plan: random wraps inside the span", plan.holdUs == 200000U);

    p.holdMinMs = p.holdMaxMs = 500;
    seqPlannerPlan(&p, 6, now, 0xFFFFFFFFU, &plan);
    check("plan: holdMin == holdMax -> fixed hold", plan.holdUs == 500000U);

    p = base();
    seqPlannerPlan(&p, 3, now, 0U, &plan);
    check("plan: fewer lanes than lights -> n = present", plan.litCount == 3
          && plan.toutUs == plan.t0Us + 3000000ULL + 200000ULL);
    p.lights = 2;
    seqPlannerPlan(&p, 6, now, 0U, &plan);
    check("plan: fewer lights than lanes -> n = lights", plan.litCount == 2);

    p = base();
    p.leadMs = 400;
    seqPlannerPlan(&p, 6, now, 0U, &plan);
    check("plan: explicit lead used", plan.t0Us == now + 400000ULL);
}

static void testReject(void)
{
    seqPlan_t plan;
    seqPlanParams_t p = base();
    p.lights = 0;
    check("reject: 0 lights", seqPlannerPlan(&p, 6, 0, 0, &plan) == CTRL_ERR_INVALID_ARG);
    p = base(); p.lights = 7;
    check("reject: 7 lights", seqPlannerPlan(&p, 6, 0, 0, &plan) == CTRL_ERR_INVALID_ARG);
    p = base(); p.stepMs = 0;
    check("reject: step 0", seqPlannerPlan(&p, 6, 0, 0, &plan) == CTRL_ERR_INVALID_ARG);
    p = base(); p.holdMinMs = 3001;
    check("reject: holdMin > holdMax", seqPlannerPlan(&p, 6, 0, 0, &plan) == CTRL_ERR_INVALID_ARG);
    p = base(); p.leadMs = SEQ_PLANNER_MIN_LEAD_MS - 1U;
    check("reject: lead inside the bus lead time -> ERR_TOO_LATE",
          seqPlannerPlan(&p, 6, 0, 0, &plan) == CTRL_ERR_TOO_LATE);
    p = base(); p.leadMs = SEQ_PLANNER_MIN_LEAD_MS;
    check("accept: lead exactly the minimum", seqPlannerPlan(&p, 6, 0, 0, &plan) == CTRL_OK);
    p = base();
    check("reject: no lane present -> ERR_LANE_ABSENT", seqPlannerPlan(&p, 0, 0, 0, &plan) == CTRL_ERR_LANE_ABSENT);
    check("reject: a sum that overflows the u64 axis",
          seqPlannerPlan(&p, 6, UINT64_MAX - 1000ULL, 0, &plan) == CTRL_ERR_INVALID_ARG);
}

int main(void)
{
    testPlan();
    testReject();
    return finish();
}
