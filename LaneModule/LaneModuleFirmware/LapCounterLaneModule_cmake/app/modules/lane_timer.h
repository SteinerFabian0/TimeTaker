/*
 * Lane timebase: the owner of TIM2.
 *
 * TIM2 free-runs at 1 µs nominal (144 MHz / 144, full 32-bit period, wraps
 * every ~71 min) and is never stopped, reloaded or reset once started: every
 * local timestamp in the firmware is a TIM2 count, and sync_time maps those
 * onto the mainboard's axis.
 *
 * Channels:
 *   CH1  input capture, rising SYNC edge on PA5 (filter N8 in CubeMX). The
 *        capture is in hardware; the ISR only feeds sync_time.
 *   CH2  alarm LANE_TIMER_ALARM_SAMPLE  - sensor force-mode trigger (lane_sampler)
 *   CH3  reserved: TIM2_CH3 is also PA2/BUS_INT, kept free for capturing a
 *        hardware start instant should the contract choose that (guide §3.6).
 *   CH4  alarm LANE_TIMER_ALARM_SCHEDULE - "at mainboard tick T do X" actions
 *
 * Alarm channels stay in their reset state (output, frozen, CCxE off): only
 * the compare flag and its interrupt are used, no pin is driven.
 *
 * This module also owns the weak HAL TIM callbacks
 * (HAL_TIM_InputCaptureCallback, HAL_TIM_CompareMatchCallback): no other file
 * may define them.
 *
 * Context: the TIM2 ISR runs at priority 1. Alarm callbacks run inside it, so
 * they must be short and must not block; they preempt the I3C (2) and I2C (3)
 * ISRs and the main loop. Everything else here is main-loop or ISR safe.
 *
 * Low power: TIM2 stops in Stop1 with the core, so local time stands still
 * and SYNC edges are missed; sync_time counts the gap on wake if it was short
 * and otherwise re-anchors and asks for alignment again.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lane_status.h"
#include "sync_time.h"

typedef enum {
    LANE_TIMER_ALARM_SAMPLE = 0,
    LANE_TIMER_ALARM_SCHEDULE,
    LANE_TIMER_ALARM_COUNT
} laneTimerAlarm_e;

/* Furthest ahead an alarm may be armed, in local counts (~18 min). Past half
 * the 32-bit range "ahead" and "already passed" become indistinguishable. */
#define LANE_TIMER_ALARM_MAX_AHEAD  (1UL << 30)

/*
 * Runs in the TIM2 ISR. dueLocal is the instant the alarm was armed for,
 * firedLocal the counter on entry, so the callee can see its own lateness (a
 * late-armed alarm fires at once with firedLocal past dueLocal).
 */
typedef void (*laneTimerAlarmFn_t)(uint32_t dueLocal, uint32_t firedLocal);

/*
 * Starts the counter and the SYNC capture with the default SYNC period.
 * Assumes mx_tim2_init() has already run. Returns LANE_ERR_TIMER if the HAL
 * refuses either start.
 */
laneStatus_e laneTimerInit(void);

/* Current local time, TIM2 counts. */
uint32_t laneTimerNow(void);

/*
 * Sets the nominal SYNC period (mainboard µs) and forgets lock and alignment.
 * Returns LANE_ERR_INVALID_ARG outside the sync_time limits and then keeps
 * the previous period.
 */
laneStatus_e laneTimerSetSyncPeriod(uint32_t periodUs);

/* See syncTimeAlign(). localRx is when the alignment message arrived. */
laneStatus_e laneTimerSyncAlign(uint32_t mbTick, uint32_t mbPhaseUs, uint32_t localRx);

/* See syncTimeToMainboard(); returns its SYNC_TIME_FLAG_* bits. */
uint8_t laneTimerToMainboard(uint32_t local, uint64_t *mainboardUs);

/* See syncTimeToLocal(). */
laneStatus_e laneTimerToLocal(uint64_t mainboardUs, uint32_t *local);

bool laneTimerSyncLocked(void);

/* Consistent copy of the SYNC state, for STATUS and diagnostics. */
void laneTimerGetSync(syncTime_t *out);

/* SYNC edges the ISR was too late to read (capture overcapture flag). */
uint32_t laneTimerCaptureOverruns(void);

/*
 * Arms an alarm for dueLocal. If dueLocal has already passed by the time the
 * compare is live, the alarm fires at once from the ISR rather than ~71 min
 * later. Returns LANE_ERR_ALARM_BUSY if it is already armed,
 * LANE_ERR_ALARM_RANGE if dueLocal is more than LANE_TIMER_ALARM_MAX_AHEAD
 * ahead, LANE_ERR_INVALID_ARG for a bad alarm or a NULL callback.
 */
laneStatus_e laneTimerArmAlarm(laneTimerAlarm_e alarm, uint32_t dueLocal,
                               laneTimerAlarmFn_t fn);

/* Disarms; harmless on an idle alarm. A callback already running finishes. */
void laneTimerCancelAlarm(laneTimerAlarm_e alarm);

bool laneTimerAlarmArmed(laneTimerAlarm_e alarm);
