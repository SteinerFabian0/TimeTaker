/*
 * Car detection. See lane_detect.h for the rules.
 */

#include "lane_detect.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "lane_protocol.h"

/* A holdoff anchor older than this is forgotten, so a TIM2 wrap (71 min)
 * cannot bring it back into a window. Holdoffs are at most 10 min (lane_regs). */
#define DETECT_HOLDOFF_FORGET_US  (1UL << 30)

/* Compiled defaults: the control module's defaults (host protocol golden
 * laneConfig), so a lane behaves the same before and after its first CONFIG. */
#define DETECT_DEFAULT_THR_DROP_PERMILLE  300U
#define DETECT_DEFAULT_HYST_PERMILLE      100U
#define DETECT_DEFAULT_MERGE_GAP_US       30000U
#define DETECT_DEFAULT_BLOCK_TIMEOUT_US   500000U
#define DETECT_DEFAULT_MIN_DWELL_US       2000U
#define DETECT_DEFAULT_SATURATION_RAW     65535U

void laneDetectDefaultConfig(laneDetectConfig_t *cfg)
{
    cfg->thrDropPermille = DETECT_DEFAULT_THR_DROP_PERMILLE;
    cfg->hystPermille    = DETECT_DEFAULT_HYST_PERMILLE;
    cfg->mergeGapUs      = DETECT_DEFAULT_MERGE_GAP_US;
    cfg->blockTimeoutUs  = DETECT_DEFAULT_BLOCK_TIMEOUT_US;
    cfg->minDwellUs      = DETECT_DEFAULT_MIN_DWELL_US;
    cfg->holdoffUs       = 0U;
    cfg->saturationRaw   = DETECT_DEFAULT_SATURATION_RAW;
}

static bool laneDetectConfigValid(const laneDetectConfig_t *cfg)
{
    /* hyst < drop also makes drop >= 1. */
    return cfg->thrDropPermille <= 999U
           && cfg->hystPermille < cfg->thrDropPermille
           && cfg->blockTimeoutUs > cfg->mergeGapUs
           && cfg->minDwellUs < cfg->blockTimeoutUs
           && cfg->saturationRaw > 0U;
}

static uint16_t laneDetectBaselineRaw(const laneDetect_t *d)
{
    return (uint16_t)((d->baselineQ8 + 128U) >> 8);
}

static void laneDetectThresholds(laneDetect_t *d)
{
    uint32_t b   = laneDetectBaselineRaw(d);
    uint32_t low = b * (1000U - d->cfg.thrDropPermille) / 1000U;

    d->thrLow  = (uint16_t)low;
    d->thrHigh = (uint16_t)(low + b * d->cfg.hystPermille / 1000U);
}

/* Falling crossing of level between (t0, v0 >= level) and (t1, v1 < level). */
static uint32_t laneDetectCrossFall(uint32_t t0, uint16_t v0, uint32_t t1, uint16_t v1,
                                    uint16_t level)
{
    uint64_t dt = (uint32_t)(t1 - t0);
    if (v0 < level || level <= v1) {
        return t1;   /* no crossing between the samples (stale prev, config change) */
    }
    return t0 + (uint32_t)(dt * (uint32_t)(v0 - level) / (uint32_t)(v0 - v1));
}

/* Rising crossing of level between (t0, v0 <= level) and (t1, v1 > level). */
static uint32_t laneDetectCrossRise(uint32_t t0, uint16_t v0, uint32_t t1, uint16_t v1,
                                    uint16_t level)
{
    uint64_t dt = (uint32_t)(t1 - t0);
    if (v0 > level || level >= v1) {
        return t1;
    }
    return t0 + (uint32_t)(dt * (uint32_t)(level - v0) / (uint32_t)(v1 - v0));
}

static bool laneDetectHoldoffAt(const laneDetect_t *d, uint32_t t)
{
    return d->cfg.holdoffUs > 0U && d->hasHoldoff
           && (uint32_t)(t - d->holdoffEntryT) < d->cfg.holdoffUs;
}

static uint16_t laneDetectDepth(const laneDetect_t *d)
{
    uint16_t b = laneDetectBaselineRaw(d);
    return (d->minRaw < b) ? (uint16_t)(b - d->minRaw) : 0U;
}

static void laneDetectFill(const laneDetect_t *d, uint8_t type, uint32_t dwellUs,
                           laneDetectEvent_t *out)
{
    out->type    = type;
    out->flags   = 0U;
    out->tEntry  = d->entryT;
    out->dwellUs = dwellUs;
    out->depth   = laneDetectDepth(d);

    if (d->merged) {
        out->flags |= LANE_FLAG_MERGED;
    }
    if (d->saturated) {
        out->flags |= LANE_FLAG_SATURATED;
    }
    if (laneDetectHoldoffAt(d, d->entryT)) {
        out->flags |= LANE_FLAG_IN_HOLDOFF;
    }
}

/* The car left at exitT. Returns false for a glitch (too short). */
static bool laneDetectFinishCrossing(laneDetect_t *d, uint32_t exitT, laneDetectEvent_t *out)
{
    uint32_t dwell = exitT - d->entryT;

    if (dwell < d->cfg.minDwellUs) {
        d->glitches++;
        return false;
    }

    laneDetectFill(d, LANE_EVT_CROSSING, dwell, out);
    if ((out->flags & LANE_FLAG_IN_HOLDOFF) == 0U) {
        d->hasHoldoff    = true;
        d->holdoffEntryT = d->entryT;
    }
    d->crossings++;
    return true;
}

/* Starts a pass whose entry crossing is at entryT; sampleSat says whether the
 * samples around the crossing were clipped. */
static void laneDetectStartPass(laneDetect_t *d, uint32_t entryT, uint16_t raw, bool sampleSat)
{
    d->state     = DETECT_STATE_IN_CAR;
    d->entryT    = entryT;
    d->minRaw    = raw;
    d->merged    = false;
    d->saturated = sampleSat;
}

laneStatus_e laneDetectInit(laneDetect_t *d, const laneDetectConfig_t *cfg)
{
    laneStatus_e status = LANE_OK;

    memset(d, 0, sizeof(*d));
    if (cfg != NULL && laneDetectConfigValid(cfg)) {
        d->cfg = *cfg;
    } else {
        laneDetectDefaultConfig(&d->cfg);
        status = LANE_ERR_INVALID_ARG;
    }
    d->state = DETECT_STATE_LEARNING;
    return status;
}

laneStatus_e laneDetectSetConfig(laneDetect_t *d, const laneDetectConfig_t *cfg)
{
    if (cfg == NULL || !laneDetectConfigValid(cfg)) {
        return LANE_ERR_INVALID_ARG;
    }

    d->cfg = *cfg;
    if (d->state != DETECT_STATE_LEARNING) {
        laneDetectThresholds(d);
    }
    return LANE_OK;
}

void laneDetectCalibrate(laneDetect_t *d)
{
    d->state      = DETECT_STATE_LEARNING;
    d->learnSum   = 0U;
    d->learnCount = 0U;
}

void laneDetectSetReference(laneDetect_t *d, uint16_t value)
{
    laneDetectCalibrate(d);   /* abandons a pass, restarts learning */
    d->hasPrev = false;
    if (value == 0U) {
        d->baselineQ8 = 0U;
        d->noiseQ8    = 0U;
        d->reference  = 0U;
        return;
    }
    d->baselineQ8 = (uint32_t)value << 8;
    d->noiseQ8    = 0U;
    d->reference  = value;
    laneDetectThresholds(d);
    d->state = DETECT_STATE_FREE;
}

void laneDetectBreak(laneDetect_t *d)
{
    d->hasPrev = false;
}

bool laneDetectSample(laneDetect_t *d, uint32_t t, uint16_t raw, laneDetectEvent_t *out)
{
    bool     emitted = false;
    bool     sat     = raw >= d->cfg.saturationRaw;
    bool     prevSat = d->hasPrev && d->prevRaw >= d->cfg.saturationRaw;
    uint32_t prevT   = d->hasPrev ? d->prevT : t;
    uint16_t prevRaw = d->hasPrev ? d->prevRaw : raw;

    if (d->hasHoldoff && (uint32_t)(t - d->holdoffEntryT) >= DETECT_HOLDOFF_FORGET_US) {
        d->hasHoldoff = false;
    }

    switch (d->state) {
    case DETECT_STATE_LEARNING:
        d->learnSum += raw;
        d->learnCount++;
        if (d->learnCount >= DETECT_LEARN_SAMPLES) {
            d->baselineQ8 = (d->learnSum * 256U) / d->learnCount;
            d->noiseQ8    = 0U;
            d->reference  = laneDetectBaselineRaw(d);
            laneDetectThresholds(d);
            d->state = DETECT_STATE_FREE;
        }
        break;

    case DETECT_STATE_FREE:
        if (raw < d->thrLow) {
            uint32_t entry = (d->hasPrev && prevRaw >= d->thrLow)
                             ? laneDetectCrossFall(prevT, prevRaw, t, raw, d->thrLow) : t;
            laneDetectStartPass(d, entry, raw, sat || prevSat);
        } else {
            int32_t  err = (int32_t)((uint32_t)raw << 8) - (int32_t)d->baselineQ8;
            uint32_t dev = (uint32_t)((err < 0) ? -err : err);

            d->baselineQ8 = (uint32_t)((int32_t)d->baselineQ8 + err / (1 << DETECT_BASELINE_SHIFT));
            d->noiseQ8    = (uint32_t)((int32_t)d->noiseQ8
                                       + ((int32_t)dev - (int32_t)d->noiseQ8) / (1 << DETECT_NOISE_SHIFT));
            laneDetectThresholds(d);
        }
        break;

    case DETECT_STATE_IN_CAR:
        if (raw < d->minRaw) {
            d->minRaw = raw;
        }
        if (raw > d->thrHigh) {
            d->gapT       = laneDetectCrossRise(prevT, prevRaw, t, raw, d->thrHigh);
            d->saturated |= sat;
            d->state      = DETECT_STATE_GAP;
        }
        break;

    case DETECT_STATE_GAP:
        if (raw < d->thrLow) {
            uint32_t reentry = laneDetectCrossFall(prevT, prevRaw, t, raw, d->thrLow);
            if ((uint32_t)(reentry - d->gapT) <= d->cfg.mergeGapUs) {
                /* Body or wing gap: the same car. */
                d->merged     = true;
                d->saturated |= sat || prevSat;
                d->state      = DETECT_STATE_IN_CAR;
                if (raw < d->minRaw) {
                    d->minRaw = raw;
                }
            } else {
                emitted = laneDetectFinishCrossing(d, d->gapT, out);
                laneDetectStartPass(d, reentry, raw, sat || prevSat);
            }
        } else if ((uint32_t)(t - d->gapT) > d->cfg.mergeGapUs) {
            emitted  = laneDetectFinishCrossing(d, d->gapT, out);
            d->state = DETECT_STATE_FREE;
        }
        break;

    case DETECT_STATE_BLOCKED:
        if (raw < d->minRaw) {
            d->minRaw = raw;
        }
        if (raw > d->thrHigh) {
            d->gapT  = laneDetectCrossRise(prevT, prevRaw, t, raw, d->thrHigh);
            d->state = DETECT_STATE_BLOCKED_GAP;
        }
        break;

    case DETECT_STATE_BLOCKED_GAP:
        if (raw < d->thrLow) {
            d->state = DETECT_STATE_BLOCKED;
            if (raw < d->minRaw) {
                d->minRaw = raw;
            }
        } else if ((uint32_t)(t - d->gapT) > d->cfg.mergeGapUs) {
            laneDetectFill(d, LANE_EVT_CLEARED, d->gapT - d->entryT, out);
            emitted  = true;
            d->state = DETECT_STATE_FREE;
        }
        break;

    default:
        d->state = DETECT_STATE_FREE;
        break;
    }

    /* A pass that never ends is a parked car or a hand: report it once. */
    if (!emitted && (d->state == DETECT_STATE_IN_CAR || d->state == DETECT_STATE_GAP)
        && (uint32_t)(t - d->entryT) >= d->cfg.blockTimeoutUs) {
        laneDetectFill(d, LANE_EVT_BLOCKED, t - d->entryT, out);
        emitted  = true;
        d->state = (d->state == DETECT_STATE_GAP) ? DETECT_STATE_BLOCKED_GAP : DETECT_STATE_BLOCKED;
        d->blockages++;
    }

    d->prevT   = t;
    d->prevRaw = raw;
    d->hasPrev = true;
    return emitted;
}

bool laneDetectInHoldoff(const laneDetect_t *d, uint32_t t)
{
    return laneDetectHoldoffAt(d, t);
}

bool laneDetectIsBlocked(const laneDetect_t *d)
{
    return d->state == DETECT_STATE_IN_CAR || d->state == DETECT_STATE_BLOCKED;
}

bool laneDetectIsCalibrated(const laneDetect_t *d)
{
    return d->state != DETECT_STATE_LEARNING;
}

uint16_t laneDetectBaseline(const laneDetect_t *d)
{
    return laneDetectBaselineRaw(d);
}

uint16_t laneDetectNoise(const laneDetect_t *d)
{
    return (uint16_t)((d->noiseQ8 + 128U) >> 8);
}

uint16_t laneDetectReference(const laneDetect_t *d)
{
    return d->reference;
}

void laneRawWindowInit(laneRawWindow_t *w)
{
    memset(w, 0, sizeof(*w));
}

void laneRawWindowAdd(laneRawWindow_t *w, uint16_t raw)
{
    if (!w->curHas) {
        w->curHas = true;
        w->curMin = raw;
        w->curMax = raw;
        return;
    }
    if (raw < w->curMin) {
        w->curMin = raw;
    }
    if (raw > w->curMax) {
        w->curMax = raw;
    }
}

void laneRawWindowTake(laneRawWindow_t *w, uint16_t last, uint16_t *min, uint16_t *max)
{
    if (w->curHas) {
        if (!w->outHas) {
            w->outHas = true;
            w->outMin = w->curMin;
            w->outMax = w->curMax;
        } else {
            if (w->curMin < w->outMin) {
                w->outMin = w->curMin;
            }
            if (w->curMax > w->outMax) {
                w->outMax = w->curMax;
            }
        }
        w->curHas = false;
    }
    *min = w->outHas ? w->outMin : last;
    *max = w->outHas ? w->outMax : last;
}

void laneRawWindowConfirm(laneRawWindow_t *w)
{
    w->outHas = false;
}
