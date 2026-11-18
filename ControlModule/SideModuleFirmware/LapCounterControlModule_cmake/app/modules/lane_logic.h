/*
 * Lane logic: the rules of the lane bus that need no bus, kept apart from
 * lane_manager so the host tests can hold them (CONTROL_FIRMWARE_GUIDE §5.1).
 *
 *   join index      Joins always arrive in chain order. When a lane hot-joins,
 *                   every known lane is probed; the joiner gets index
 *                   (lanes answering contiguously from lane 1) + 1. Known lanes
 *                   from the first gap on are LEFT; lanes behind the gap that
 *                   still answer are told to release their address, so they
 *                   rejoin in order instead of keeping a stale one.
 *   EVENT_FIFO      header + count records; only records fully received count
 *                   (the lane pops exactly those, lane_protocol.h).
 *   EVT_CROSSING    one per lane record: lane, type, flags, tEntryUs, seq,
 *                   dwell, depth copied 1:1.
 *   seq gaps        counted, never published (the host sees laneSeq).
 *   duplicates      a record whose seq was already received (at most
 *                   LANE_LOGIC_SEQ_DUP_WINDOW behind the next expected one,
 *                   wrapping u16) is dropped: a read cut by a bus error pops
 *                   nothing on the lane, so its records come again
 *                   (lane_protocol.h). Further back is a lane that restarted
 *                   its count: taken, and the count resyncs.
 *   idle-safe       a STATUS with LANE_STATUS_BUS_IDLE_SAFE means the lane's
 *                   bus watchdog stopped sampling: re-push the sampling mode
 *                   (COMMAND SAMPLING, which also clears the flag).
 *   health          STATUS changes become SYNC_LOST / SYNC_OK / FAULT once.
 *   configured      CONFIG applied and SYNC aligned, lane IDLE or SAMPLING.
 *   config checks   what LANE_CONFIG_SET refuses before it reaches a lane.
 *
 * Pure logic: no HAL, no globals. Host-tested (test/host_lane_logic).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "host_protocol.h"
#include "lane_protocol.h"

/* Bit n-1 = lane n. */
#define LANE_LOGIC_MASK(n)     ((uint8_t)(1U << ((n) - 1U)))
#define LANE_LOGIC_ALL_MASK    ((uint8_t)((1U << LANE_MAX_LANES) - 1U))

typedef struct {
    uint8_t index;          /* 1..6 for the joiner, 0 = no slot (a 7th lane) */
    uint8_t leftMask;       /* known lanes to mark LEFT                       */
    uint8_t releaseMask;    /* answering lanes behind the gap: RELEASE_CHAIN  */
} laneJoinDecision_t;

void laneLogicDecideJoin(uint8_t knownMask, uint8_t answeringMask, laneJoinDecision_t *d);

/*
 * Parses an EVENT_FIFO read of rxLen bytes. *count = records fully received
 * (header count, capped by the bytes that arrived). Errors: fewer bytes than
 * the header (ERR_BAD_LENGTH), a header count above LANE_FIFO_READ_MAX
 * (ERR_INVALID_ARG: garbage, take nothing).
 */
ctrlStatus_e laneLogicParseFifo(const uint8_t *buf, uint16_t rxLen, laneFifoHeader_t *hdr,
                                laneEvent_t events[LANE_FIFO_READ_MAX], uint8_t *count);

/* Same for RAW_FIFO: *count samples fully received, header count capped at
 * LANE_RAW_READ_MAX. */
ctrlStatus_e laneLogicParseRaw(const uint8_t *buf, uint16_t rxLen, laneRawHeader_t *hdr,
                               uint16_t samples[LANE_RAW_READ_MAX], uint8_t *count);

void laneLogicCrossing(uint8_t lane, const laneEvent_t *ev, hpEvtCrossing_t *out);

/* Events lost on the lane between the expected seq and the one received
 * (wrapping u16). 0 = in order. A value >= 0x8000 means the lane went
 * backwards (a reset the bus did not notice): the caller resyncs. */
uint16_t laneLogicSeqGap(uint16_t expected, uint16_t got);

/* How far back a repeated seq may lie: a cut read re-sends at most
 * LANE_FIFO_READ_MAX records; the rest is margin. */
#define LANE_LOGIC_SEQ_DUP_WINDOW  64U

typedef enum {
    LANE_SEQ_TAKE = 0,      /* the next one, or the first one seen          */
    LANE_SEQ_GAP,           /* take it; laneLogicSeqGap() were lost          */
    LANE_SEQ_DUPLICATE,     /* already received: drop it                     */
    LANE_SEQ_RESYNC,        /* far behind: the lane restarted; take it       */
} laneSeqVerdict_e;

/* Verdict on a received record seq, given the next expected one (known =
 * false: nothing expected yet, always LANE_SEQ_TAKE). After a take, gap or
 * resync the caller expects got + 1; after a duplicate, nothing changes. */
laneSeqVerdict_e laneLogicSeqCheck(bool known, uint16_t expected, uint16_t got);

/*
 * Where seq checking starts when a lane becomes CONFIGURED. Normally the
 * oldest queued record is STATUS.nextEventSeq - fifoCount. That only holds
 * while the lane never dropped an event: a full lane FIFO spends the seq of
 * every dropped event (lane event_fifo.h), so after an overflow the queue
 * holds a seq gap and the subtraction lands past the oldest record, which
 * would then be dropped as a "duplicate". Unknown (the first record read
 * sets it) when fifoCount saturated (LANE_FIFO_COUNT_MAX) or the lane ever
 * dropped events (droppedEvents != 0 or LANE_STATUS_FIFO_OVERFLOW).
 */
void laneLogicSeqStart(const laneStatus_t *st, bool *known, uint16_t *nextSeq);

/* SCHEDULE read-back (start sequence): the lane's STATUS.schedulePending
 * against the actions just queued. The lane may not have taken the writes
 * off its bus mailbox when the read arrives (its main loop applies them), so
 * a short count is retried until LANE_LOGIC_SCHED_VERIFY_TRIES reads, spaced
 * by LANE_LOGIC_SCHED_VERIFY_RETRY_MS; the sequence's own T0 deadline still
 * aborts first. A failed read or a write that failed is final. */
#define LANE_LOGIC_SCHED_VERIFY_TRIES     10U
#define LANE_LOGIC_SCHED_VERIFY_RETRY_MS  2U

typedef enum {
    LANE_SCHED_VERIFY_OK = 0,
    LANE_SCHED_VERIFY_RETRY,
    LANE_SCHED_VERIFY_FAILED,
} laneSchedVerify_e;

/* readOk: the STATUS read completed with a whole laneStatus_t. tries: reads
 * done so far for this verify, this one included. */
laneSchedVerify_e laneLogicSchedVerify(bool readOk, uint8_t schedulePending, uint8_t expect, uint8_t tries);

/* The lane's bus watchdog put it to safe defaults: re-push the mode. */
bool laneLogicNeedsModePush(const laneStatus_t *st);

/* What the health poll saw last, per lane. */
typedef struct {
    bool    known;          /* a STATUS was read before */
    bool    syncLost;
    bool    faulted;
    int16_t lastError;
} laneHealthView_t;

/*
 * Compares a fresh STATUS with the view, updates the view and writes the
 * hpLaneState_e values to publish (at most 3) into out. Returns how many.
 * Rules: SYNC_LOST when LANE_STATUS_SYNC_LOST rises, SYNC_OK when it falls;
 * FAULT when lastError changes to non-zero or the lane is PARKED, once until
 * it clears. The first STATUS only reports a lane that is already lost or
 * faulted.
 */
uint8_t laneLogicHealth(laneHealthView_t *view, const laneStatus_t *st, uint8_t out[3]);

bool laneLogicIsConfigured(const laneStatus_t *st);

/* The lane lost its config (reset without the bus noticing): push again. */
bool laneLogicNeedsPush(const laneStatus_t *st);

/* LANE_CONFIG_SET checks: the CONFIG ranges of lane_protocol.h (LANE_CFG_*
 * and the cross-field rules listed there), plus reserved 0. Not checked
 * here: the lane's sensor-cycle floor of samplePeriodUs (the lane rejects
 * that one itself). */
ctrlStatus_e laneLogicCheckConfig(const hpLaneConfig_t *cfg);

/* Compiled defaults for lane n: 1 kHz, phases staggered by period / 6 so the
 * 200 mA IR pulses of six lanes never overlap. */
void laneLogicDefaultConfig(uint8_t lane, hpLaneConfig_t *cfg);

/* LANE_STATUS response body from the lane's STATUS and SIGNAL. */
void laneLogicStatusRsp(uint8_t lane, const laneStatus_t *st, const laneSignal_t *sig,
                        hpLaneStatusRsp_t *out);

/* NODE_LIST entry of a lane from its ID register. */
void laneLogicNodeInfo(uint8_t lane, const laneId_t *id, uint8_t nodeState, hpNodeInfo_t *out);
