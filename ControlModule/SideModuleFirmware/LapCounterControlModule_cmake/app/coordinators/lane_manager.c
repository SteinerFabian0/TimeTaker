/*
 * lane_manager.c
 *
 * @brief  Lane chain lifecycle. See lane_manager.h.
 */

#include "lane_manager.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "stm32_hal.h"
#include "ctrl_status.h"
#include "host_protocol.h"
#include "lane_protocol.h"
#include "lane_chain.h"
#include "lane_logic.h"
#include "i3c_ctrl.h"
#include "sys_time.h"
#include "settings_store.h"
#include "log_line.h"
#include "host_manager.h"

/* --- Transfer tags: lane (3 bits) | op (5 bits) | slot generation (8 bits) -- */

typedef enum {
    OP_RSTDAA = 1,
    OP_ENTDAA,
    OP_PROBE,
    OP_ID,
    OP_LANE_ID,
    OP_CONFIG,
    OP_ALIGN,
    OP_MODE,
    OP_VERIFY,
    OP_FIFO,
    OP_RAW,
    OP_HEALTH,
    OP_ALIGN_CHECK,
    OP_WRITE,          /* fire and forget: LED, COMMAND, config change */
    OP_SCHED,
    OP_SCHED_VERIFY,
    OP_REQ_STATUS,
    OP_REQ_SIGNAL,
} laneOp_e;

#define LANE_TAG(lane, op, gen)  ((uint16_t)(((uint16_t)(gen) << 8) | ((uint16_t)(op) << 3) | (uint16_t)(lane)))
#define LANE_TAG_LANE(tag)       ((uint8_t)((tag) & 0x7U))
#define LANE_TAG_OP(tag)         ((uint8_t)(((tag) >> 3) & 0x1FU))
#define LANE_TAG_GEN(tag)        ((uint8_t)((tag) >> 8))

#define LANE_RX_MAX  ((LANE_RAW_READ_BYTES > LANE_FIFO_READ_BYTES) ? LANE_RAW_READ_BYTES : LANE_FIFO_READ_BYTES)

typedef enum {
    LANE_POWER_OFF = 0,
    LANE_POWER_SETTLING,
    LANE_POWER_ON,
} lanePowerState_e;

typedef enum {
    BUS_DOWN = 0,
    BUS_RESETTING,      /* RSTDAA queued, CHAIN_OUT low */
    BUS_CHAIN_LOW,      /* waiting LANE_MANAGER_CHAIN_LOW_MS */
    BUS_UP,             /* CHAIN_OUT high, joins accepted */
} laneBusState_e;

typedef enum {
    JOIN_IDLE = 0,
    JOIN_PROBING,
    JOIN_DAA,
} laneJoinState_e;

typedef enum {
    SLOT_ABSENT = 0,
    SLOT_JOINED,        /* addressed, push in progress */
    SLOT_CONFIGURED,    /* polled */
    SLOT_FAILED,        /* push gave up or wrong protocol: health only */
} laneSlotState_e;

typedef enum {
    PUSH_ID = 0,
    PUSH_LANE_ID,
    PUSH_CONFIG,
    PUSH_ALIGN,
    PUSH_MODE,
    PUSH_VERIFY,
} lanePushStep_e;

typedef struct {
    uint8_t          state;              /* laneSlotState_e */
    uint8_t          gen;                /* bumped on LEFT: late results are dropped */
    uint8_t          pushStep;
    uint8_t          pushTries;
    uint8_t          verifyTries;
    uint8_t          failStreak;
    bool             opPending;          /* one sequenced transfer in flight */
    bool             pollAgain;
    bool             rawAgain;
    uint32_t         nextVerifyMs;
    uint32_t         lastPollMs;
    uint32_t         lastRawMs;
    uint32_t         lastHealthMs;
    uint32_t         lastAlignMs;
    laneId_t         id;
    bool             idValid;
    laneHealthView_t health;
    bool             seqKnown;
    uint16_t         nextSeq;
    uint32_t         seqGaps;
    uint32_t         seqDups;            /* repeated records dropped */
    uint16_t         lastDropped;
    uint16_t         lastShifts;
    uint8_t          rx[LANE_RX_MAX];
    uint8_t          probe[sizeof(laneStatus_t)];
    uint8_t          sched[sizeof(laneStatus_t)];
    uint8_t          schedResult;        /* laneSchedResult_e */
    uint8_t          schedExpect;
    uint8_t          schedWrites;        /* priority writes still queued */
    uint8_t          schedVerifyTries;   /* STATUS read-backs of this verify */
    bool             schedVerifyDue;     /* a read-back to send at schedVerifyAtMs */
    uint32_t         schedVerifyAtMs;
} laneSlot_t;

static lanePowerState_e laneManagerPower;
static uint32_t         laneManagerRailOnMs;
static laneBusState_e   laneBus;
static uint32_t         laneBusMs;
static laneJoinState_e  laneJoin;
static uint8_t          laneJoinProbes;      /* probes outstanding */
static uint8_t          laneJoinAnswering;   /* mask */
static uint8_t          laneJoinIndex;
static bool             laneJoinProbeLost;   /* a probe could not be queued */
static uint32_t         laneHotJoins;        /* latched, not yet handled */
static bool             laneEnumOpen;
static uint32_t         laneEnumDeadlineMs;
static uint8_t          laneBusGen;          /* bus-level tag generation */

static laneSlot_t       laneSlots[LANE_MAX_LANES + 1U];   /* index 1..6 */

static hpLaneConfig_t   laneTable[LANE_MAX_LANES];
static uint8_t          laneTableMode[LANE_MAX_LANES];

static uint8_t          laneRawLane;         /* 0 = none */
static uint16_t         laneRawDecimation;
static uint8_t          laneRawPrevMode;

static uint8_t          laneReqLane;         /* LANE_STATUS in progress, 0 = none */
static bool             laneReqDone;
static ctrlStatus_e     laneReqStatus;
static laneStatus_t     laneReqSt;
static uint8_t          laneReqBuf[sizeof(laneSignal_t) > sizeof(laneStatus_t) ? sizeof(laneSignal_t) : sizeof(laneStatus_t)];
static hpLaneStatusRsp_t laneReqRsp;

static void laneDone(uint16_t tag, ctrlStatus_e status, uint16_t rxLen);


/* --- Logging ---------------------------------------------------------------- */

static void laneLog(uint8_t level, const char *what, uint8_t lane, const char *key, int32_t v)
{
    logLine_t l;
    logLineInit(&l, "LANE ");
    logLineU32(&l, lane);
    logLineStr(&l, " ");
    logLineStr(&l, what);
    if (key != NULL) {
        logLineKv(&l, key, v);
    }
    hostManagerLog(level, &l);
}

static void lanePublishState(uint8_t lane, uint8_t state, ctrlStatus_e status)
{
    const hpEvtLaneState_t evt = {
        .lane   = lane,
        .state  = state,
        .status = (int16_t)status,
        .sysUs  = sysTimeNowUs(),
    };
    (void)hostManagerPublishEvent(HP_MSG_EVT_LANE_STATE, &evt, sizeof(evt));
}


/* --- Transfers ----------------------------------------------------------------- */

static ctrlStatus_e laneSubmit(uint8_t lane, uint8_t op, uint8_t reg, const void *payload, uint8_t len,
                               uint8_t *rd, uint16_t rdLen, i3cCtrlPrepareFn_t prepare, bool priority)
{
    i3cCtrlXfer_t x;
    memset(&x, 0, sizeof(x));
    x.op      = (rd != NULL) ? (uint8_t)I3C_CTRL_OP_READ : (uint8_t)I3C_CTRL_OP_WRITE;
    x.addr    = LANE_DYN_ADDR(lane);
    x.reg     = reg;
    x.wrLen   = len;
    if (len != 0U) {
        memcpy(x.wr, payload, len);
    }
    x.rd      = rd;
    x.rdLen   = rdLen;
    x.tag     = LANE_TAG(lane, op, laneSlots[lane].gen);
    x.done    = laneDone;
    x.prepare = prepare;
    return i3cCtrlSubmit(&x, priority);
}

/* A sequenced step: at most one per lane in flight. */
static void laneStep(uint8_t lane, uint8_t op, uint8_t reg, const void *payload, uint8_t len,
                     uint8_t *rd, uint16_t rdLen, i3cCtrlPrepareFn_t prepare)
{
    if (laneSubmit(lane, op, reg, payload, len, rd, rdLen, prepare, false) == CTRL_OK) {
        laneSlots[lane].opPending = true;
    }
}

/* SYNC_ALIGN payload, measured as the transfer starts (i3c_ctrl prepare). */
static void lanePrepareAlign(uint8_t *payload, uint8_t len)
{
    uint32_t tick;
    uint32_t phase;
    sysTimeNowSplit(&tick, &phase);
    const laneSyncAlign_t a = { .tick = tick, .phaseUs = phase, .periodUs = LANE_SYNC_DEFAULT_PERIOD_US };
    if (len >= sizeof(a)) {
        memcpy(payload, &a, sizeof(a));
    }
}

static void laneSendAlign(uint8_t lane, uint8_t op)
{
    laneSyncAlign_t a = { 0 };   /* filled by lanePrepareAlign at the start */
    laneStep(lane, op, LANE_REG_SYNC_ALIGN, &a, (uint8_t)sizeof(a), NULL, 0U, lanePrepareAlign);
}

static uint8_t laneModeFor(uint8_t lane)
{
    return (lane == laneRawLane) ? (uint8_t)LANE_SAMPLING_RAW : laneTableMode[lane - 1U];
}

static laneConfig_t laneConfigFor(uint8_t lane)
{
    laneConfig_t c;
    memcpy(&c, &laneTable[lane - 1U], sizeof(c));
    if (lane == laneRawLane) {
        c.rawDecimation = laneRawDecimation;
    }
    return c;
}

static void laneWrite(uint8_t lane, uint8_t reg, const void *payload, uint8_t len)
{
    (void)laneSubmit(lane, OP_WRITE, reg, payload, len, NULL, 0U, NULL, false);
}

static void laneCommand(uint8_t lane, uint8_t cmd, uint8_t arg)
{
    const laneCommand_t c = { .cmd = cmd, .arg = arg };
    laneWrite(lane, LANE_REG_COMMAND, &c, (uint8_t)sizeof(c));
}

static bool laneOnBus(uint8_t lane)
{
    return lane >= 1U && lane <= LANE_MAX_LANES && laneSlots[lane].state != SLOT_ABSENT;
}


/* --- Lane lifecycle -------------------------------------------------------------- */

static void laneMarkLeft(uint8_t lane, ctrlStatus_e reason)
{
    laneSlot_t *s = &laneSlots[lane];
    if (s->state == SLOT_ABSENT) {
        return;
    }
    const uint8_t gen = (uint8_t)(s->gen + 1U);
    memset(s, 0, sizeof(*s));
    s->gen = gen;
    if (laneRawLane == lane) {
        laneRawLane = 0U;
    }
    if (laneReqLane == lane && !laneReqDone) {
        laneReqDone   = true;
        laneReqStatus = CTRL_ERR_LANE_ABSENT;
    }
    lanePublishState(lane, (uint8_t)HP_LANE_STATE_LEFT, reason);
    laneLog(HP_LOG_INFO, "LEFT", lane, "status", reason);
}

static void laneStartPush(uint8_t lane, uint8_t fromStep)
{
    laneSlot_t *s = &laneSlots[lane];
    s->state       = SLOT_JOINED;
    s->pushStep    = fromStep;
    s->verifyTries = 0U;
}

static void laneJoined(uint8_t lane)
{
    laneSlot_t *s = &laneSlots[lane];
    const uint8_t gen = s->gen;
    memset(s, 0, sizeof(*s));
    s->gen = gen;
    laneStartPush(lane, PUSH_ID);
    lanePublishState(lane, (uint8_t)HP_LANE_STATE_JOINED, CTRL_OK);
    laneLog(HP_LOG_INFO, "JOINED", lane, "addr", LANE_DYN_ADDR(lane));
}

static void laneConfigured(uint8_t lane, const laneStatus_t *st)
{
    laneSlot_t     *s   = &laneSlots[lane];
    const uint32_t  now = HAL_GetTick();
    s->state       = SLOT_CONFIGURED;
    s->failStreak  = 0U;
    /* First record still queued, unless the count saturated or the lane
     * dropped events: then the first record read sets it (lane_logic.h). */
    laneLogicSeqStart(st, &s->seqKnown, &s->nextSeq);
    s->lastDropped = st->droppedEvents;
    s->lastShifts  = st->syncShifts;
    s->lastPollMs  = now;
    s->lastHealthMs = now + (uint32_t)lane * 150U;    /* stagger the 1 s reads */
    s->lastAlignMs  = now + (uint32_t)lane * 150U + 75U;
    (void)laneLogicHealth(&s->health, st, (uint8_t[3]){ 0 });
    lanePublishState(lane, (uint8_t)HP_LANE_STATE_CONFIGURED, CTRL_OK);
    laneLog(HP_LOG_INFO, "CONFIGURED", lane, "seq", s->nextSeq);
}

/* One push step per call, when nothing is in flight for the lane. */
static void lanePushNext(uint8_t lane)
{
    laneSlot_t *s = &laneSlots[lane];
    switch (s->pushStep) {
    case PUSH_ID:
        laneStep(lane, OP_ID, LANE_REG_ID, NULL, 0U, s->rx, (uint16_t)sizeof(laneId_t), NULL);
        break;
    case PUSH_LANE_ID: {
        const laneIndex_t idx = { .laneIndex = lane };
        laneStep(lane, OP_LANE_ID, LANE_REG_LANE_ID, &idx, (uint8_t)sizeof(idx), NULL, 0U, NULL);
        break;
    }
    case PUSH_CONFIG: {
        const laneConfig_t c = laneConfigFor(lane);
        laneStep(lane, OP_CONFIG, LANE_REG_CONFIG, &c, (uint8_t)sizeof(c), NULL, 0U, NULL);
        break;
    }
    case PUSH_ALIGN:
        laneSendAlign(lane, OP_ALIGN);
        break;
    case PUSH_MODE: {
        const laneCommand_t c = { .cmd = LANE_CMD_SAMPLING, .arg = laneModeFor(lane) };
        laneStep(lane, OP_MODE, LANE_REG_COMMAND, &c, (uint8_t)sizeof(c), NULL, 0U, NULL);
        break;
    }
    case PUSH_VERIFY:
        if ((int32_t)(HAL_GetTick() - s->nextVerifyMs) >= 0) {
            laneStep(lane, OP_VERIFY, LANE_REG_STATUS, NULL, 0U, s->rx, (uint16_t)sizeof(laneStatus_t), NULL);
        }
        break;
    default:
        break;
    }
}

static void laneVerifyResult(uint8_t lane, const laneStatus_t *st)
{
    laneSlot_t *s = &laneSlots[lane];
    if (laneLogicIsConfigured(st)) {
        laneConfigured(lane, st);
        return;
    }
    if (++s->verifyTries < LANE_MANAGER_VERIFY_TRIES) {
        s->nextVerifyMs = HAL_GetTick() + LANE_MANAGER_VERIFY_RETRY_MS;
        return;
    }
    if (++s->pushTries < LANE_MANAGER_PUSH_TRIES) {
        laneLog(HP_LOG_WARN, "REPUSH", lane, "flags", st->flags);
        laneStartPush(lane, PUSH_CONFIG);
        return;
    }
    s->state = SLOT_FAILED;
    lanePublishState(lane, (uint8_t)HP_LANE_STATE_FAULT, CTRL_ERR_LANE_NACK);
    laneLog(HP_LOG_ERROR, "PUSH FAILED", lane, "flags", st->flags);
}


/* --- Polling results ----------------------------------------------------------- */

static void laneFifoResult(uint8_t lane, uint16_t rxLen)
{
    laneSlot_t      *s = &laneSlots[lane];
    laneFifoHeader_t hdr;
    laneEvent_t      ev[LANE_FIFO_READ_MAX];
    uint8_t          n = 0U;

    if (laneLogicParseFifo(s->rx, rxLen, &hdr, ev, &n) != CTRL_OK) {
        laneLog(HP_LOG_WARN, "FIFO BAD", lane, "len", rxLen);
        return;
    }
    for (uint8_t i = 0U; i < n; i++) {
        const laneSeqVerdict_e v = laneLogicSeqCheck(s->seqKnown, s->nextSeq, ev[i].seq);
        if (v == LANE_SEQ_DUPLICATE) {
            s->seqDups++;   /* re-sent after a cut read: already published */
            continue;
        }
        if (v == LANE_SEQ_GAP) {
            const uint16_t gap = laneLogicSeqGap(s->nextSeq, ev[i].seq);
            s->seqGaps += gap;
            laneLog(HP_LOG_WARN, "SEQ GAP", lane, "lost", gap);
        } else if (v == LANE_SEQ_RESYNC) {
            laneLog(HP_LOG_WARN, "SEQ BACK", lane, "seq", ev[i].seq);
        }
        s->seqKnown = true;
        s->nextSeq  = (uint16_t)(ev[i].seq + 1U);

        hpEvtCrossing_t c;
        laneLogicCrossing(lane, &ev[i], &c);
        (void)hostManagerPublishEvent(HP_MSG_EVT_CROSSING, &c, sizeof(c));
    }
    s->pollAgain = hdr.remaining != 0U;
}

static void laneRawResult(uint8_t lane, uint16_t rxLen)
{
    laneSlot_t     *s = &laneSlots[lane];
    laneRawHeader_t hdr;
    uint8_t         n = 0U;
    struct __attribute__((packed)) {
        hpStrRawSamples_t head;
        uint16_t          samples[LANE_RAW_READ_MAX];
    } str;
    _Static_assert(HP_STR_RAW_SAMPLES_BYTES + LANE_RAW_READ_MAX * 2U <= HP_MAX_PAYLOAD, "raw chunk fits a frame");

    uint16_t samples[LANE_RAW_READ_MAX];
    if (laneLogicParseRaw(s->rx, rxLen, &hdr, samples, &n) != CTRL_OK || n == 0U) {
        s->rawAgain = false;
        return;
    }
    memcpy(str.samples, samples, (size_t)n * sizeof(uint16_t));
    memset(&str.head, 0, sizeof(str.head));
    str.head.lane     = lane;
    str.head.chunkSeq = hdr.chunkSeq;
    str.head.periodUs = hdr.periodUs;
    str.head.firstUs  = hdr.firstUs;
    hostManagerPublishStream(HP_MSG_STR_RAW_SAMPLES, &str,
                             (uint16_t)(HP_STR_RAW_SAMPLES_BYTES + (uint16_t)n * sizeof(uint16_t)));
    s->rawAgain = n == LANE_RAW_READ_MAX;
}

static void laneHealthResult(uint8_t lane, const laneStatus_t *st)
{
    laneSlot_t *s = &laneSlots[lane];
    uint8_t     out[3];
    const uint8_t n = laneLogicHealth(&s->health, st, out);
    for (uint8_t i = 0U; i < n; i++) {
        const ctrlStatus_e why = (out[i] == (uint8_t)HP_LANE_STATE_FAULT) ? (ctrlStatus_e)st->lastError : CTRL_OK;
        lanePublishState(lane, out[i], why);
        laneLog(HP_LOG_WARN, "HEALTH", lane, "state", out[i]);
    }
    if (st->droppedEvents != s->lastDropped) {
        laneLog(HP_LOG_WARN, "FIFO DROPPED", lane, "total", st->droppedEvents);
        s->lastDropped = st->droppedEvents;
    }
    if (st->syncShifts != s->lastShifts) {
        laneLog(HP_LOG_WARN, "SYNC SHIFT", lane, "total", st->syncShifts);
        s->lastShifts = st->syncShifts;
    }
    if (s->state == SLOT_CONFIGURED && laneLogicNeedsPush(st)) {
        laneLog(HP_LOG_WARN, "LOST CONFIG", lane, "state", st->state);
        laneStartPush(lane, PUSH_LANE_ID);
    } else if (s->state == SLOT_CONFIGURED && laneLogicNeedsModePush(st)) {
        laneLog(HP_LOG_WARN, "BUS IDLE SAFE", lane, "mode", laneModeFor(lane));
        laneCommand(lane, LANE_CMD_SAMPLING, laneModeFor(lane));
    }
}


/* --- Join -------------------------------------------------------------------------- */

static uint8_t laneKnownMask(void)
{
    uint8_t m = 0U;
    for (uint8_t n = 1U; n <= LANE_MAX_LANES; n++) {
        if (laneSlots[n].state != SLOT_ABSENT) {
            m |= LANE_LOGIC_MASK(n);
        }
    }
    return m;
}

static void laneJoinDecide(void)
{
    if (laneJoinProbeLost) {
        /* A known lane went unprobed (queue full): deciding now would mark
         * it LEFT and hand out its index. Try the join again next pass. */
        laneJoin = JOIN_IDLE;
        laneHotJoins++;
        return;
    }

    laneJoinDecision_t d;
    laneLogicDecideJoin(laneKnownMask(), laneJoinAnswering, &d);

    for (uint8_t n = 1U; n <= LANE_MAX_LANES; n++) {
        if ((d.releaseMask & LANE_LOGIC_MASK(n)) != 0U) {
            laneCommand(n, LANE_CMD_RELEASE_CHAIN, 0U);   /* forget the stale address, rejoin in order */
        }
        if ((d.leftMask & LANE_LOGIC_MASK(n)) != 0U) {
            laneMarkLeft(n, CTRL_ERR_LANE_ABSENT);
        }
    }
    if (d.index == 0U) {
        laneLog(HP_LOG_WARN, "NO SLOT FOR A 7TH", 7U, NULL, 0);
        laneJoin = JOIN_IDLE;
        return;
    }

    i3cCtrlXfer_t x;
    memset(&x, 0, sizeof(x));
    x.op   = I3C_CTRL_OP_ENTDAA;
    x.addr = LANE_DYN_ADDR(d.index);
    x.tag  = LANE_TAG(0U, OP_ENTDAA, laneBusGen);
    x.done = laneDone;
    laneJoinIndex = d.index;
    laneJoin = (i3cCtrlSubmit(&x, true) == CTRL_OK) ? JOIN_DAA : JOIN_IDLE;
}

static void laneJoinStart(void)
{
    laneJoinAnswering = 0U;
    laneJoinProbes    = 0U;
    laneJoinProbeLost = false;
    laneJoin          = JOIN_PROBING;
    for (uint8_t n = 1U; n <= LANE_MAX_LANES; n++) {
        if (laneSlots[n].state == SLOT_ABSENT) {
            continue;
        }
        if (laneSubmit(n, OP_PROBE, LANE_REG_STATUS, NULL, 0U, laneSlots[n].probe,
                       (uint16_t)sizeof(laneStatus_t), NULL, true) == CTRL_OK) {
            laneJoinProbes++;
        } else {
            laneJoinProbeLost = true;
        }
    }
    if (laneJoinProbes == 0U) {
        laneJoinDecide();
    }
}


/* --- Completion (main loop, from i3cCtrlRun) ------------------------------------------ */

static void laneBusDone(uint8_t op, ctrlStatus_e status, uint16_t rxLen)
{
    if (op == OP_RSTDAA) {
        if (status != CTRL_OK) {
            laneLog(HP_LOG_WARN, "RSTDAA FAILED", 0U, "status", status);
        }
        laneBus   = BUS_CHAIN_LOW;
        laneBusMs = HAL_GetTick();
        return;
    }
    if (op == OP_ENTDAA) {
        laneJoin = JOIN_IDLE;
        if (status != CTRL_OK || rxLen == 0U) {
            laneLog(HP_LOG_DEBUG, "DAA NOBODY", laneJoinIndex, "status", status);
            return;
        }
        if (rxLen > 1U) {
            laneLog(HP_LOG_ERROR, "DAA MULTI", laneJoinIndex, "targets", rxLen);
        }
        laneJoined(laneJoinIndex);
        laneEnumDeadlineMs = HAL_GetTick() + LANE_MANAGER_ENUM_NEXT_MS;
    }
}

static void laneDone(uint16_t tag, ctrlStatus_e status, uint16_t rxLen)
{
    const uint8_t lane = LANE_TAG_LANE(tag);
    const uint8_t op   = LANE_TAG_OP(tag);

    if (lane == 0U) {
        if (LANE_TAG_GEN(tag) == laneBusGen) {
            laneBusDone(op, status, rxLen);
        }
        return;
    }
    if (lane > LANE_MAX_LANES) {
        return;
    }
    laneSlot_t *s = &laneSlots[lane];

    if (op == OP_PROBE) {
        if (status == CTRL_OK && rxLen >= sizeof(laneStatus_t)) {
            laneJoinAnswering |= LANE_LOGIC_MASK(lane);
        }
        if (laneJoinProbes > 0U && --laneJoinProbes == 0U && laneJoin == JOIN_PROBING) {
            laneJoinDecide();
        }
        return;
    }

    if (LANE_TAG_GEN(tag) != s->gen) {
        return;   /* for an incarnation that LEFT */
    }
    if (s->state == SLOT_ABSENT) {
        return;   /* a write sent after LEFT (RELEASE_CHAIN): nothing to count */
    }

    const bool sequenced = op != OP_WRITE && op != OP_SCHED && op != OP_SCHED_VERIFY
                           && op != OP_REQ_STATUS && op != OP_REQ_SIGNAL;
    if (sequenced) {
        s->opPending = false;
    }

    if (op == OP_SCHED) {
        if (s->schedWrites > 0U) {
            s->schedWrites--;
        }
        if (status != CTRL_OK) {
            s->schedResult = LANE_SCHED_RESULT_FAILED;
        }
    } else if (op == OP_SCHED_VERIFY && s->schedResult == LANE_SCHED_RESULT_PENDING) {
        laneStatus_t st;
        memcpy(&st, s->sched, sizeof(st));
        const laneSchedVerify_e v = laneLogicSchedVerify(status == CTRL_OK && rxLen >= sizeof(st),
                                                         st.schedulePending, s->schedExpect,
                                                         ++s->schedVerifyTries);
        if (v == LANE_SCHED_VERIFY_RETRY) {
            s->schedVerifyDue   = true;   /* the lane has not applied the writes yet */
            s->schedVerifyAtMs  = HAL_GetTick() + LANE_LOGIC_SCHED_VERIFY_RETRY_MS;
        } else {
            s->schedResult = (v == LANE_SCHED_VERIFY_OK) ? LANE_SCHED_RESULT_OK : LANE_SCHED_RESULT_FAILED;
        }
    } else if (op == OP_REQ_STATUS || op == OP_REQ_SIGNAL) {
        if (laneReqLane != lane || laneReqDone) {
            return;
        }
        if (status != CTRL_OK) {
            laneReqDone   = true;
            laneReqStatus = status;
        } else if (op == OP_REQ_STATUS) {
            memcpy(&laneReqSt, laneReqBuf, sizeof(laneReqSt));
            if (laneSubmit(lane, OP_REQ_SIGNAL, LANE_REG_SIGNAL, NULL, 0U, laneReqBuf,
                           (uint16_t)sizeof(laneSignal_t), NULL, false) != CTRL_OK) {
                laneReqDone   = true;
                laneReqStatus = CTRL_ERR_BUSY;
            }
        } else {
            laneSignal_t sig;
            memcpy(&sig, laneReqBuf, sizeof(sig));
            laneLogicStatusRsp(lane, &laneReqSt, &sig, &laneReqRsp);
            laneReqDone   = true;
            laneReqStatus = CTRL_OK;
        }
    }

    if (status != CTRL_OK) {
        if (++s->failStreak >= LANE_MANAGER_FAIL_LIMIT) {
            laneMarkLeft(lane, status);   /* the downstream will fail too; the hot-joins bring them back */
            if (status != CTRL_ERR_LANE_ABSENT) {
                /* It still acknowledged its address: a lane that keeps it
                 * never hot-joins again. Make it rejoin (priority queue, so
                 * this goes out before any later probe or ENTDAA). */
                const laneCommand_t c = { .cmd = LANE_CMD_RELEASE_CHAIN, .arg = 0U };
                (void)laneSubmit(lane, OP_WRITE, LANE_REG_COMMAND, &c, (uint8_t)sizeof(c), NULL, 0U, NULL, true);
            }
        }
        return;   /* a sequenced step is retried on the next pass */
    }
    s->failStreak = 0U;

    switch (op) {
    case OP_ID:
        memcpy(&s->id, s->rx, sizeof(s->id));
        s->idValid = true;
        if (s->id.protoMajor != LANE_PROTO_MAJOR) {
            s->state = SLOT_FAILED;
            lanePublishState(lane, (uint8_t)HP_LANE_STATE_FAULT, CTRL_ERR_PROTO_MISMATCH);
            laneLog(HP_LOG_ERROR, "PROTO MISMATCH", lane, "major", s->id.protoMajor);
        } else {
            s->pushStep = PUSH_LANE_ID;
        }
        break;
    case OP_LANE_ID: s->pushStep = PUSH_CONFIG; break;
    case OP_CONFIG:  s->pushStep = PUSH_ALIGN;  break;
    case OP_ALIGN:   s->pushStep = PUSH_MODE;   s->lastAlignMs = HAL_GetTick(); break;
    case OP_MODE:    s->pushStep = PUSH_VERIFY; s->nextVerifyMs = HAL_GetTick(); break;
    case OP_VERIFY: {
        laneStatus_t st;
        memcpy(&st, s->rx, sizeof(st));
        laneVerifyResult(lane, &st);
        break;
    }
    case OP_FIFO:   laneFifoResult(lane, rxLen); break;
    case OP_RAW:    laneRawResult(lane, rxLen);  break;
    case OP_HEALTH: {
        laneStatus_t st;
        memcpy(&st, s->rx, sizeof(st));
        laneHealthResult(lane, &st);
        break;
    }
    default:
        break;
    }
}


/* --- Per-lane scheduler ------------------------------------------------------------------ */

static void laneService(uint8_t lane, uint32_t now)
{
    laneSlot_t *s = &laneSlots[lane];
    if (s->schedVerifyDue && s->state == SLOT_CONFIGURED && (int32_t)(now - s->schedVerifyAtMs) >= 0) {
        s->schedVerifyDue = false;   /* not sequenced: goes out beside a running step */
        if (laneSubmit(lane, OP_SCHED_VERIFY, LANE_REG_STATUS, NULL, 0U, s->sched,
                       (uint16_t)sizeof(laneStatus_t), NULL, true) != CTRL_OK) {
            s->schedResult = LANE_SCHED_RESULT_FAILED;
        }
    }
    if (s->state == SLOT_ABSENT || s->opPending) {
        return;
    }
    if (s->state == SLOT_JOINED) {
        lanePushNext(lane);
        return;
    }

    if (s->state == SLOT_CONFIGURED && (s->pollAgain || now - s->lastPollMs >= LANE_MANAGER_POLL_MS)) {
        s->lastPollMs = now;
        s->pollAgain  = false;
        laneStep(lane, OP_FIFO, LANE_REG_EVENT_FIFO, NULL, 0U, s->rx, (uint16_t)LANE_FIFO_READ_BYTES, NULL);
    } else if (s->state == SLOT_CONFIGURED && lane == laneRawLane
               && (s->rawAgain || now - s->lastRawMs >= LANE_MANAGER_RAW_POLL_MS)) {
        s->lastRawMs = now;
        s->rawAgain  = false;
        laneStep(lane, OP_RAW, LANE_REG_RAW_FIFO, NULL, 0U, s->rx, (uint16_t)LANE_RAW_READ_BYTES, NULL);
    } else if ((int32_t)(now - s->lastHealthMs) >= (int32_t)LANE_MANAGER_HEALTH_MS) {
        s->lastHealthMs = now;
        laneStep(lane, OP_HEALTH, LANE_REG_STATUS, NULL, 0U, s->rx, (uint16_t)sizeof(laneStatus_t), NULL);
    } else if (s->state == SLOT_CONFIGURED && (int32_t)(now - s->lastAlignMs) >= (int32_t)LANE_MANAGER_ALIGN_MS) {
        s->lastAlignMs = now;
        laneSendAlign(lane, OP_ALIGN_CHECK);
    }
}


/* --- Bus start / stop ---------------------------------------------------------------------- */

static void laneAllLeft(ctrlStatus_e reason)
{
    for (uint8_t n = 1U; n <= LANE_MAX_LANES; n++) {
        laneMarkLeft(n, reason);
    }
    laneJoin       = JOIN_IDLE;
    laneJoinProbes = 0U;
    laneHotJoins   = 0U;
    laneBusGen++;
}

/* CHAIN_OUT low, RSTDAA; CHAIN_OUT goes high LANE_MANAGER_CHAIN_LOW_MS later. */
static ctrlStatus_e laneBusRestart(void)
{
    (void)laneChainSetChainOut(false);
    laneAllLeft(CTRL_OK);
    (void)i3cCtrlTakeHotJoins();

    i3cCtrlXfer_t x;
    memset(&x, 0, sizeof(x));
    x.op   = I3C_CTRL_OP_CCC;
    x.reg  = 0x06U;   /* RSTDAA, broadcast */
    x.tag  = LANE_TAG(0U, OP_RSTDAA, laneBusGen);
    x.done = laneDone;
    laneBus = BUS_RESETTING;
    return i3cCtrlSubmit(&x, true);
}

static ctrlStatus_e laneBusStart(void)
{
    CTRL_RETURN_IF_ERR(laneChainReleaseBusPins());
    ctrlStatus_e status = i3cCtrlStart();
    if (status != CTRL_OK) {
        (void)laneChainParkBusPins();
        return status;
    }
    return laneBusRestart();
}

static ctrlStatus_e laneManagerPowerOff(void)
{
    laneAllLeft(CTRL_ERR_LANE_POWER);
    laneRawLane = 0U;
    i3cCtrlStop();
    laneBus      = BUS_DOWN;
    laneEnumOpen = false;

    /* Signals before rail. Every step runs even if an earlier one fails: a
     * half-done power-down is worse than a reported error. */
    ctrlStatus_e status = sysTimeSetSyncOutput(false);
    ctrlStatus_e park   = laneChainParkBusPins();
    laneChainSetRail(false);
    laneManagerPower = LANE_POWER_OFF;
    return (status != CTRL_OK) ? status : park;
}


/* --- API ------------------------------------------------------------------------------------ */

ctrlStatus_e laneManagerInit(void)
{
    memset(laneSlots, 0, sizeof(laneSlots));
    const settingsData_t *set = settingsStoreData();
    memcpy(laneTable, set->laneConfig, sizeof(laneTable));
    memcpy(laneTableMode, set->laneSampling, sizeof(laneTableMode));
    for (uint8_t n = 1U; n <= LANE_MAX_LANES; n++) {
        if (laneLogicCheckConfig(&laneTable[n - 1U]) != CTRL_OK || laneTableMode[n - 1U] > (uint8_t)HP_SAMPLING_ON) {
            laneLogicDefaultConfig(n, &laneTable[n - 1U]);
            laneTableMode[n - 1U] = (uint8_t)HP_SAMPLING_OFF;
        }
    }
    laneRawLane = 0U;
    laneReqLane = 0U;
    /* lane_chain already parked everything at its init; make the state agree. */
    return laneManagerPowerOff();
}

ctrlStatus_e laneManagerSetPower(bool on)
{
    if (!on) {
        return laneManagerPowerOff();
    }
    if (laneManagerPower == LANE_POWER_OFF) {
        laneChainSetRail(true);
        laneManagerRailOnMs = HAL_GetTick();
        laneManagerPower    = LANE_POWER_SETTLING;
    }
    return CTRL_OK;
}

ctrlStatus_e laneManagerRun(void)
{
    const uint32_t now    = HAL_GetTick();
    ctrlStatus_e   result = CTRL_OK;

    if (laneManagerPower == LANE_POWER_SETTLING && now - laneManagerRailOnMs >= LANE_MANAGER_SETTLE_MS) {
        ctrlStatus_e status = sysTimeSetSyncOutput(true);
        if (status != CTRL_OK) {
            return status;               /* stays SETTLING: retried next pass */
        }
        laneManagerPower = LANE_POWER_ON;
        result = laneBusStart();
        if (result != CTRL_OK) {
            laneLog(HP_LOG_ERROR, "BUS START FAILED", 0U, "status", result);
        }
    }
    if (laneManagerPower != LANE_POWER_ON || laneBus == BUS_DOWN) {
        return result;
    }

    i3cCtrlRun();
    if (!i3cCtrlIsRunning()) {
        laneBus = BUS_DOWN;      /* recovery gave up: LANE_SCAN or a power cycle restarts it */
        laneAllLeft(CTRL_ERR_I3C);
        return CTRL_ERR_I3C;
    }

    if (laneBus == BUS_CHAIN_LOW && now - laneBusMs >= LANE_MANAGER_CHAIN_LOW_MS) {
        (void)laneChainSetChainOut(true);
        laneBus            = BUS_UP;
        laneEnumOpen       = true;
        laneEnumDeadlineMs = now + LANE_MANAGER_ENUM_FIRST_MS;
        laneLog(HP_LOG_INFO, "CHAIN UP", 0U, NULL, 0);
    }
    if (laneBus != BUS_UP) {
        return result;
    }

    laneHotJoins += i3cCtrlTakeHotJoins();
    if (laneJoin == JOIN_IDLE && laneHotJoins > 0U) {
        laneHotJoins--;
        laneJoinStart();
    }
    if (laneEnumOpen && (int32_t)(now - laneEnumDeadlineMs) >= 0) {
        laneEnumOpen = false;
        laneLog(HP_LOG_INFO, "ENUM DONE", laneManagerPresentCount(), NULL, 0);
    }

    for (uint8_t n = 1U; n <= LANE_MAX_LANES; n++) {
        laneService(n, now);
    }
    return result;
}

bool laneManagerIsPowered(void)
{
    return laneManagerPower == LANE_POWER_ON;
}

bool laneManagerIsRailOn(void)
{
    return laneManagerPower != LANE_POWER_OFF;
}

ctrlStatus_e laneManagerScan(void)
{
    if (laneManagerPower != LANE_POWER_ON) {
        return CTRL_ERR_LANE_POWER;
    }
    if (laneBus == BUS_DOWN) {
        return laneBusStart();
    }
    return laneBusRestart();
}

uint8_t laneManagerConfiguredMask(void)
{
    uint8_t m = 0U;
    for (uint8_t n = 1U; n <= LANE_MAX_LANES; n++) {
        if (laneSlots[n].state == SLOT_CONFIGURED) {
            m |= LANE_LOGIC_MASK(n);
        }
    }
    return m;
}

uint8_t laneManagerPresentCount(void)
{
    uint8_t n = 0U;
    while (n < LANE_MAX_LANES && laneSlots[n + 1U].state != SLOT_ABSENT) {
        n++;
    }
    return n;
}

bool laneManagerNodeInfo(uint8_t lane, hpNodeInfo_t *info)
{
    if (!laneOnBus(lane)) {
        return false;
    }
    const laneSlot_t *s = &laneSlots[lane];
    laneId_t id;
    memset(&id, 0, sizeof(id));
    if (s->idValid) {
        id = s->id;
    } else {
        id.boardType = LANE_BOARD_TYPE_LANE;   /* joined, ID not read yet */
    }
    uint8_t state = (uint8_t)HP_NODE_STATE_FAULT;           /* push gave up, wrong protocol */
    if (s->state == SLOT_CONFIGURED) {
        state = (uint8_t)HP_NODE_STATE_OK;
    } else if (s->state == SLOT_JOINED) {
        state = (uint8_t)HP_NODE_STATE_UPDATING;             /* config push in progress */
    }
    laneLogicNodeInfo(lane, &id, state, info);
    return true;
}

static ctrlStatus_e laneCheckMask(uint8_t mask)
{
    return (mask == 0U || (mask & (uint8_t)~LANE_LOGIC_ALL_MASK) != 0U) ? CTRL_ERR_INVALID_ARG : CTRL_OK;
}

ctrlStatus_e laneManagerGetConfig(uint8_t lane, hpLaneConfig_t *cfg)
{
    if (lane < 1U || lane > LANE_MAX_LANES) {
        return CTRL_ERR_INVALID_ARG;
    }
    *cfg = laneTable[lane - 1U];
    return CTRL_OK;
}

ctrlStatus_e laneManagerSetConfig(uint8_t laneMask, const hpLaneConfig_t *cfg, bool save)
{
    CTRL_RETURN_IF_ERR(laneCheckMask(laneMask));
    CTRL_RETURN_IF_ERR(laneLogicCheckConfig(cfg));
    for (uint8_t n = 1U; n <= LANE_MAX_LANES; n++) {
        if ((laneMask & LANE_LOGIC_MASK(n)) == 0U) {
            continue;
        }
        laneTable[n - 1U] = *cfg;
        if (laneOnBus(n)) {
            const laneConfig_t c = laneConfigFor(n);
            laneWrite(n, LANE_REG_CONFIG, &c, (uint8_t)sizeof(c));
        }
    }
    if (save) {
        settingsStoreSetLaneTable(laneTable, laneTableMode);
    }
    return CTRL_OK;
}

ctrlStatus_e laneManagerSetSampling(uint8_t laneMask, uint8_t mode)
{
    CTRL_RETURN_IF_ERR(laneCheckMask(laneMask));
    if (mode > (uint8_t)HP_SAMPLING_RAW) {
        return CTRL_ERR_INVALID_ARG;
    }
    if (mode == (uint8_t)HP_SAMPLING_RAW) {
        return CTRL_ERR_INVALID_ARG;   /* RAW belongs to LANE_RAW_STREAM, one lane at a time */
    }
    for (uint8_t n = 1U; n <= LANE_MAX_LANES; n++) {
        if ((laneMask & LANE_LOGIC_MASK(n)) == 0U) {
            continue;
        }
        laneTableMode[n - 1U] = mode;
        if (laneOnBus(n) && n != laneRawLane) {
            laneCommand(n, LANE_CMD_SAMPLING, mode);
        }
    }
    return CTRL_OK;
}

void laneManagerArmSampling(uint8_t laneMask)
{
    for (uint8_t n = 1U; n <= LANE_MAX_LANES; n++) {
        if ((laneMask & LANE_LOGIC_MASK(n)) != 0U && laneTableMode[n - 1U] == (uint8_t)HP_SAMPLING_OFF) {
            laneTableMode[n - 1U] = (uint8_t)HP_SAMPLING_ON;   /* the lane switched itself at T0 */
        }
    }
}

ctrlStatus_e laneManagerSetLed(uint8_t laneMask, const laneLed_t *led)
{
    CTRL_RETURN_IF_ERR(laneCheckMask(laneMask));
    bool any = false;
    for (uint8_t n = 1U; n <= LANE_MAX_LANES; n++) {
        if ((laneMask & LANE_LOGIC_MASK(n)) != 0U && laneOnBus(n)) {
            laneWrite(n, LANE_REG_LED, led, (uint8_t)sizeof(*led));
            any = true;
        }
    }
    return any ? CTRL_OK : CTRL_ERR_LANE_ABSENT;
}

ctrlStatus_e laneManagerCalibrate(uint8_t laneMask)
{
    CTRL_RETURN_IF_ERR(laneCheckMask(laneMask));
    bool any = false;
    for (uint8_t n = 1U; n <= LANE_MAX_LANES; n++) {
        if ((laneMask & LANE_LOGIC_MASK(n)) != 0U && laneOnBus(n)) {
            laneCommand(n, LANE_CMD_CALIBRATE, 0U);
            any = true;
        }
    }
    return any ? CTRL_OK : CTRL_ERR_LANE_ABSENT;
}

static void laneRawStop(void)
{
    const uint8_t lane = laneRawLane;
    laneRawLane = 0U;
    if (laneOnBus(lane)) {
        const laneConfig_t c = laneConfigFor(lane);   /* the table's decimation again */
        laneWrite(lane, LANE_REG_CONFIG, &c, (uint8_t)sizeof(c));
        laneCommand(lane, LANE_CMD_SAMPLING, laneRawPrevMode);
    }
}

ctrlStatus_e laneManagerRawStream(uint8_t lane, uint16_t decimation)
{
    if (lane > LANE_MAX_LANES) {
        return CTRL_ERR_INVALID_ARG;
    }
    if (lane == 0U) {
        if (laneRawLane != 0U) {
            laneRawStop();
        }
        return CTRL_OK;
    }
    if (decimation == 0U) {
        return CTRL_ERR_INVALID_ARG;
    }
    if (laneSlots[lane].state != SLOT_CONFIGURED) {
        return CTRL_ERR_LANE_ABSENT;
    }
    if (laneRawLane != 0U && laneRawLane != lane) {
        laneRawStop();
    }
    if (laneRawLane != lane) {
        laneRawPrevMode = laneTableMode[lane - 1U];
    }
    laneRawLane       = lane;
    laneRawDecimation = decimation;
    const laneConfig_t c = laneConfigFor(lane);
    laneWrite(lane, LANE_REG_CONFIG, &c, (uint8_t)sizeof(c));
    laneCommand(lane, LANE_CMD_SAMPLING, LANE_SAMPLING_RAW);
    return CTRL_OK;
}

ctrlStatus_e laneManagerStatusRequest(uint8_t lane)
{
    if (lane < 1U || lane > LANE_MAX_LANES) {
        return CTRL_ERR_INVALID_ARG;
    }
    if (!laneOnBus(lane)) {
        return CTRL_ERR_LANE_ABSENT;
    }
    if (laneReqLane != 0U && !laneReqDone) {
        return CTRL_ERR_BUSY;
    }
    laneReqLane = lane;
    laneReqDone = false;
    ctrlStatus_e status = laneSubmit(lane, OP_REQ_STATUS, LANE_REG_STATUS, NULL, 0U, laneReqBuf,
                                     (uint16_t)sizeof(laneStatus_t), NULL, false);
    if (status != CTRL_OK) {
        laneReqLane = 0U;
    }
    return status;
}

bool laneManagerStatusPoll(hpLaneStatusRsp_t *rsp, ctrlStatus_e *status)
{
    if (laneReqLane == 0U || !laneReqDone) {
        return false;
    }
    *status = laneReqStatus;
    if (laneReqStatus == CTRL_OK) {
        *rsp = laneReqRsp;
    }
    laneReqLane = 0U;
    return true;
}

ctrlStatus_e laneManagerSchedule(uint8_t lane, const laneSchedule_t *action)
{
    if (lane < 1U || lane > LANE_MAX_LANES || laneSlots[lane].state != SLOT_CONFIGURED) {
        return CTRL_ERR_LANE_ABSENT;
    }
    laneSlot_t *s = &laneSlots[lane];
    CTRL_RETURN_IF_ERR(laneSubmit(lane, OP_SCHED, LANE_REG_SCHEDULE, action, (uint8_t)sizeof(*action),
                                  NULL, 0U, NULL, true));
    s->schedWrites++;
    if (s->schedResult == LANE_SCHED_RESULT_NONE) {
        s->schedResult = LANE_SCHED_RESULT_PENDING;
    }
    return CTRL_OK;
}

ctrlStatus_e laneManagerScheduleVerify(uint8_t lane, uint8_t expectPending)
{
    if (lane < 1U || lane > LANE_MAX_LANES || laneSlots[lane].state != SLOT_CONFIGURED) {
        return CTRL_ERR_LANE_ABSENT;
    }
    laneSlot_t *s = &laneSlots[lane];
    s->schedExpect      = expectPending;
    s->schedVerifyTries = 0U;
    s->schedVerifyDue   = false;
    if (s->schedResult != LANE_SCHED_RESULT_FAILED) {
        s->schedResult = LANE_SCHED_RESULT_PENDING;
    }
    return laneSubmit(lane, OP_SCHED_VERIFY, LANE_REG_STATUS, NULL, 0U, s->sched,
                      (uint16_t)sizeof(laneStatus_t), NULL, true);
}

laneSchedResult_e laneManagerScheduleResult(uint8_t lane)
{
    if (lane < 1U || lane > LANE_MAX_LANES) {
        return LANE_SCHED_RESULT_NONE;
    }
    if (laneSlots[lane].state == SLOT_ABSENT) {
        return LANE_SCHED_RESULT_FAILED;
    }
    return (laneSchedResult_e)laneSlots[lane].schedResult;
}

void laneManagerScheduleForget(void)
{
    for (uint8_t n = 1U; n <= LANE_MAX_LANES; n++) {
        laneSlots[n].schedResult    = LANE_SCHED_RESULT_NONE;
        laneSlots[n].schedWrites    = 0U;
        laneSlots[n].schedVerifyDue = false;
    }
}
