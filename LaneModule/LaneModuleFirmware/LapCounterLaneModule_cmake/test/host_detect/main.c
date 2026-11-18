/*
 * Host test for lane_detect.c: synthetic proximity traces in, events out.
 *
 * A simulated lane samples a piecewise-linear signal every P µs of local
 * time: bare foil at BASE counts, a car pulls it down to CAR counts with
 * linear edges. With the defaults (drop 30 %, hysteresis 10 %) LOW is 2800
 * and HIGH 3200 for BASE 4000, and on a linear edge the interpolated
 * crossing is exact, so entry and exit times can be checked to a µs.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "lane_detect.h"
#include "lane_protocol.h"

static int failures;

static void check(const char *name, int passed)
{
    printf("%-58s %s\n", name, passed ? "ok" : "FAIL");
    failures += passed ? 0 : 1;
}


/* --- Simulated signal ----------------------------------------------------- */

#define P      1000U        /* sample period, µs */
#define BASE   4000U
#define CAR    800U
#define EDGE   2000U        /* µs for a full BASE <-> CAR swing */
#define MAXCAR 8U

typedef struct {
    uint32_t in;            /* start of the falling edge, relative µs */
    uint32_t out;           /* start of the rising edge */
    uint16_t low;           /* level under the car */
} simCar_t;

static simCar_t  simCars[MAXCAR];
static uint32_t  simCarCount;
static uint32_t  simBase;
static uint32_t  simT0;         /* local time of relative 0 */
static uint32_t  simRel;        /* relative time of the next sample */
static int32_t   simDriftPerS;  /* baseline change in counts per second */

static laneDetect_t      det;
static laneDetectEvent_t evs[16];
static uint32_t          evCount;

static uint16_t simLevel(uint32_t rel)
{
    int64_t base = (int64_t)simBase + (int64_t)simDriftPerS * (int64_t)rel / 1000000;
    int64_t v    = base;

    for (uint32_t i = 0U; i < simCarCount; i++) {
        const simCar_t *c = &simCars[i];
        int64_t swing = base - c->low;
        int64_t w     = base;
        if (rel >= c->in && rel < c->in + EDGE) {
            w = base - swing * (rel - c->in) / EDGE;
        } else if (rel >= c->in + EDGE && rel < c->out) {
            w = c->low;
        } else if (rel >= c->out && rel < c->out + EDGE) {
            w = c->low + swing * (rel - c->out) / EDGE;
        }
        if (w < v) {
            v = w;
        }
    }
    if (v < 0) {
        v = 0;
    }
    if (v > 65535) {
        v = 65535;
    }
    return (uint16_t)v;
}

static void simReset(uint32_t t0, uint32_t base, const laneDetectConfig_t *cfg)
{
    laneDetectConfig_t def;
    laneDetectDefaultConfig(&def);
    (void)laneDetectInit(&det, (cfg != NULL) ? cfg : &def);

    simCarCount  = 0U;
    simBase      = base;
    simT0        = t0;
    simRel       = 0U;
    simDriftPerS = 0;
    evCount      = 0U;
}

static void simAddCar(uint32_t in, uint32_t out, uint16_t low)
{
    simCars[simCarCount].in  = in;
    simCars[simCarCount].out = out;
    simCars[simCarCount].low = low;
    simCarCount++;
}

/* Samples up to (excluding) relative time `until`, every `period` µs. */
static void simRunPeriod(uint32_t until, uint32_t period)
{
    while (simRel < until) {
        laneDetectEvent_t e;
        if (laneDetectSample(&det, simT0 + simRel, simLevel(simRel), &e) && evCount < 16U) {
            evs[evCount++] = e;
        }
        simRel += period;
    }
}

static void simRun(uint32_t until)
{
    simRunPeriod(until, P);
}

/* Truth: the falling edge of a car starting at `in` crosses `level`. */
static uint32_t truthFall(uint32_t in, uint16_t level, uint16_t low)
{
    return in + EDGE * (simBase - level) / (simBase - low);
}

static uint32_t truthRise(uint32_t out, uint16_t level, uint16_t low)
{
    return out + EDGE * (uint32_t)(level - low) / (simBase - low);
}

static bool near(uint32_t got, uint32_t want, uint32_t tol)
{
    int32_t d = (int32_t)(got - want);
    return d >= -(int32_t)tol && d <= (int32_t)tol;
}

/* Learn the baseline, then let it settle. */
static void simWarmUp(void)
{
    simRun(100000U);
}


/* --- Cases ---------------------------------------------------------------- */

static void testLearning(void)
{
    simReset(0U, BASE, NULL);
    simRun((DETECT_LEARN_SAMPLES - 1U) * P);
    check("learning: not calibrated before 64 samples", !laneDetectIsCalibrated(&det));
    simRun(DETECT_LEARN_SAMPLES * P);
    check("learning: calibrated after 64 samples", laneDetectIsCalibrated(&det));
    check("learning: baseline is the mean", laneDetectBaseline(&det) == BASE);

    /* A car during learning is not an event (the lane must be free). */
    simReset(0U, BASE, NULL);
    simAddCar(10000U, 30000U, CAR);
    simRun(64000U);
    check("learning: no event while learning", evCount == 0U);
}

static void testCleanPass(void)
{
    simReset(1000U, BASE, NULL);
    simWarmUp();
    simAddCar(200250U, 220250U, CAR);

    uint32_t entry = truthFall(200250U, 2800U, CAR);
    uint32_t exitT = truthRise(220250U, 3200U, CAR);

    simRun(exitT + 20000U);
    check("clean pass: nothing before the merge gap ran out", evCount == 0U);
    simRun(exitT + 40000U);

    check("clean pass: exactly one event", evCount == 1U);
    check("clean pass: type CROSSING", evCount >= 1U && evs[0].type == LANE_EVT_CROSSING);
    check("clean pass: tEntry interpolated (+-2 us)",
          evCount >= 1U && near(evs[0].tEntry, 1000U + entry, 2U));
    check("clean pass: dwell = exit - entry (+-4 us)",
          evCount >= 1U && near(evs[0].dwellUs, exitT - entry, 4U));
    /* The falling-edge samples above LOW still feed the baseline IIR, which
     * pulls it down by a fraction of a count. */
    check("clean pass: depth = baseline - minimum (+-2)",
          evCount >= 1U && evs[0].depth >= BASE - CAR - 2U && evs[0].depth <= BASE - CAR);
    check("clean pass: no flags", evCount >= 1U && evs[0].flags == 0U);
    check("clean pass: free again afterwards", !laneDetectIsBlocked(&det));
}

static void testBlockedFlagDuringPass(void)
{
    simReset(0U, BASE, NULL);
    simWarmUp();
    simAddCar(200000U, 220000U, CAR);
    simRun(210000U);
    check("pass: IsBlocked while the signal is below LOW", laneDetectIsBlocked(&det));
}

static void testMergedPass(void)
{
    simReset(0U, BASE, NULL);
    simWarmUp();
    /* Nose and body: up again for ~10 ms, inside the 30 ms merge gap. */
    simAddCar(200000U, 208000U, CAR);
    simAddCar(220000U, 230000U, CAR);
    simRun(300000U);

    uint32_t entry = truthFall(200000U, 2800U, CAR);
    uint32_t exitT = truthRise(230000U, 3200U, CAR);

    check("merged pass: one event", evCount == 1U);
    check("merged pass: MERGED flag",
          evCount >= 1U && (evs[0].flags & LANE_FLAG_MERGED) != 0U);
    check("merged pass: entry of the first dip (+-2 us)",
          evCount >= 1U && near(evs[0].tEntry, entry, 2U));
    check("merged pass: dwell to the last exit (+-4 us)",
          evCount >= 1U && near(evs[0].dwellUs, exitT - entry, 4U));
}

static void testTwoCars(void)
{
    simReset(0U, BASE, NULL);
    simWarmUp();
    /* Second car 50 ms after the first left: beyond the merge gap. */
    simAddCar(200000U, 220000U, CAR);
    simAddCar(272000U, 290000U, CAR);
    simRun(400000U);

    check("two cars: two events", evCount == 2U);
    check("two cars: neither MERGED",
          evCount == 2U && (evs[0].flags & LANE_FLAG_MERGED) == 0U
          && (evs[1].flags & LANE_FLAG_MERGED) == 0U);
    check("two cars: second entry interpolated",
          evCount == 2U && near(evs[1].tEntry, truthFall(272000U, 2800U, CAR), 2U));
}

static void testBackToBack(void)
{
    /* The next car enters in the very sample that ends the previous gap. */
    simReset(0U, BASE, NULL);
    simWarmUp();
    simAddCar(200000U, 220000U, CAR);
    simAddCar(253500U, 270000U, CAR);
    simRun(400000U);
    check("back to back: two CROSSINGs", evCount == 2U && evs[1].type == LANE_EVT_CROSSING);
}

static void testGlitch(void)
{
    laneDetectConfig_t cfg;
    laneDetectDefaultConfig(&cfg);          /* minDwell 2 ms */

    simReset(0U, BASE, &cfg);
    simWarmUp();
    /* A 1 ms dip: entry ~200 ms + 0.75 ms, exit ~1 ms later. */
    simCars[0].in  = 200000U;
    simCars[0].out = 200000U;
    simCars[0].low = CAR;
    simCarCount    = 1U;
    simRun(300000U);
    check("glitch: shorter than minDwell is no event", evCount == 0U);
    check("glitch: counted", det.glitches == 1U);

    /* The same dip with minDwell 0.5 ms is a (very short) car. */
    cfg.minDwellUs = 500U;
    simReset(0U, BASE, &cfg);
    simWarmUp();
    simCars[0].in  = 200000U;
    simCars[0].out = 200000U;
    simCars[0].low = CAR;
    simCarCount    = 1U;
    simRun(300000U);
    check("glitch: longer than minDwell is a CROSSING", evCount == 1U);
}

static void testHandBlock(void)
{
    simReset(0U, BASE, NULL);
    simWarmUp();
    simAddCar(200000U, 2200000U, 300U);    /* a hand for 2 s */

    uint32_t entry = truthFall(200000U, 2800U, 300U);

    simRun(entry + 499000U);
    check("hand: nothing before the block timeout", evCount == 0U);
    simRun(entry + 502000U);
    check("hand: BLOCKED at the block timeout",
          evCount == 1U && evs[0].type == LANE_EVT_BLOCKED);
    check("hand: BLOCKED carries the entry",
          evCount >= 1U && near(evs[0].tEntry, entry, 2U));
    check("hand: BLOCKED dwell ~ block timeout",
          evCount >= 1U && evs[0].dwellUs >= 500000U && evs[0].dwellUs < 502000U);
    check("hand: IsBlocked", laneDetectIsBlocked(&det));

    simRun(2100000U);
    check("hand: only one BLOCKED while it stays", evCount == 1U);

    uint32_t lift = truthRise(2200000U, 3200U, 300U);
    simRun(lift + 40000U);
    check("hand: CLEARED once it lifts", evCount == 2U && evs[1].type == LANE_EVT_CLEARED);
    check("hand: CLEARED carries the same entry",
          evCount == 2U && evs[1].tEntry == evs[0].tEntry);
    check("hand: CLEARED dwell = whole blockage (+-4 us)",
          evCount == 2U && near(evs[1].dwellUs, lift - entry, 4U));
    check("hand: free afterwards", !laneDetectIsBlocked(&det));

    /* A car right after it is detected normally. */
    simAddCar(2400000U, 2420000U, CAR);
    simRun(2500000U);
    check("hand: next car is a CROSSING", evCount == 3U && evs[2].type == LANE_EVT_CROSSING);
}

static void testHoldoff(void)
{
    laneDetectConfig_t cfg;
    laneDetectDefaultConfig(&cfg);
    cfg.holdoffUs = 5000000U;

    simReset(0U, BASE, &cfg);
    simWarmUp();
    simAddCar(1000000U, 1020000U, CAR);   /* lap candidate */
    simAddCar(3000000U, 3020000U, CAR);   /* 2 s later: inside */
    simAddCar(5500000U, 5520000U, CAR);   /* 4.5 s after the first: inside */
    simAddCar(6500000U, 6520000U, CAR);   /* 5.5 s after the first: outside */

    simRun(2000000U);
    check("holdoff: in holdoff after a crossing", laneDetectInHoldoff(&det, 2000000U));
    simRun(7000000U);

    check("holdoff: nothing dropped (4 events)", evCount == 4U);
    check("holdoff: first not flagged",
          evCount == 4U && (evs[0].flags & LANE_FLAG_IN_HOLDOFF) == 0U);
    check("holdoff: second flagged IN_HOLDOFF",
          evCount == 4U && (evs[1].flags & LANE_FLAG_IN_HOLDOFF) != 0U);
    check("holdoff: a flagged event does not extend the window (3rd flagged)",
          evCount == 4U && (evs[2].flags & LANE_FLAG_IN_HOLDOFF) != 0U);
    check("holdoff: fourth outside the window",
          evCount == 4U && (evs[3].flags & LANE_FLAG_IN_HOLDOFF) == 0U);
    check("holdoff: window restarts at the fourth",
          laneDetectInHoldoff(&det, 7000000U));

    /* Holdoff 0 = off: never flagged. */
    simReset(0U, BASE, NULL);
    simWarmUp();
    simAddCar(1000000U, 1020000U, CAR);
    simAddCar(1100000U, 1120000U, CAR);
    simRun(1300000U);
    check("holdoff off: never flagged",
          evCount == 2U && (evs[1].flags & LANE_FLAG_IN_HOLDOFF) == 0U);
    check("holdoff off: InHoldoff false", !laneDetectInHoldoff(&det, 1150000U));
}

static void testSparseSampling(void)
{
    /* Holdoff watch rate: 100 Hz still sees a 25 ms car. */
    simReset(0U, BASE, NULL);
    simRunPeriod(1000000U, 10000U);
    simAddCar(1500000U, 1525000U, CAR);
    simRunPeriod(1700000U, 10000U);
    check("100 Hz: a 25 ms car is still one CROSSING",
          evCount == 1U && evs[0].type == LANE_EVT_CROSSING);
}

static void testSaturation(void)
{
    /* Foil so bright the sensor clips: the sample before entry is at full
     * scale, so the interpolated entry is too early by an unknown amount. */
    simReset(0U, 65535U, NULL);
    simWarmUp();
    simAddCar(200000U, 220000U, 10000U);
    simRun(300000U);
    check("saturated foil: event still emitted", evCount == 1U);
    check("saturated foil: SATURATED flag",
          evCount == 1U && (evs[0].flags & LANE_FLAG_SATURATED) != 0U);

    /* Only the entry is clipped: a hand on clipped foil, BLOCKED before any
     * exit sample exists. */
    simReset(0U, 65535U, NULL);
    simWarmUp();
    simAddCar(200000U, 2000000U, 10000U);
    simRun(800000U);
    check("saturated foil: clipped entry alone flags (BLOCKED)",
          evCount == 1U && evs[0].type == LANE_EVT_BLOCKED && (evs[0].flags & LANE_FLAG_SATURATED) != 0U);

    /* Unclipped foil: no flag (the clean pass checks flags == 0 too). */
    simReset(0U, 60000U, NULL);
    simWarmUp();
    simAddCar(200000U, 2000000U, 10000U);
    simRun(800000U);
    check("unclipped foil: BLOCKED not SATURATED",
          evCount == 1U && (evs[0].flags & LANE_FLAG_SATURATED) == 0U);
}

static void testBaselineDrift(void)
{
    /* Light fades by 40 % over 120 s: without tracking, the foil itself
     * would fall below LOW and read as a blockage. */
    simReset(0U, BASE, NULL);
    simWarmUp();
    simDriftPerS = -(int32_t)(BASE * 40U / 100U) / 120;
    simRun(120000000U);
    check("drift: no event while the baseline fades", evCount == 0U);
    uint16_t b = laneDetectBaseline(&det);
    uint16_t want = simLevel(simRel);
    check("drift: baseline tracked (within 2 %)", b > want - want / 50U && b < want + want / 50U);
    check("drift: thresholds follow the baseline", det.thrLow < 2000U && det.thrLow > 1500U);

    /* A car on the faded foil is still a car. */
    simDriftPerS = 0;
    simBase      = want;
    simAddCar(120200000U, 120220000U, 500U);
    simRun(120400000U);
    check("drift: car on faded foil detected",
          evCount == 1U && evs[0].type == LANE_EVT_CROSSING);
}

static void testNoBaselineUpdateUnderCar(void)
{
    /* A long (but not blocking) pass must not drag the baseline down. */
    simReset(0U, BASE, NULL);
    simWarmUp();
    simAddCar(200000U, 600000U, CAR);   /* 400 ms */
    simRun(800000U);
    check("pass: baseline untouched by the car", laneDetectBaseline(&det) == BASE);
}

static void testCalibrate(void)
{
    simReset(0U, BASE, NULL);
    simWarmUp();
    simAddCar(200000U, 220000U, CAR);
    simRun(205000U);
    laneDetectCalibrate(&det);
    check("calibrate: learning again", !laneDetectIsCalibrated(&det));
    simCarCount = 0U;
    simBase     = 6000U;
    simRun(400000U);
    check("calibrate: pass in progress abandoned (no event)", evCount == 0U);
    check("calibrate: new baseline learned", laneDetectBaseline(&det) == 6000U);
}

static void testWrap(void)
{
    /* The TIM2 counter wraps in the middle of the pass. */
    simReset(0xFFFFFFFFU - 210000U, BASE, NULL);
    simWarmUp();
    simAddCar(200000U, 230000U, CAR);
    simRun(400000U);
    uint32_t entry = (0xFFFFFFFFU - 210000U) + truthFall(200000U, 2800U, CAR);
    check("wrap: event across the TIM2 wrap", evCount == 1U);
    check("wrap: tEntry right (+-2 us)", evCount == 1U && near(evs[0].tEntry, entry, 2U));
    check("wrap: dwell right",
          evCount == 1U && near(evs[0].dwellUs,
                                truthRise(230000U, 3200U, CAR) - truthFall(200000U, 2800U, CAR), 4U));
}

static void testConfigRejects(void)
{
    laneDetectConfig_t good;
    laneDetectConfig_t c;
    laneDetectDefaultConfig(&good);
    (void)laneDetectInit(&det, &good);

    check("config: defaults accepted", laneDetectSetConfig(&det, &good) == LANE_OK);

    c = good; c.thrDropPermille = 0U;
    check("config: drop 0 rejected", laneDetectSetConfig(&det, &c) == LANE_ERR_INVALID_ARG);
    c = good; c.thrDropPermille = 1000U;
    check("config: drop 1000 rejected", laneDetectSetConfig(&det, &c) == LANE_ERR_INVALID_ARG);
    c = good; c.hystPermille = c.thrDropPermille;
    check("config: hysteresis >= drop rejected", laneDetectSetConfig(&det, &c) == LANE_ERR_INVALID_ARG);
    c = good; c.blockTimeoutUs = c.mergeGapUs;
    check("config: block timeout <= merge gap rejected", laneDetectSetConfig(&det, &c) == LANE_ERR_INVALID_ARG);
    c = good; c.minDwellUs = c.blockTimeoutUs;
    check("config: min dwell >= block timeout rejected", laneDetectSetConfig(&det, &c) == LANE_ERR_INVALID_ARG);
    c = good; c.saturationRaw = 0U;
    check("config: saturation 0 rejected", laneDetectSetConfig(&det, &c) == LANE_ERR_INVALID_ARG);
    check("config: rejected config left unchanged", det.cfg.thrDropPermille == good.thrDropPermille);

    c = good; c.thrDropPermille = 0U;
    check("config: init with a bad config falls back to defaults",
          laneDetectInit(&det, &c) == LANE_ERR_INVALID_ARG && det.cfg.thrDropPermille == 300U);
}

int main(void)
{
    testLearning();
    testCleanPass();
    testBlockedFlagDuringPass();
    testMergedPass();
    testTwoCars();
    testBackToBack();
    testGlitch();
    testHandBlock();
    testHoldoff();
    testSparseSampling();
    testSaturation();
    testBaselineDrift();
    testNoBaselineUpdateUnderCar();
    testCalibrate();
    testWrap();
    testConfigRejects();

    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
