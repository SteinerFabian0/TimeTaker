/*
 * Lane manager: the lane chain's whole lifecycle (CONTROL_FIRMWARE_GUIDE §5.1,
 * §5.2; lane side: LANE_FIRMWARE_GUIDE §3, lane_protocol.h).
 *
 *   power on:   rail on -> LANE_MANAGER_SETTLE_MS -> SYNC on -> bus pins to
 *               I3C, controller up, RSTDAA -> CHAIN_OUT high. Lane 1 hot-joins.
 *   power off:  every present lane LEFT, controller down, SYNC off, pins
 *               parked, CHAIN_OUT low -> rail off. Signals first.
 *   join:       on every hot-join, every known lane is probed (one STATUS
 *               read); the joiner gets index = lanes answering from 1 upward
 *               + 1 (lane_logic). Lanes from the first gap on are LEFT; lanes
 *               behind the gap that still answer get COMMAND RELEASE_CHAIN.
 *               ENTDAA gives LANE_DYN_ADDR(index). JOINED is published.
 *   push:       ID read, LANE_ID, CONFIG (from the table), SET_REFERENCE
 *               (only if a reference is saved for the index), SYNC_ALIGN
 *               (measured at the transfer start), COMMAND sampling mode, then
 *               STATUS until it shows config applied and SYNC aligned:
 *               CONFIGURED. A lane that resets mid-race comes back configured
 *               and sampling without the host doing anything.
 *   running:    every LANE_MANAGER_POLL_MS one EVENT_FIFO read per lane
 *               (polling first: IBIs are not used in v1), again at once while
 *               the lane says more are waiting. Each record -> EVT_CROSSING,
 *               in the order read. Per-lane seq gaps are counted and logged.
 *               Every second a STATUS read (health: SYNC_LOST/SYNC_OK/FAULT
 *               changes only, re-push a lane that lost its config) and a
 *               SYNC_ALIGN (consistency check; the lane counts shifts).
 *   failures:   three failed transfers in a row to a lane -> LEFT. Nobody is
 *               re-addressed: the lane's hot-join brings it back.
 *   scan:       LANE_SCAN: CHAIN_OUT low, every lane LEFT, RSTDAA, CHAIN_OUT
 *               high again: the chain enumerates from lane 1.
 *   raw:        LANE_RAW_STREAM puts one lane in RAW mode (with the requested
 *               decimation) and turns its RAW_FIFO into STR_RAW_SAMPLES.
 *   idle-safe:  STATUS with BUS_IDLE_SAFE: SET_REFERENCE (if saved), then
 *               COMMAND SAMPLING with the table mode.
 *   calibrate:  LANE_CALIBRATE, a state machine per lane run from
 *               laneManagerRun (never blocks). LEARN: COMMAND CALIBRATE, and
 *               COMMAND SAMPLING ON after it when the table mode is OFF (the
 *               lane only learns while sampling; OFF is restored after);
 *               STATUS every 10 ms until CALIBRATED was seen clear and is set
 *               again (1 s at most); SIGNAL -> reference, noise. When no lane
 *               is learning any more, the references of every finished lane
 *               go to settings_store in one change, so one flash write
 *               covers them; the write's outcome gives SAVED / NOT_SAVED.
 *               FORGET: references 0 for the mask, SET_REFERENCE 0 to the
 *               lanes on the bus, FORGOTTEN / NOT_SAVED after the write. A
 *               lane that leaves while learning: FAILED.
 *               EVT_LANE_CALIBRATED per lane (lane_logic.h "calibration").
 *
 * The control module keeps the lane config table (one entry per index plus
 * the sampling mode); the lanes keep nothing across a reset. It starts from
 * settings_store and is written back there on LANE_CONFIG_SET{save = 1}.
 *
 * sequence_manager queues its start-light actions through
 * laneManagerSchedule(): they go to the controller's priority queue, ahead of
 * polling, in their own order.
 *
 * Owns: the policy over lane_chain, i3c_ctrl (its only user) and the SYNC
 * output of sys_time. Publishes EVT_LANE_STATE, EVT_CROSSING, STR_RAW_SAMPLES.
 * Context: main loop only.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ctrl_status.h"
#include "host_protocol.h"
#include "lane_protocol.h"

/* U403 TPS22917 with CT = 10 nF ramps in about 16 ms into ~600 µF of lane
 * capacitance; the lanes' own boot needs a few ms more before SYNC matters. */
#define LANE_MANAGER_SETTLE_MS        30U

#define LANE_MANAGER_POLL_MS          10U     /* EVENT_FIFO per lane */
#define LANE_MANAGER_RAW_POLL_MS      10U
#define LANE_MANAGER_HEALTH_MS        1000U   /* STATUS per lane */
#define LANE_MANAGER_ALIGN_MS         1000U   /* SYNC_ALIGN per lane */
#define LANE_MANAGER_FAIL_LIMIT       3U      /* failed transfers in a row -> LEFT */
#define LANE_MANAGER_ENUM_FIRST_MS    500U    /* end-of-chain timeout, lane 1 (still booting) */
#define LANE_MANAGER_ENUM_NEXT_MS     200U    /* ... after each join (bench values) */
#define LANE_MANAGER_CHAIN_LOW_MS     20U     /* CHAIN_OUT low before high again (lanes debounce) */
#define LANE_MANAGER_VERIFY_RETRY_MS  20U
#define LANE_MANAGER_VERIFY_TRIES     5U
#define LANE_MANAGER_PUSH_TRIES       3U

typedef enum {
    LANE_SCHED_RESULT_NONE = 0,     /* nothing asked */
    LANE_SCHED_RESULT_PENDING,      /* writes or the verify read still queued */
    LANE_SCHED_RESULT_OK,           /* the lane holds at least the expected actions */
    LANE_SCHED_RESULT_FAILED,       /* a write failed or the lane holds fewer */
} laneSchedResult_e;

ctrlStatus_e laneManagerInit(void);

ctrlStatus_e laneManagerRun(void);

/* Starts the power-on sequence or performs the power-off one. Idempotent. */
ctrlStatus_e laneManagerSetPower(bool on);

/* Rail settled and SYNC running. */
bool laneManagerIsPowered(void);

/* Rail on, settled or not. */
bool laneManagerIsRailOn(void);

/* LANE_SCAN. CTRL_ERR_LANE_POWER without lane power. */
ctrlStatus_e laneManagerScan(void);

/* Bit n-1 = lane n CONFIGURED (polled, ready for a start sequence). */
uint8_t laneManagerConfiguredMask(void);

/* Lanes enumerated contiguously from lane 1 (JOINED or later). */
uint8_t laneManagerPresentCount(void);

/* NODE_LIST entry for lane n; false if it is not on the bus. */
bool laneManagerNodeInfo(uint8_t lane, hpNodeInfo_t *info);

/* Config table. */
ctrlStatus_e laneManagerGetConfig(uint8_t lane, hpLaneConfig_t *cfg);
ctrlStatus_e laneManagerSetConfig(uint8_t laneMask, const hpLaneConfig_t *cfg, bool save);
ctrlStatus_e laneManagerSetSampling(uint8_t laneMask, uint8_t mode);

/* START_SEQUENCE armSampling reached T0: the lanes ran their scheduled
 * SAMPLING_ON, so their table mode becomes ON where it was OFF (no bus
 * write). A re-join or a BUS_IDLE_SAFE re-push then keeps them sampling. */
void         laneManagerArmSampling(uint8_t laneMask);

/* Immediate lane requests. CTRL_ERR_LANE_ABSENT if no lane of the mask is on
 * the bus; the writes to the others still go out. */
ctrlStatus_e laneManagerSetLed(uint8_t laneMask, const laneLed_t *led);

/* LANE_CALIBRATE: action hpCalibrateAction_e. Rejections: laneLogicCalAccept
 * (lane_logic.h); the caller rejects it while a start sequence runs. The
 * outcomes follow as EVT_LANE_CALIBRATED. */
ctrlStatus_e laneManagerCalibrate(uint8_t laneMask, uint8_t action);

/* LANE_RAW_STREAM: lane 1..6 starts (one at a time), 0 stops. */
ctrlStatus_e laneManagerRawStream(uint8_t lane, uint16_t decimation);

/* LANE_STATUS, read fresh: start, then poll until it returns true. */
ctrlStatus_e laneManagerStatusRequest(uint8_t lane);
bool         laneManagerStatusPoll(hpLaneStatusRsp_t *rsp, ctrlStatus_e *status);

/* Start-sequence support: priority writes to the SCHEDULE register, and a
 * STATUS read that checks the lane's queue holds at least expectPending. */
ctrlStatus_e      laneManagerSchedule(uint8_t lane, const laneSchedule_t *action);
ctrlStatus_e      laneManagerScheduleVerify(uint8_t lane, uint8_t expectPending);
laneSchedResult_e laneManagerScheduleResult(uint8_t lane);
void              laneManagerScheduleForget(void);
