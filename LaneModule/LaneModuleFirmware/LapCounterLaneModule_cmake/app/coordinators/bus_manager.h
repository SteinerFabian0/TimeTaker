/**
 * @file bus_manager.h
 * @brief Coordinator for the lane bus: lane_bus (I3C target) + lane_chain,
 *        the register map, and the scheduled-action queue
 *        (SYSTEM_ARCHITECTURE §5, guide §3.2, §3.6, §3.7, §5).
 *
 * Turns register writes into calls on sense_manager, indicator_led and the
 * schedule queue; builds every read response for lane_bus (in the I3C ISR,
 * from snapshots); runs enumeration (hot-join when CHAIN_IN is high and the
 * lane has no address, CHAIN_OUT once addressed); executes scheduled actions
 * from TIM2 alarm CH4 (LANE_TIMER_ALARM_SCHEDULE), so all lanes act on the
 * same mainboard tick regardless of bus latency.
 *
 * Owns: the laneRegs_t counters (badWrites), the schedule queue and CH4, the
 * lane index, the sleep flag, lateActions, the bus watchdog and the IBI
 * policy.
 *
 * Rules:
 *   - Losing the address (CHAIN_IN fell, RSTDAA, RELEASE_CHAIN) forgets the
 *     lane index and the CONFIG, stops sampling, turns the LED off and
 *     cancels pending actions: the lane is UNENUMERATED again.
 *   - Bus watchdog: enumerated and no frame for LANE_BUS_IDLE_SAFE_MS puts
 *     sampling OFF, the LED off and the queue empty, and sets
 *     LANE_STATUS_BUS_IDLE_SAFE. The address is kept and the lane keeps
 *     answering; the flag clears at the next frame, and the control module
 *     re-pushes what it wants.
 *   - A scheduled action needs SYNC locked and aligned to be mapped to local
 *     time; until then it waits in the queue (and fires late, counted).
 *     Late = more than LANE_SCHED_LATE_US after its time.
 *   - IBI (when the controller enabled it): LANE_IBI_EVENTS on a new event,
 *     LANE_IBI_RAW on a full raw chunk, LANE_IBI_STATUS on a STATUS flag or
 *     lastError change; repeated every LANE_BUS_IBI_REPEAT_MS while events
 *     wait. Polling stays the primary path (control guide §5.1).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lane_status.h"
#include "lane_protocol.h"

#define LANE_BUS_IDLE_SAFE_MS     5000U
#define LANE_BUS_IBI_REPEAT_MS    50U
#define LANE_SCHED_LATE_US        100U

laneStatus_e busManagerInit(uint32_t nowMs);

/* Main loop, every pass. */
laneStatus_e busManagerRun(uint32_t nowMs);

/* lane_master publishes the STATUS snapshot each pass; reads patch the live
 * FIFO fields into it. */
void busManagerPublishStatus(const laneStatus_t *status);

/* Standalone bench mode: losing or lacking an address does not stop sensing,
 * and the bus watchdog is off. */
void busManagerSetStandalone(bool on);

/* Parked lane: LED, sampling and schedule writes are refused (still
 * enumerable, STATUS readable). */
void busManagerPark(void);

bool     busManagerHasAddress(void);
uint8_t  busManagerLaneIndex(void);
bool     busManagerSleeping(void);
bool     busManagerBusIdleSafe(void);
uint16_t busManagerBadWrites(void);
uint16_t busManagerLateActions(void);
uint16_t busManagerSyncShifts(void);
uint8_t  busManagerSchedulePending(void);
