/*
 * Sequence manager: the start sequence on the system clock
 * (SYSTEM_ARCHITECTURE §6.4, CONTROL_FIRMWARE_GUIDE §5.3).
 *
 *   START_SEQUENCE  plan it (seq_planner) with a TRNG word, then queue to every
 *                   configured lane through laneManagerSchedule() (priority):
 *                   CANCEL_ALL, then each lane's plan: LED_PULSE (breathing) at T0,
 *                   the first lane on at the last peak, the others off at
 *                   the next trough and on one per step toward lane n, off at
 *                   Tout; "sampling ON at T0" with armSampling. Then
 *                   a STATUS read per lane checks the actions arrived.
 *                   EVT_SEQ_STARTED{T0, lights, step} at once. NEVER Tout.
 *   arming          every lane must confirm before T0 - CTRL_POLICY_SEQ_
 *                   CONFIRM_MS, or the sequence is aborted (LANE_FAILED)
 *                   rather than started with a dark lane.
 *   armSampling     at T0 the lanes' table mode becomes ON (lane_manager),
 *                   so a re-join or BUS_IDLE_SAFE re-push keeps them sampling.
 *   lights out      the lanes go dark on their own timers. When sysUs passes
 *                   Tout in the loop, EVT_LIGHTS_OUT{Tout} is published; a loop
 *                   pass late does not matter, the event carries Tout.
 *   abort           ABORT_SEQUENCE (HOST), BTN2 long (BUTTON, panel_manager),
 *                   a lane leaving (LANE_LEFT), lane power off (POWER):
 *                   cancel every lane's pending actions, blink all lights 3
 *                   times, EVT_SEQ_ABORTED{atUs, reason}.
 *
 * False starts, reaction times and "the first crossing is not a lap" are the
 * web app's, judged against Tout. Nothing here.
 *
 * The per-pass decisions are ctrl_policy's ctrlPolicySeqStep (host-tested).
 *
 * Owns: the start-sequence state. Uses lane_manager's schedule API and rng.
 * Context: main loop only.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ctrl_status.h"
#include "host_protocol.h"

/* Lanes confirm by T0 - CTRL_POLICY_SEQ_CONFIRM_MS (ctrl_policy.h). */
#define SEQUENCE_MANAGER_ABORT_BLINK_MS 400U

ctrlStatus_e sequenceManagerInit(void);

ctrlStatus_e sequenceManagerRun(void);

/* START_SEQUENCE. CTRL_ERR_SEQ_RUNNING if one runs; seq_planner's refusals;
 * CTRL_ERR_LANE_ABSENT with no configured lane. */
ctrlStatus_e sequenceManagerStart(const hpStartSequenceReq_t *req, uint64_t *t0Us);

/* hpSeqAbortReason_e. CTRL_ERR_INVALID_STATE if nothing runs. */
ctrlStatus_e sequenceManagerAbort(uint8_t reason);

bool sequenceManagerIsRunning(void);
