/*
 * Scheduled-action queue. See schedule_queue.h for the rules.
 */

#include "schedule_queue.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

void scheduleQueueInit(scheduleQueue_t *q)
{
    memset(q, 0, sizeof(*q));
}

laneStatus_e scheduleQueueAdd(scheduleQueue_t *q, uint64_t atUs, uint8_t action, uint16_t param)
{
    if (q->count >= LANE_SCHEDULE_DEPTH) {
        return LANE_ERR_SCHED_FULL;
    }

    /* After every entry due at or before atUs: equal times keep their order. */
    uint32_t pos = q->count;
    while (pos > 0U && q->entries[pos - 1U].atUs > atUs) {
        q->entries[pos] = q->entries[pos - 1U];
        pos--;
    }

    q->entries[pos].atUs   = atUs;
    q->entries[pos].action = action;
    q->entries[pos].param  = param;
    q->count++;
    return LANE_OK;
}

void scheduleQueueCancelAll(scheduleQueue_t *q)
{
    q->count = 0U;
}

uint8_t scheduleQueueCount(const scheduleQueue_t *q)
{
    return q->count;
}

bool scheduleQueuePeek(const scheduleQueue_t *q, scheduleEntry_t *out)
{
    if (q->count == 0U) {
        return false;
    }
    *out = q->entries[0];
    return true;
}

bool scheduleQueuePopDue(scheduleQueue_t *q, uint64_t nowUs, scheduleEntry_t *out,
                         uint64_t *lateUs)
{
    if (q->count == 0U || q->entries[0].atUs > nowUs) {
        return false;
    }

    *out    = q->entries[0];
    *lateUs = nowUs - out->atUs;

    q->count--;
    memmove(&q->entries[0], &q->entries[1], (uint32_t)q->count * sizeof(scheduleEntry_t));
    return true;
}
