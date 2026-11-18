/*
 * Car detection: proximity samples in, crossing events out.
 *
 * Pure logic: no HAL, no globals, host-tested (test/host_detect). The sense
 * manager feeds it one sample at a time from the main loop and owns the
 * struct; nothing here is reentrant or ISR-safe.
 *
 * Signal: the VCNL4040 looks down at retro foil. The bare foil reflects
 * strongly (high count, the baseline); a car blocks it (the count drops).
 *
 *   LOW  = baseline × (1000 − thrDropPermille) / 1000   "car present" below
 *   HIGH = LOW + baseline × hystPermille / 1000          "car gone" above
 *
 * One pass, guide §3.5 as changed on 2026-11-16:
 *   FREE ──raw < LOW──► IN_CAR ──raw > HIGH──► GAP ──above for > mergeGap──► emit CROSSING
 *                          ▲                    │
 *                          └── raw < LOW within mergeGap (same car: MERGED)
 *   IN_CAR/GAP longer than blockTimeout since entry ──► emit BLOCKED ──► BLOCKED
 *   BLOCKED ──raw > HIGH for > mergeGap──► emit CLEARED ──► FREE
 *
 * - tEntry is the falling crossing of LOW, interpolated linearly between the
 *   two samples around it: the car's nose on the line, the timing reference.
 * - The exit is the rising crossing of HIGH, interpolated the same way; the
 *   event is emitted only once the signal has stayed up for mergeGap, so it
 *   carries dwell (entry to exit) and depth (baseline − minimum).
 * - A pass shorter than minDwell is a glitch: counted, never emitted.
 * - BLOCKED carries the entry and the dwell so far. CLEARED carries the SAME
 *   entry and the whole blocked time as dwell, so tEntry + dwell is when the
 *   line came free.
 * - Holdoff flags, never drops: an event whose entry lies within holdoffUs
 *   of the last unflagged CROSSING's entry gets LANE_FLAG_IN_HOLDOFF. Only an
 *   unflagged CROSSING (re)starts the window, so chatter cannot extend it.
 * - SATURATED: a sample used for the entry, the exit or the depth sat at the
 *   sensor's full scale, so the interpolation or depth is clipped.
 *
 * Baseline: learned as the mean of the first DETECT_LEARN_SAMPLES samples
 * after init or laneDetectCalibrate() (no detection meanwhile; the lane must
 * be free), then tracked by a slow IIR while FREE only. Noise is the IIR of
 * |raw − baseline| while FREE, for SIGNAL.
 *
 * Reference (lane_protocol.h "Reference"): the baseline as it stood when
 * learning last finished (boot, laneDetectCalibrate()), or the value given to
 * laneDetectSetReference(). Unlike the baseline it never moves with the IIR;
 * 0 = none yet. A relearn keeps the old reference until it has a new one.
 *
 * Raw window (laneRawWindow_t): lowest/highest sample since the last SIGNAL
 * read. Two contexts share it, the caller makes each call atomic against the
 * other (sense manager: irq_guard around every call):
 *   - main loop, once per sample:        laneRawWindowAdd()
 *   - I3C ISR, when the SIGNAL response
 *     is built (also speculatively after
 *     every read):                       laneRawWindowTake()
 *   - I3C ISR, when the read-done hook
 *     says the response was clocked out: laneRawWindowConfirm()
 * Take is not destructive: what it hands out stays "in flight" and is merged
 * into the next Take until Confirm drops it, so a response that is built but
 * never read, or rebuilt, loses nothing. A sample added after a Take lands in
 * the next window; none is lost or split.
 *
 * Time: local µs (TIM2 counts), u32, wrap-safe for intervals below 2^31 µs.
 * The sense manager maps tEntry onto the mainboard axis when it emits.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lane_status.h"

/* Samples averaged into the first baseline (64 ms at 1 kHz). */
#define DETECT_LEARN_SAMPLES     64U
/* Baseline IIR while FREE: new = old + (raw − old) / 2^11 (≈2 s at 1 kHz). */
#define DETECT_BASELINE_SHIFT    11U
/* Noise IIR: 1/64 per sample. */
#define DETECT_NOISE_SHIFT       6U

typedef struct {
    uint16_t thrDropPermille;   /* 1..999                                  */
    uint16_t hystPermille;      /* < thrDropPermille, so HIGH < baseline   */
    uint32_t mergeGapUs;
    uint32_t blockTimeoutUs;    /* > mergeGapUs                            */
    uint32_t minDwellUs;
    uint32_t holdoffUs;         /* 0 = off                                 */
    uint16_t saturationRaw;     /* sensor full scale (65535 at 16 bit)     */
} laneDetectConfig_t;

typedef enum {
    DETECT_STATE_LEARNING = 0,
    DETECT_STATE_FREE,
    DETECT_STATE_IN_CAR,
    DETECT_STATE_GAP,
    DETECT_STATE_BLOCKED,
    DETECT_STATE_BLOCKED_GAP,
} laneDetectState_e;

/* One emitted event, local time. flags uses the LANE_FLAG_* bits of
 * lane_protocol.h (IN_HOLDOFF, MERGED, SATURATED); the sync flags are the
 * sense manager's to add. */
typedef struct {
    uint8_t  type;          /* laneEventType_e                                */
    uint8_t  flags;
    uint16_t depth;         /* baseline − minimum, raw counts                 */
    uint32_t tEntry;        /* local µs                                       */
    uint32_t dwellUs;
} laneDetectEvent_t;

/* Lowest/highest raw sample since the last confirmed SIGNAL read. */
typedef struct {
    bool     curHas;        /* samples added since the last Take               */
    uint16_t curMin;
    uint16_t curMax;
    bool     outHas;        /* handed out by Take, not yet confirmed           */
    uint16_t outMin;
    uint16_t outMax;
} laneRawWindow_t;

typedef struct {
    laneDetectConfig_t cfg;
    laneDetectState_e  state;

    uint32_t baselineQ8;    /* raw counts × 256                               */
    uint32_t noiseQ8;
    uint16_t reference;     /* bare-foil level of the last calibration, 0 = none */
    uint32_t learnSum;
    uint32_t learnCount;
    uint16_t thrLow;
    uint16_t thrHigh;

    bool     hasPrev;
    uint16_t prevRaw;
    uint32_t prevT;

    uint32_t entryT;        /* entry of the current pass / blockage           */
    uint32_t gapT;          /* rising crossing of HIGH that opened the gap    */
    uint16_t minRaw;
    bool     merged;
    bool     saturated;

    bool     hasHoldoff;
    uint32_t holdoffEntryT; /* entry of the last unflagged CROSSING           */

    /* Diagnostics; they only count up. */
    uint32_t crossings;
    uint32_t glitches;
    uint32_t blockages;
} laneDetect_t;

/* Fills the compiled defaults (the CONFIG defaults of lane_regs). */
void laneDetectDefaultConfig(laneDetectConfig_t *cfg);

/*
 * Starts over: forgets the baseline and learns it from the next samples.
 * Returns LANE_ERR_INVALID_ARG for a config laneDetectSetConfig() would
 * reject, and then runs with the defaults.
 */
laneStatus_e laneDetectInit(laneDetect_t *d, const laneDetectConfig_t *cfg);

/*
 * New parameters, baseline kept. A pass in progress finishes under the new
 * thresholds. Returns LANE_ERR_INVALID_ARG (config unchanged) for
 * thrDropPermille above 999, hystPermille >= thrDropPermille (so drop 0 too),
 * blockTimeoutUs <= mergeGapUs, or minDwellUs >= blockTimeoutUs.
 */
laneStatus_e laneDetectSetConfig(laneDetect_t *d, const laneDetectConfig_t *cfg);

/* Relearns the baseline from the next samples; a pass in progress is
 * abandoned without an event. */
void laneDetectCalibrate(laneDetect_t *d);

/*
 * COMMAND SET_REFERENCE. value != 0: baseline = reference = value, no
 * learning, calibrated at once (state FREE); a pass in progress is abandoned
 * without an event and no crossing is interpolated across the jump. value 0:
 * forget, learn again exactly as after init (reference 0 until done).
 */
void laneDetectSetReference(laneDetect_t *d, uint16_t value);

/* The sample stream was interrupted (sampler stopped/restarted): the next
 * sample is not adjacent to the last, so no crossing is interpolated across. */
void laneDetectBreak(laneDetect_t *d);

/*
 * One sample at local time t (strictly after the previous one). Returns true
 * and fills *out when the sample completes an event.
 */
bool laneDetectSample(laneDetect_t *d, uint32_t t, uint16_t raw, laneDetectEvent_t *out);

/* Whether t lies inside the holdoff window (for the slower sample period). */
bool laneDetectInHoldoff(const laneDetect_t *d, uint32_t t);

/* Signal below LOW right now (a car, a blockage, or a pass in progress). */
bool laneDetectIsBlocked(const laneDetect_t *d);

/* Baseline learned. */
bool laneDetectIsCalibrated(const laneDetect_t *d);

uint16_t laneDetectBaseline(const laneDetect_t *d);
uint16_t laneDetectNoise(const laneDetect_t *d);

/* Reference level, 0 until the first learning has finished or one was set. */
uint16_t laneDetectReference(const laneDetect_t *d);

/* Raw window, see above. Init empties it. */
void laneRawWindowInit(laneRawWindow_t *w);
void laneRawWindowAdd(laneRawWindow_t *w, uint16_t raw);
/* Window since the last confirmed read (in-flight part included); *min and
 * *max are `last` (the latest sample) when nothing was added. */
void laneRawWindowTake(laneRawWindow_t *w, uint16_t last, uint16_t *min, uint16_t *max);
/* What the last Take handed out reached the controller. */
void laneRawWindowConfirm(laneRawWindow_t *w);
