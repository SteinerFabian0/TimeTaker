/*
 * Lane logic. See lane_logic.h.
 */

#include "lane_logic.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The host protocol mirrors the lane structs it forwards: one layout. */
_Static_assert(sizeof(hpLaneConfig_t) == sizeof(laneConfig_t), "laneConfig layout");
_Static_assert((int)HP_SAMPLING_RAW == (int)LANE_SAMPLING_RAW, "sampling mode values");
_Static_assert((int)HP_LANE_EVT_SENSOR_FAULT == (int)LANE_EVT_SENSOR_FAULT, "event type values");
_Static_assert((int)HP_LANE_BUS_PARKED == (int)LANE_STATE_PARKED, "lane state values");
_Static_assert((int)HP_LANE_FLAG_SATURATED == (int)LANE_FLAG_SATURATED, "event flag values");

#define LANE_LOGIC_DEFAULT_PERIOD_US  1000U

void laneLogicDecideJoin(uint8_t knownMask, uint8_t answeringMask, laneJoinDecision_t *d)
{
    uint8_t contiguous = 0U;
    while (contiguous < LANE_MAX_LANES && (answeringMask & LANE_LOGIC_MASK(contiguous + 1U)) != 0U) {
        contiguous++;
    }
    const uint8_t keep = (uint8_t)((1U << contiguous) - 1U);

    d->index       = (contiguous < LANE_MAX_LANES) ? (uint8_t)(contiguous + 1U) : 0U;
    d->leftMask    = (uint8_t)(knownMask & (uint8_t)~keep & LANE_LOGIC_ALL_MASK);
    d->releaseMask = (uint8_t)(answeringMask & (uint8_t)~keep & LANE_LOGIC_ALL_MASK);
}

ctrlStatus_e laneLogicParseFifo(const uint8_t *buf, uint16_t rxLen, laneFifoHeader_t *hdr,
                                laneEvent_t events[LANE_FIFO_READ_MAX], uint8_t *count)
{
    *count = 0U;
    if (rxLen < sizeof(laneFifoHeader_t)) {
        return CTRL_ERR_BAD_LENGTH;
    }
    memcpy(hdr, buf, sizeof(*hdr));
    if (hdr->count > LANE_FIFO_READ_MAX) {
        return CTRL_ERR_INVALID_ARG;
    }

    const uint16_t whole = (uint16_t)((rxLen - sizeof(laneFifoHeader_t)) / sizeof(laneEvent_t));
    const uint8_t  n     = (hdr->count < whole) ? hdr->count : (uint8_t)whole;
    for (uint8_t i = 0U; i < n; i++) {
        memcpy(&events[i], buf + sizeof(laneFifoHeader_t) + (size_t)i * sizeof(laneEvent_t),
               sizeof(laneEvent_t));
    }
    *count = n;
    return CTRL_OK;
}

ctrlStatus_e laneLogicParseRaw(const uint8_t *buf, uint16_t rxLen, laneRawHeader_t *hdr,
                               uint16_t samples[LANE_RAW_READ_MAX], uint8_t *count)
{
    *count = 0U;
    if (rxLen < sizeof(laneRawHeader_t)) {
        return CTRL_ERR_BAD_LENGTH;
    }
    memcpy(hdr, buf, sizeof(*hdr));
    if (hdr->count > LANE_RAW_READ_MAX) {
        return CTRL_ERR_INVALID_ARG;
    }

    const uint16_t whole = (uint16_t)((rxLen - sizeof(laneRawHeader_t)) / sizeof(uint16_t));
    const uint8_t  n     = (hdr->count < whole) ? hdr->count : (uint8_t)whole;
    memcpy(samples, buf + sizeof(laneRawHeader_t), (size_t)n * sizeof(uint16_t));
    *count = n;
    return CTRL_OK;
}

void laneLogicCrossing(uint8_t lane, const laneEvent_t *ev, hpEvtCrossing_t *out)
{
    memset(out, 0, sizeof(*out));
    out->lane      = lane;
    out->type      = ev->type;
    out->flags     = ev->flags;
    out->tEntryUs  = ev->tEntryUs;
    out->laneSeq   = ev->seq;
    out->dwell10us = ev->dwell10us;
    out->depth     = ev->depth;
}

uint16_t laneLogicSeqGap(uint16_t expected, uint16_t got)
{
    return (uint16_t)(got - expected);
}

laneSeqVerdict_e laneLogicSeqCheck(bool known, uint16_t expected, uint16_t got)
{
    if (!known) {
        return LANE_SEQ_TAKE;
    }
    const uint16_t ahead  = (uint16_t)(got - expected);
    const uint16_t behind = (uint16_t)(expected - got);
    if (ahead == 0U) {
        return LANE_SEQ_TAKE;
    }
    if (behind >= 1U && behind <= LANE_LOGIC_SEQ_DUP_WINDOW) {
        return LANE_SEQ_DUPLICATE;
    }
    return (ahead < 0x8000U) ? LANE_SEQ_GAP : LANE_SEQ_RESYNC;
}

void laneLogicSeqStart(const laneStatus_t *st, bool *known, uint16_t *nextSeq)
{
    const bool dropped = st->droppedEvents != 0U || (st->flags & LANE_STATUS_FIFO_OVERFLOW) != 0U;
    *known   = st->fifoCount < LANE_FIFO_COUNT_MAX && !dropped;
    *nextSeq = (uint16_t)(st->nextEventSeq - st->fifoCount);
}

laneSchedVerify_e laneLogicSchedVerify(bool readOk, uint8_t schedulePending, uint8_t expect, uint8_t tries)
{
    if (!readOk) {
        return LANE_SCHED_VERIFY_FAILED;
    }
    if (schedulePending >= expect) {
        return LANE_SCHED_VERIFY_OK;
    }
    return (tries < LANE_LOGIC_SCHED_VERIFY_TRIES) ? LANE_SCHED_VERIFY_RETRY : LANE_SCHED_VERIFY_FAILED;
}

bool laneLogicNeedsModePush(const laneStatus_t *st)
{
    return (st->flags & LANE_STATUS_BUS_IDLE_SAFE) != 0U;
}

uint8_t laneLogicPushNext(uint8_t done, uint16_t savedReference)
{
    switch (done) {
    case LANE_PUSH_ID:        return (uint8_t)LANE_PUSH_LANE_ID;
    case LANE_PUSH_LANE_ID:   return (uint8_t)LANE_PUSH_CONFIG;
    case LANE_PUSH_CONFIG:    return (savedReference != 0U) ? (uint8_t)LANE_PUSH_REFERENCE : (uint8_t)LANE_PUSH_ALIGN;
    case LANE_PUSH_REFERENCE: return (uint8_t)LANE_PUSH_ALIGN;
    case LANE_PUSH_ALIGN:     return (uint8_t)LANE_PUSH_MODE;
    default:                  return (uint8_t)LANE_PUSH_VERIFY;
    }
}

uint8_t laneLogicHealth(laneHealthView_t *view, const laneStatus_t *st, bool pushDone, uint8_t out[3])
{
    uint8_t n = 0U;

    const bool lostNow    = (st->flags & LANE_STATUS_SYNC_LOST) != 0U;
    const bool faultedNow = st->lastError != 0 || st->state == (uint8_t)LANE_STATE_PARKED;

    if (lostNow && (!view->known || !view->syncLost)) {
        out[n++] = (uint8_t)HP_LANE_STATE_SYNC_LOST;
    } else if (!lostNow && view->known && view->syncLost) {
        out[n++] = (uint8_t)HP_LANE_STATE_SYNC_OK;
    }

    if (faultedNow && (!view->known || !view->faulted || st->lastError != view->lastError)) {
        out[n++] = (uint8_t)HP_LANE_STATE_FAULT;
    } else if (!faultedNow && view->known && view->faulted && pushDone) {
        out[n++] = (uint8_t)HP_LANE_STATE_CONFIGURED;   /* recovered */
    }

    view->known     = true;
    view->syncLost  = lostNow;
    view->faulted   = faultedNow;
    view->lastError = st->lastError;
    return n;
}

bool laneLogicIsConfigured(const laneStatus_t *st)
{
    const uint16_t need = LANE_STATUS_CONFIG_VALID | LANE_STATUS_SYNC_ALIGNED;
    return (st->flags & need) == need
        && (st->state == (uint8_t)LANE_STATE_IDLE || st->state == (uint8_t)LANE_STATE_SAMPLING);
}

bool laneLogicNeedsPush(const laneStatus_t *st)
{
    return (st->flags & LANE_STATUS_CONFIG_VALID) == 0U
        || st->state == (uint8_t)LANE_STATE_UNENUMERATED || st->state == (uint8_t)LANE_STATE_ENUMERATED;
}

ctrlStatus_e laneLogicCheckConfig(const hpLaneConfig_t *cfg)
{
    /* The same rules as the lane's CONFIG write (lane_protocol.h). */
    const bool ok = cfg->samplePeriodUs <= LANE_CFG_SAMPLE_PERIOD_US_MAX
                    && cfg->samplePhaseUs < cfg->samplePeriodUs
                    && cfg->mergeGapUs <= LANE_CFG_MERGE_GAP_US_MAX
                    && cfg->blockTimeoutUs > cfg->mergeGapUs
                    && cfg->blockTimeoutUs <= LANE_CFG_BLOCK_TIMEOUT_US_MAX
                    && cfg->holdoffUs <= LANE_CFG_HOLDOFF_US_MAX
                    && (cfg->holdoffSamplePeriodUs == 0U
                        || (cfg->holdoffSamplePeriodUs >= cfg->samplePeriodUs
                            && cfg->holdoffSamplePeriodUs <= LANE_CFG_HOLDOFF_SAMPLE_PERIOD_US_MAX))
                    && cfg->thrDropPermille <= LANE_CFG_THR_DROP_PERMILLE_MAX
                    && cfg->hystPermille < cfg->thrDropPermille
                    && (uint32_t)cfg->minDwell10us * 10U < cfg->blockTimeoutUs
                    && cfg->rawDecimation >= LANE_CFG_RAW_DECIMATION_MIN
                    && cfg->rawDecimation <= LANE_CFG_RAW_DECIMATION_MAX
                    && cfg->irCurrent <= LANE_CFG_IR_CURRENT_MAX
                    && cfg->psIntegration <= LANE_CFG_PS_INTEGRATION_MAX
                    && cfg->psMultiPulse <= LANE_CFG_PS_MULTI_PULSE_MAX
                    && cfg->reserved == 0U;
    return ok ? CTRL_OK : CTRL_ERR_INVALID_ARG;
}

void laneLogicDefaultConfig(uint8_t lane, hpLaneConfig_t *cfg)
{
    const uint8_t slot = (lane >= 1U && lane <= LANE_MAX_LANES) ? (uint8_t)(lane - 1U) : 0U;

    memset(cfg, 0, sizeof(*cfg));
    cfg->samplePeriodUs        = LANE_LOGIC_DEFAULT_PERIOD_US;
    cfg->samplePhaseUs         = (uint32_t)slot * (LANE_LOGIC_DEFAULT_PERIOD_US / LANE_MAX_LANES);
    cfg->mergeGapUs            = 30000U;
    cfg->blockTimeoutUs        = 500000U;
    cfg->holdoffUs             = 0U;
    cfg->holdoffSamplePeriodUs = 0U;
    cfg->thrDropPermille       = 300U;
    cfg->hystPermille          = 100U;
    cfg->minDwell10us          = 200U;
    cfg->rawDecimation         = 1U;
    cfg->irCurrent             = 7U;
    cfg->psIntegration         = 3U;
    cfg->psMultiPulse          = 0U;
}

ctrlStatus_e laneLogicCalAccept(uint8_t laneMask, uint8_t action, uint8_t configuredMask, uint8_t busyMask,
                                uint8_t *runMask)
{
    *runMask = 0U;
    if (laneMask == 0U || (laneMask & (uint8_t)~LANE_LOGIC_ALL_MASK) != 0U
        || action > (uint8_t)HP_CALIB_FORGET) {
        return CTRL_ERR_INVALID_ARG;
    }
    if ((laneMask & busyMask) != 0U) {
        return CTRL_ERR_BUSY;
    }
    if (action == (uint8_t)HP_CALIB_FORGET) {
        *runMask = laneMask;
        return CTRL_OK;
    }
    const uint8_t run = (uint8_t)(laneMask & configuredMask);
    if (run == 0U) {
        return CTRL_ERR_LANE_ABSENT;
    }
    *runMask = run;
    return CTRL_OK;
}

laneCalPoll_e laneLogicCalPoll(bool *seenClear, bool readOk, uint16_t flags, uint32_t elapsedMs)
{
    if (readOk) {
        if ((flags & LANE_STATUS_CALIBRATED) == 0U) {
            *seenClear = true;
        } else if (*seenClear) {
            return LANE_CAL_LEARNED;
        }
    }
    return (elapsedMs >= LANE_LOGIC_CAL_TIMEOUT_MS) ? LANE_CAL_TIMEOUT : LANE_CAL_POLL_AGAIN;
}

bool laneLogicCalReferenceOk(bool readOk, const laneSignal_t *sig)
{
    return readOk && sig->reference != 0U;
}

uint8_t laneLogicCalSaveResult(bool forget, bool written)
{
    if (!written) {
        return (uint8_t)HP_CALIB_RESULT_NOT_SAVED;
    }
    return forget ? (uint8_t)HP_CALIB_RESULT_FORGOTTEN : (uint8_t)HP_CALIB_RESULT_SAVED;
}

void laneLogicCalEvent(uint8_t lane, uint8_t result, uint16_t reference, uint16_t noise, uint64_t sysUs,
                       hpEvtLaneCalibrated_t *out)
{
    memset(out, 0, sizeof(*out));
    const bool learned = reference != 0U
                         && (result == (uint8_t)HP_CALIB_RESULT_SAVED || result == (uint8_t)HP_CALIB_RESULT_NOT_SAVED);
    out->lane      = lane;
    out->result    = result;
    out->reference = learned ? reference : 0U;
    out->noise     = learned ? noise : 0U;
    out->sysUs     = sysUs;
}

void laneLogicStatusRsp(uint8_t lane, const laneStatus_t *st, const laneSignal_t *sig,
                        hpLaneStatusRsp_t *out)
{
    memset(out, 0, sizeof(*out));
    out->lane          = lane;
    out->state         = st->state;
    out->samplingMode  = st->samplingMode;
    out->fifoCount     = st->fifoCount;
    out->flags         = st->flags;
    out->droppedEvents = st->droppedEvents;
    out->syncShifts    = st->syncShifts;
    out->badWrites     = st->badWrites;
    out->lateActions   = st->lateActions;
    out->lastError     = st->lastError;
    out->resetCause    = st->resetCause;
    out->faultId       = st->faultId;
    out->uptimeMs      = st->uptimeMs;
    out->raw           = sig->raw;
    out->baseline      = sig->baseline;
    out->thrLow        = sig->thrLow;
    out->thrHigh       = sig->thrHigh;
    out->noise         = sig->noise;
    out->reference     = sig->reference;
    out->rawMin        = sig->rawMin;
    out->rawMax        = sig->rawMax;
}

void laneLogicNodeInfo(uint8_t lane, const laneId_t *id, uint8_t nodeState, hpNodeInfo_t *out)
{
    memset(out, 0, sizeof(*out));
    out->nodeClass  = (uint8_t)HP_NODE_LANE;
    out->index      = lane;
    out->boardType  = id->boardType;
    out->state      = nodeState;
    out->fwMajor    = id->fwMajor;
    out->fwMinor    = id->fwMinor;
    out->fwPatch    = id->fwPatch;
    out->protoMinor = id->protoMinor;
    out->fwGitRev   = id->fwGitRev;
    out->imageCrc   = id->imageCrc;
}
