/*
 * The SCHEDULE register's action queue: "at system time T do X", guide §3.7.
 *
 * Pure logic: no HAL, no globals, host-tested (test/host_schedule). The owner
 * (bus_manager) adds and cancels from the main loop and pops due actions from
 * the TIM2 alarm ISR; it guards every call with irq_guard.
 *
 * Rules:
 *   - LANE_SCHEDULE_DEPTH (8, at least 4) actions, kept sorted by due time;
 *     equal times fire in the order they were added;
 *   - a full queue rejects the new action (the caller counts badWrites);
 *   - cancel-all drops every pending action: a cancelled action never fires;
 *   - a popped action is gone: it never fires twice;
 *   - an action whose time has already passed is due at once; its lateness
 *     (now − at) is reported so the caller can count lateActions.
 *
 * Times are mainboard µs (sysUs), u64.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lane_status.h"
#include "lane_protocol.h"

typedef struct {
    uint64_t atUs;
    uint16_t param;
    uint8_t  action;    /* laneAction_e */
} scheduleEntry_t;

typedef struct {
    scheduleEntry_t entries[LANE_SCHEDULE_DEPTH];
    uint8_t         count;
} scheduleQueue_t;

void scheduleQueueInit(scheduleQueue_t *q);

/* Queues one action. LANE_ERR_SCHED_FULL when all slots are taken. */
laneStatus_e scheduleQueueAdd(scheduleQueue_t *q, uint64_t atUs, uint8_t action, uint16_t param);

void scheduleQueueCancelAll(scheduleQueue_t *q);

uint8_t scheduleQueueCount(const scheduleQueue_t *q);

/* The earliest action, without removing it. False when empty. */
bool scheduleQueuePeek(const scheduleQueue_t *q, scheduleEntry_t *out);

/*
 * Removes and returns the earliest action if it is due at nowUs (atUs <=
 * nowUs). *lateUs gets nowUs − atUs. False (queue untouched) otherwise.
 */
bool scheduleQueuePopDue(scheduleQueue_t *q, uint64_t nowUs, scheduleEntry_t *out,
                         uint64_t *lateUs);
