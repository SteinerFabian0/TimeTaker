/*
 * Host test for seq_planner.c: the start sequence plan (SYSTEM_ARCHITECTURE
 * §6.4, CONTROL_FIRMWARE_GUIDE §5.3). One broken rule per rejection.
 */

#include <stdbool.h>
#include <stdint.h>

#include "seq_planner.h"
#include "../test_check.h"

#define MS 1000ULL

static seqPlanParams_t base(void)
{
    seqPlanParams_t p = { .lights = 6, .attentionPulses = 2, .pulseRiseMs = 1000, .stepMs = 1000,
                          .holdMinMs = 200, .holdMaxMs = 3000, .leadMs = 0 };
    return p;
}

static bool is(const seqPlan_t *plan, uint8_t lane, uint8_t i, uint8_t action, uint16_t param, uint64_t at)
{
    const seqPlanAction_t *a = &plan->lane[lane - 1U].act[i];
    return i < plan->lane[lane - 1U].count && a->action == action && a->param == param && a->atUs == at;
}

static void testPlan(void)
{
    seqPlanParams_t p = base();
    seqPlan_t plan;
    const uint64_t now = 10000000ULL;
    check("plan: OK", seqPlannerPlan(&p, 0x3F, now, 0U, &plan) == CTRL_OK);
    const uint64_t t0 = now + 150000ULL;
    check("plan: default lead 150 ms", plan.t0Us == t0);
    check("plan: n = 6, first lane 1", plan.litCount == 6 && plan.firstLane == 1 && plan.litMask == 0x3F);
    /* k = 2, P = 1 s: peaks at T0+1s, T0+3s; trough at T0+4s */
    check("first lane: pulse, on at the last peak, off at Tout",
          plan.lane[0].count == 3 && is(&plan, 1, 0, LANE_ACT_LED_PULSE, 1000, t0)
          && is(&plan, 1, 1, LANE_ACT_LED_ON, 1000, t0 + 3000 * MS)
          && is(&plan, 1, 2, LANE_ACT_LED_OFF, 0, plan.toutUs));
    check("lane 2: pulse, off at trough, on at +1 step, off at Tout",
          plan.lane[1].count == 4 && is(&plan, 2, 0, LANE_ACT_LED_PULSE, 1000, t0)
          && is(&plan, 2, 1, LANE_ACT_LED_OFF, 0, t0 + 4000 * MS)
          && is(&plan, 2, 2, LANE_ACT_LED_ON, 1000, t0 + 5000 * MS)
          && is(&plan, 2, 3, LANE_ACT_LED_OFF, 0, plan.toutUs));
    check("lane 6 lights last, 5 steps after the trough",
          is(&plan, 6, 2, LANE_ACT_LED_ON, 1000, t0 + 9000 * MS));
    check("plan: Tlit = T0 + 2kP + (n-1) step", plan.tLitUs == t0 + 9000 * MS);
    check("plan: random 0 -> hold = holdMin, Tout = Tlit + hold", plan.holdUs == 200000U
          && plan.toutUs == plan.tLitUs + 200 * MS);
    check("plan: ascending order across lanes", is(&plan, 5, 2, LANE_ACT_LED_ON, 1000, t0 + 8000 * MS));

    seqPlannerPlan(&p, 0x3F, now, 2800000U, &plan);
    check("plan: random at the top of the span -> hold = holdMax", plan.holdUs == 3000000U);
    seqPlannerPlan(&p, 0x3F, now, 2800001U, &plan);
    check("plan: random wraps inside the span", plan.holdUs == 200000U);

    p.holdMinMs = p.holdMaxMs = 500;
    seqPlannerPlan(&p, 0x3F, now, 0xFFFFFFFFU, &plan);
    check("plan: holdMin == holdMax -> fixed hold", plan.holdUs == 500000U);

    p = base();
    seqPlannerPlan(&p, 0x07, now, 0U, &plan);
    check("plan: fewer lanes than lights -> n = present, first = 1",
          plan.litCount == 3 && plan.firstLane == 1 && plan.tLitUs == plan.t0Us + 4000 * MS + 2000 * MS);
    p.lights = 2;
    seqPlannerPlan(&p, 0x3F, now, 0U, &plan);
    check("plan: fewer lights than lanes -> first 2 lanes, first = 1",
          plan.litCount == 2 && plan.firstLane == 1 && plan.litMask == 0x03 && plan.lane[2].count == 0);

    p = base();
    p.leadMs = 400;
    seqPlannerPlan(&p, 0x3F, now, 0U, &plan);
    check("plan: explicit lead used", plan.t0Us == now + 400000ULL);

    p = base();
    p.pulseRiseMs = 0;
    seqPlannerPlan(&p, 0x3F, now, 0U, &plan);
    check("plan: pulseRise 0 = 500 ms", is(&plan, 1, 0, LANE_ACT_LED_PULSE, 500, plan.t0Us));
    p.pulseRiseMs = 500;
    seqPlannerPlan(&p, 0x3F, now, 0U, &plan);
    check("plan: P = 500, first on at T0 + 1500", is(&plan, 1, 1, LANE_ACT_LED_ON, 1000, plan.t0Us + 1500 * MS)
          && is(&plan, 3, 1, LANE_ACT_LED_OFF, 0, plan.t0Us + 2000 * MS));

    p = base();
    p.attentionPulses = 1;
    seqPlannerPlan(&p, 0x3F, now, 0U, &plan);
    check("plan: k = 1, first on at the single peak", is(&plan, 1, 1, LANE_ACT_LED_ON, 1000, plan.t0Us + 1000 * MS)
          && is(&plan, 6, 1, LANE_ACT_LED_OFF, 0, plan.t0Us + 2000 * MS));
}

static void testNoPulses(void)
{
    seqPlanParams_t p = base();
    seqPlan_t plan;
    p.attentionPulses = 0;
    seqPlannerPlan(&p, 0x3F, 0, 0U, &plan);
    const uint64_t t0 = plan.t0Us;
    check("k=0: first lane on at T0, no pulse", plan.lane[0].count == 2
          && is(&plan, 1, 0, LANE_ACT_LED_ON, 1000, t0) && is(&plan, 1, 1, LANE_ACT_LED_OFF, 0, plan.toutUs));
    check("k=0: lane 2 on at T0 + step, lane 6 at T0 + 5 steps",
          is(&plan, 2, 0, LANE_ACT_LED_ON, 1000, t0 + 1000 * MS)
          && is(&plan, 6, 0, LANE_ACT_LED_ON, 1000, t0 + 5000 * MS));
    check("k=0: Tlit = T0 + 5 steps", plan.tLitUs == t0 + 5000 * MS);
}

static void testSingleLane(void)
{
    seqPlanParams_t p = base();
    seqPlan_t plan;
    seqPlannerPlan(&p, 0x08, 0, 0U, &plan);   /* only lane 4 */
    check("n=1: lane 4 is the first lane, pulse + on at last peak + off at Tout",
          plan.litCount == 1 && plan.firstLane == 4 && plan.lane[3].count == 3
          && is(&plan, 4, 1, LANE_ACT_LED_ON, 1000, plan.t0Us + 3000 * MS)
          && plan.tLitUs == plan.t0Us + 4000 * MS && plan.toutUs == plan.tLitUs + 200 * MS);
    p.attentionPulses = 0;
    seqPlannerPlan(&p, 0x08, 0, 0U, &plan);
    check("n=1, k=0: on at T0", is(&plan, 4, 0, LANE_ACT_LED_ON, 1000, plan.t0Us) && plan.tLitUs == plan.t0Us);
}

static void testQueueDepth(void)
{
    seqPlan_t plan;
    bool fits = true, ordered = true;
    for (uint8_t k = 0U; k < 4U; k++) {
        for (uint8_t arm = 0U; arm < 2U; arm++) {
            for (uint8_t mask = 1U; mask < 64U; mask++) {
                seqPlanParams_t p = base();
                p.attentionPulses = k;
                p.armSampling = arm;
                seqPlannerPlan(&p, mask, 0, 12345U, &plan);
                for (uint8_t l = 0U; l < 6U; l++) {
                    fits = fits && plan.lane[l].count <= SEQ_PLANNER_MAX_ACTIONS
                           && plan.lane[l].count <= LANE_SCHEDULE_DEPTH;
                    for (uint8_t i = 1U; i < plan.lane[l].count; i++) {
                        ordered = ordered && plan.lane[l].act[i].atUs >= plan.lane[l].act[i - 1U].atUs;
                    }
                }
            }
        }
    }
    check("queue: every lane <= 5 actions (depth 8)", fits);
    check("queue: actions in time order per lane", ordered);

    seqPlanParams_t p = base();
    p.armSampling = 1;
    seqPlannerPlan(&p, 0x3F, 0, 0U, &plan);
    check("sampling: ON at T0 on every lane, first", is(&plan, 1, 0, LANE_ACT_SAMPLING_ON, 0, plan.t0Us)
          && plan.lane[1].count == 5);
    p.lights = 2;
    seqPlannerPlan(&p, 0x3F, 0, 0U, &plan);
    check("sampling: unlit lane still samples", plan.lane[4].count == 1
          && is(&plan, 5, 0, LANE_ACT_SAMPLING_ON, 0, plan.t0Us));
}

static void testHoldRange(void)
{
    seqPlanParams_t p = base();
    seqPlan_t plan;
    bool inRange = true;
    for (uint32_t w = 0U; w < 100000U; w++) {
        seqPlannerPlan(&p, 0x3F, 0, w * 40503U, &plan);
        inRange = inRange && plan.holdUs >= 200000U && plan.holdUs <= 3000000U;
    }
    check("plan: hold always within [holdMin, holdMax]", inRange);
    p.holdMinMs = 0; p.holdMaxMs = 65535;
    seqPlannerPlan(&p, 0x3F, 0, 0xFFFFFFFFU, &plan);
    check("plan: full u16 hold range fits", plan.holdUs <= 65535000U && plan.toutUs > plan.t0Us);
}

static void testReject(void)
{
    seqPlan_t plan;
    seqPlanParams_t p = base();
    p.lights = 0;
    check("reject: 0 lights", seqPlannerPlan(&p, 0x3F, 0, 0, &plan) == CTRL_ERR_INVALID_ARG);
    p = base(); p.lights = 7;
    check("reject: 7 lights", seqPlannerPlan(&p, 0x3F, 0, 0, &plan) == CTRL_ERR_INVALID_ARG);
    p = base(); p.stepMs = 0;
    check("reject: step 0", seqPlannerPlan(&p, 0x3F, 0, 0, &plan) == CTRL_ERR_INVALID_ARG);
    p = base(); p.holdMinMs = 3001;
    check("reject: holdMin > holdMax", seqPlannerPlan(&p, 0x3F, 0, 0, &plan) == CTRL_ERR_INVALID_ARG);
    p = base(); p.leadMs = SEQ_PLANNER_MIN_LEAD_MS - 1U;
    check("reject: lead inside the bus lead time -> ERR_TOO_LATE",
          seqPlannerPlan(&p, 0x3F, 0, 0, &plan) == CTRL_ERR_TOO_LATE);
    p = base(); p.leadMs = SEQ_PLANNER_MIN_LEAD_MS;
    check("accept: lead exactly the minimum", seqPlannerPlan(&p, 0x3F, 0, 0, &plan) == CTRL_OK);
    p = base();
    check("reject: no lane present -> ERR_LANE_ABSENT", seqPlannerPlan(&p, 0, 0, 0, &plan) == CTRL_ERR_LANE_ABSENT);
    check("reject: a sum that overflows the u64 axis",
          seqPlannerPlan(&p, 0x3F, UINT64_MAX - 1000ULL, 0, &plan) == CTRL_ERR_INVALID_ARG);
    p.attentionPulses = 255; p.pulseRiseMs = 65535;
    check("accept: largest pulse train fits the axis", seqPlannerPlan(&p, 0x3F, 0, 0, &plan) == CTRL_OK);
}

int main(void)
{
    testPlan();
    testNoPulses();
    testSingleLane();
    testQueueDepth();
    testHoldRange();
    testReject();
    return finish();
}
