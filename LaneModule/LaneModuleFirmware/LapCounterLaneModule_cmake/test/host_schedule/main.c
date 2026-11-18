/*
 * Host test for schedule_queue.c: the SCHEDULE register's queue. Actions fire
 * in time order (equal times in the order added), a late one is due at once
 * and reports its lateness, cancel-all means nothing fires, a full queue
 * rejects, and a popped action never fires twice.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "schedule_queue.h"

static int failures;

static void check(const char *name, int passed)
{
    printf("%-58s %s\n", name, passed ? "ok" : "FAIL");
    failures += passed ? 0 : 1;
}

static scheduleQueue_t q;

static void testOrdering(void)
{
    scheduleEntry_t e;
    uint64_t        late = 0U;

    scheduleQueueInit(&q);
    check("empty: nothing to peek", !scheduleQueuePeek(&q, &e));
    check("empty: nothing due", !scheduleQueuePopDue(&q, UINT64_MAX, &e, &late));

    (void)scheduleQueueAdd(&q, 3000U, LANE_ACT_LED_OFF, 0U);
    (void)scheduleQueueAdd(&q, 1000U, LANE_ACT_LED_ON, 500U);
    (void)scheduleQueueAdd(&q, 2000U, LANE_ACT_LED_ON, 600U);
    (void)scheduleQueueAdd(&q, 2000U, LANE_ACT_LED_OFF, 1U);   /* same time, added later */

    check("order: peek gives the earliest", scheduleQueuePeek(&q, &e) && e.atUs == 1000U && e.param == 500U);
    check("order: not due before its time", !scheduleQueuePopDue(&q, 999U, &e, &late));
    check("order: count 4", scheduleQueueCount(&q) == 4U);

    check("order: 1000 first, on time",
          scheduleQueuePopDue(&q, 1000U, &e, &late) && e.atUs == 1000U && late == 0U);
    check("order: equal times in the order added (1)",
          scheduleQueuePopDue(&q, 2000U, &e, &late) && e.action == LANE_ACT_LED_ON && e.param == 600U);
    check("order: equal times in the order added (2)",
          scheduleQueuePopDue(&q, 2000U, &e, &late) && e.action == LANE_ACT_LED_OFF && e.param == 1U);
    check("order: 3000 not yet at 2000", !scheduleQueuePopDue(&q, 2000U, &e, &late));
    check("order: 3000 last", scheduleQueuePopDue(&q, 3000U, &e, &late) && e.atUs == 3000U);
    check("order: popped actions never fire twice", !scheduleQueuePopDue(&q, UINT64_MAX, &e, &late));
}

static void testLate(void)
{
    scheduleEntry_t e;
    uint64_t        late = 0U;

    scheduleQueueInit(&q);
    (void)scheduleQueueAdd(&q, 1000U, LANE_ACT_LED_OFF, 0U);
    check("late: a past time is due at once",
          scheduleQueuePopDue(&q, 1750U, &e, &late) && e.atUs == 1000U);
    check("late: lateness reported", late == 750U);

    /* Several overdue: they still come out in time order, each with its own
     * lateness, so the caller can fire them all in one pass. */
    (void)scheduleQueueAdd(&q, 500U, LANE_ACT_LED_ON, 0U);
    (void)scheduleQueueAdd(&q, 200U, LANE_ACT_SAMPLING_ON, 0U);
    check("late: oldest overdue first",
          scheduleQueuePopDue(&q, 1000U, &e, &late) && e.atUs == 200U && late == 800U);
    check("late: then the next",
          scheduleQueuePopDue(&q, 1000U, &e, &late) && e.atUs == 500U && late == 500U);
}

static void testCancel(void)
{
    scheduleEntry_t e;
    uint64_t        late = 0U;

    scheduleQueueInit(&q);
    (void)scheduleQueueAdd(&q, 1000U, LANE_ACT_LED_ON, 0U);
    (void)scheduleQueueAdd(&q, 5000U, LANE_ACT_LED_OFF, 0U);
    scheduleQueueCancelAll(&q);
    check("cancel: queue empty", scheduleQueueCount(&q) == 0U);
    check("cancel: a cancelled action never fires, even late",
          !scheduleQueuePopDue(&q, UINT64_MAX, &e, &late));
    check("cancel: queue usable again", scheduleQueueAdd(&q, 7000U, LANE_ACT_LED_OFF, 0U) == LANE_OK);
}

static void testFull(void)
{
    scheduleEntry_t e;
    uint64_t        late = 0U;

    scheduleQueueInit(&q);
    laneStatus_e s = LANE_OK;
    for (uint32_t i = 0U; i < LANE_SCHEDULE_DEPTH; i++) {
        s = scheduleQueueAdd(&q, 10000U - i, LANE_ACT_LED_ON, (uint16_t)i);
    }
    check("full: depth >= 4 required", LANE_SCHEDULE_DEPTH >= 4U);
    check("full: LANE_SCHEDULE_DEPTH actions fit", s == LANE_OK && scheduleQueueCount(&q) == LANE_SCHEDULE_DEPTH);
    check("full: one more is rejected", scheduleQueueAdd(&q, 1U, LANE_ACT_LED_OFF, 0U) == LANE_ERR_SCHED_FULL);
    check("full: the rejected one is not queued",
          scheduleQueuePeek(&q, &e) && e.atUs == 10000U - (LANE_SCHEDULE_DEPTH - 1U));
    (void)scheduleQueuePopDue(&q, UINT64_MAX, &e, &late);
    check("full: room after a pop", scheduleQueueAdd(&q, 1U, LANE_ACT_LED_OFF, 0U) == LANE_OK);
}

static void testStartSequence(void)
{
    /* What the control module writes for an F1 start on lane 3: cancel, on
     * at T0 + 2 s, off at Tout. */
    scheduleEntry_t e;
    uint64_t        late = 0U;
    uint64_t        t0   = 123456789000ULL;

    scheduleQueueInit(&q);
    scheduleQueueCancelAll(&q);
    (void)scheduleQueueAdd(&q, t0 + 4321000U, LANE_ACT_LED_OFF, 0U);
    (void)scheduleQueueAdd(&q, t0 + 2000000U, LANE_ACT_LED_ON, 1000U);
    check("start: light on first",
          scheduleQueuePopDue(&q, t0 + 2000003U, &e, &late) && e.action == LANE_ACT_LED_ON && late == 3U);
    check("start: lights out at Tout, 64-bit times",
          scheduleQueuePopDue(&q, t0 + 4321000U, &e, &late) && e.action == LANE_ACT_LED_OFF && late == 0U);
}

static void testPulse(void)
{
    scheduleEntry_t e;
    uint64_t        late = 0U;

    scheduleQueueInit(&q);
    (void)scheduleQueueAdd(&q, 5000U, LANE_ACT_LED_OFF, 0U);
    (void)scheduleQueueAdd(&q, 1000U, LANE_ACT_LED_PULSE, 1000U);
    (void)scheduleQueueAdd(&q, 3000U, LANE_ACT_LED_PULSE, 500U);
    check("pulse: queued like any action", scheduleQueueCount(&q) == 3U);
    check("pulse: sorted, param kept",
          scheduleQueuePopDue(&q, 1000U, &e, &late) && e.action == LANE_ACT_LED_PULSE && e.param == 1000U);
    check("pulse: second pulse after it, then the OFF",
          scheduleQueuePopDue(&q, 4000U, &e, &late) && e.action == LANE_ACT_LED_PULSE && e.param == 500U
          && scheduleQueuePopDue(&q, 5000U, &e, &late) && e.action == LANE_ACT_LED_OFF);
}

int main(void)
{
    testPulse();
    testOrdering();
    testLate();
    testCancel();
    testFull();
    testStartSequence();

    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
