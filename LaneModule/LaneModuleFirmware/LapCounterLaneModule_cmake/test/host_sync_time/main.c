/*
 * Host test for sync_time.c: SYNC edges to the mainboard time axis.
 *
 * A simulated mainboard emits a rising SYNC edge every P µs of its own time;
 * the lane's TIM2 runs at `rate` local counts per mainboard µs (HSI error up
 * to ±1 %) and captures each edge. The real sync_time.c must map any local
 * instant back onto the mainboard's axis to within a couple of µs, through
 * drift, counter wrap, glitches and dropouts, and must flag what it cannot
 * vouch for.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "sync_time.h"

static int failures;

static void check(const char *name, int passed)
{
    printf("%-52s %s\n", name, passed ? "ok" : "FAIL");
    failures += passed ? 0 : 1;
}


/* --- Simulated mainboard and lane clock ---------------------------------- */

#define P  SYNC_TIME_DEFAULT_PERIOD_US

static syncTime_t st;
static uint64_t   simMb;          /* mainboard µs */
static double     simLocal;       /* unwrapped local counts since simLocalStart */
static double     simRate;        /* local counts per mainboard µs */
static uint32_t   simLocalStart;  /* TIM2 value at simMb = 0 */

static void simReset(uint32_t localStart, double rate, uint64_t mbStart)
{
    (void)syncTimeInit(&st, P);
    simMb         = mbStart;
    simLocal      = 0.0;
    simRate       = rate;
    simLocalStart = localStart;
}

static uint32_t simLocalNow(void)
{
    return simLocalStart + (uint32_t)(int64_t)(simLocal + 0.5);
}

static void simAdvanceTo(uint64_t mb)
{
    simLocal += (double)(mb - simMb) * simRate;
    simMb     = mb;
}

/* Runs n SYNC periods; each edge is captured unless feed is false. */
static void simEdges(uint32_t n, bool feed)
{
    for (uint32_t i = 0U; i < n; i++) {
        simAdvanceTo((simMb / P + 1U) * P);
        if (feed) {
            (void)syncTimeOnEdge(&st, simLocalNow());
        }
    }
}

/* Aligns the way the mainboard would: tick and phase at simMb, received
 * latencyUs later. */
static laneStatus_e simAlign(uint32_t latencyUs)
{
    uint32_t tick  = (uint32_t)(simMb / P);
    uint32_t phase = (uint32_t)(simMb % P);
    simAdvanceTo(simMb + latencyUs);
    return syncTimeAlign(&st, tick, phase, simLocalNow());
}

/* Maps the instant mb (>= simMb) and returns |error| in µs; flags out. */
static uint64_t simMapError(uint64_t mb, uint8_t *flags)
{
    simAdvanceTo(mb);
    uint64_t got = 0U;
    *flags = syncTimeToMainboard(&st, simLocalNow(), simLocalNow(), &got);
    return (got > mb) ? got - mb : mb - got;
}

/* Locked and aligned at 1 % fast, mainboard tick 5000. */
static void simLockedAligned(uint32_t localStart, double rate)
{
    simReset(localStart, rate, 5000U * (uint64_t)P);
    simEdges(20U, true);
    simAdvanceTo(simMb + 300U);
    (void)simAlign(20U);
}


/* --- Cases --------------------------------------------------------------- */

static void testInit(void)
{
    syncTime_t s;
    check("init: 100 us accepted",            syncTimeInit(&s, 100U) == LANE_OK);
    check("init: 100000 us accepted",         syncTimeInit(&s, 100000U) == LANE_OK);
    check("init: 99 us rejected",             syncTimeInit(&s, 99U) == LANE_ERR_INVALID_ARG);
    check("init: rejected period falls back to default", s.nominalUs == SYNC_TIME_DEFAULT_PERIOD_US);
    check("init: 100001 us rejected",         syncTimeInit(&s, 100001U) == LANE_ERR_INVALID_ARG);
}

static void testNoLock(void)
{
    uint8_t  flags;
    uint64_t got = 0U;

    simReset(1234U, 1.0, 0U);
    flags = syncTimeToMainboard(&st, 777U, 777U, &got);
    check("no edge: raw local time, NO_LOCK|UNALIGNED",
          got == 777U && flags == (SYNC_TIME_FLAG_NO_LOCK | SYNC_TIME_FLAG_UNALIGNED));

    simEdges(1U, true);
    flags = syncTimeToMainboard(&st, 777U, 777U, &got);
    check("one edge: still NO_LOCK", (flags & SYNC_TIME_FLAG_NO_LOCK) != 0U);

    simEdges(1U, true);
    check("two edges: locked", syncTimeIsLocked(&st, simLocalNow()));
    flags = syncTimeToMainboard(&st, simLocalNow(), simLocalNow(), &got);
    check("two edges: only UNALIGNED left", flags == SYNC_TIME_FLAG_UNALIGNED);

    uint32_t local = 0U;
    check("toLocal before alignment: UNALIGNED",
          syncTimeToLocal(&st, 0U, &local) == LANE_ERR_SYNC_UNALIGNED);

    simReset(0U, 1.0, 0U);
    simEdges(1U, true);
    check("toLocal before a period: NO_LOCK",
          syncTimeToLocal(&st, 0U, &local) == LANE_ERR_SYNC_NO_LOCK);
    check("align before a period: NO_LOCK",
          syncTimeAlign(&st, 0U, 0U, simLocalNow()) == LANE_ERR_SYNC_NO_LOCK);
}

static void testMappingUnderDrift(void)
{
    const double rates[] = { 1.01, 0.99, 1.0, 1.003 };
    uint8_t flags;

    for (unsigned i = 0U; i < sizeof rates / sizeof rates[0]; i++) {
        simLockedAligned(0x00100000U, rates[i]);

        uint64_t worst = 0U;
        int      clean = 1;
        for (uint32_t k = 0U; k < 50U; k++) {
            simEdges(1U, true);
            for (uint32_t at = 1U; at < P; at += 97U) {
                uint64_t e = simMapError(simMb - simMb % P + at, &flags);
                worst = (e > worst) ? e : worst;
                clean = clean && flags == 0U;
            }
        }
        char name[64];
        snprintf(name, sizeof name, "drift %.3f: max error <= 2 us, no flags", rates[i]);
        check(name, worst <= 2U && clean);
    }
}

static void testAlignAcrossEdge(void)
{
    uint8_t flags;

    /* Built 10 µs before edge K+1, received 5 µs after the lane captured it. */
    simReset(500U, 1.01, 7000U * (uint64_t)P);
    simEdges(10U, true);
    simAdvanceTo(simMb + 990U);
    uint32_t tick  = (uint32_t)(simMb / P);
    uint32_t phase = (uint32_t)(simMb % P);
    simEdges(1U, true);
    simAdvanceTo(simMb + 5U);
    check("align: message straddles an edge (built before)",
          syncTimeAlign(&st, tick, phase, simLocalNow()) == LANE_OK
          && simMapError(simMb + 100U, &flags) <= 2U && flags == 0U);

    /* Mainboard already past edge K+1, lane has not captured it yet. */
    simReset(500U, 0.99, 7000U * (uint64_t)P);
    simEdges(10U, true);
    simEdges(1U, false);
    tick  = (uint32_t)(simMb / P);
    simAdvanceTo(simMb + 2U);
    phase = 2U;
    simAdvanceTo(simMb + 8U);
    laneStatus_e s = syncTimeAlign(&st, tick, phase, simLocalNow());
    /* Feed the late edge as the ISR would, then check. */
    (void)syncTimeOnEdge(&st, simLocalStart + (uint32_t)(int64_t)(simLocal - 10.0 * 0.99 + 0.5));
    check("align: message straddles an edge (lane behind)",
          s == LANE_OK && simMapError(simMb + 300U, &flags) <= 2U && flags == 0U);

    check("align: phase of P rejected",
          syncTimeAlign(&st, 1U, P, simLocalNow()) == LANE_ERR_INVALID_ARG);
}

static void testRealign(void)
{
    simLockedAligned(0U, 1.01);
    simEdges(3U, true);
    simAdvanceTo(simMb + 400U);
    check("realign: consistent data shifts nothing",
          simAlign(30U) == LANE_OK && st.realignShifts == 0U);

    simAdvanceTo(simMb + 100U);
    uint32_t tick = (uint32_t)(simMb / P) + 1U;
    check("realign: a tick off is counted",
          syncTimeAlign(&st, tick, (uint32_t)(simMb % P), simLocalNow()) == LANE_OK
          && st.realignShifts == 1U);
}

static void testWrap(void)
{
    uint8_t flags;

    /* TIM2 wraps a few periods after alignment. */
    simLockedAligned(0xFFFFFFFFU - 25000U, 1.01);
    uint64_t worst = 0U;
    for (uint32_t k = 0U; k < 20U; k++) {
        simEdges(1U, true);
        uint64_t e = simMapError(simMb + 501U, &flags);
        worst = (e > worst) ? e : worst;
    }
    check("TIM2 wrap: mapping continuous", worst <= 2U && flags == 0U);
}

static void testDriftChange(void)
{
    uint8_t flags;

    simLockedAligned(0U, 1.01);
    simRate = 0.995;
    simEdges(60U, true);
    check("HSI drift step 1.01 -> 0.995: tracked within 60 edges",
          simMapError(simMb + 900U, &flags) <= 2U && flags == 0U);
}

static void testGlitch(void)
{
    uint8_t flags;

    simLockedAligned(0U, 1.01);
    simAdvanceTo(simMb + 480U);
    bool accepted = syncTimeOnEdge(&st, simLocalNow());
    simEdges(1U, true);
    check("glitch mid-period: rejected", !accepted && st.edgesRejected == 1U);
    check("glitch mid-period: tick and mapping intact",
          simMapError(simMb + 700U, &flags) <= 2U && flags == 0U);

    simLockedAligned(0U, 1.01);
    simAdvanceTo(simMb + P - (P / SYNC_TIME_TOLERANCE_DIV) - 20U);
    check("edge just outside tolerance: rejected", !syncTimeOnEdge(&st, simLocalNow()));
}

static void testMissedEdges(void)
{
    uint8_t flags;

    simLockedAligned(0U, 0.99);
    simEdges(3U, false);
    simEdges(1U, true);
    check("3 missed edges: counted", st.edgesMissed == 3U);
    check("3 missed edges: tick and mapping intact",
          simMapError(simMb + 250U, &flags) <= 2U && flags == 0U);

    simEdges(SYNC_TIME_MAX_GAP_PERIODS + 1U, false);
    simEdges(1U, true);
    (void)simMapError(simMb + 250U, &flags);
    check("gap beyond MAX_GAP: alignment dropped", flags == SYNC_TIME_FLAG_UNALIGNED);
    check("gap beyond MAX_GAP: clock kept", syncTimeIsLocked(&st, simLocalNow()));
}

static void testExtrapolation(void)
{
    uint8_t flags;

    simLockedAligned(0U, 1.01);
    simEdges(1U, true);
    check("SYNC present: locked", syncTimeIsLocked(&st, simLocalNow()));
    simEdges(SYNC_TIME_LOSS_PERIODS + 1U, false);
    uint64_t e = simMapError(simMb + 10U, &flags);
    check("SYNC lost: EXTRAPOLATED, still accurate",
          flags == SYNC_TIME_FLAG_EXTRAPOLATED && e <= 2U);
    check("SYNC lost: not locked", !syncTimeIsLocked(&st, simLocalNow()));
    check("SYNC lost: align refused",
          syncTimeAlign(&st, 0U, 0U, simLocalNow()) == LANE_ERR_SYNC_NO_LOCK);
}

static void testBadAnchorRecovery(void)
{
    /* First thing captured is a spike 400 µs before a real edge. */
    simReset(0U, 1.0, 100U * (uint64_t)P);
    simAdvanceTo(simMb + 600U);
    (void)syncTimeOnEdge(&st, simLocalNow());

    uint32_t n = 0U;
    while (!syncTimeIsLocked(&st, simLocalNow()) && n < 20U) {
        simEdges(1U, true);
        n++;
    }
    check("glitch as first anchor: locks within REJECTS + 2 edges",
          n <= SYNC_TIME_REANCHOR_REJECTS + 2U && st.reanchors >= 1U);
}

static void testPeriodChanged(void)
{
    /* Mainboard switched to 1300 µs without telling the lane: never locks
     * onto the wrong grid, keeps re-anchoring. */
    simLockedAligned(0U, 1.0);
    uint32_t before = st.reanchors;
    for (uint32_t k = 0U; k < 20U; k++) {
        simAdvanceTo(simMb + 1300U);
        (void)syncTimeOnEdge(&st, simLocalNow());
    }
    uint8_t  flags;
    uint64_t got;
    flags = syncTimeToMainboard(&st, simLocalNow(), simLocalNow(), &got);
    check("unannounced period change: re-anchors, flags it",
          st.reanchors > before && (flags & (SYNC_TIME_FLAG_NO_LOCK | SYNC_TIME_FLAG_UNALIGNED)) != 0U);
}

static void testNegativeDelta(void)
{
    simLockedAligned(0U, 1.01);
    simEdges(1U, true);
    uint64_t edgeMb    = simMb;
    uint32_t edgeLocal = simLocalNow();
    uint32_t before    = edgeLocal - (uint32_t)(int64_t)(100.0 * 1.01 + 0.5);
    uint64_t got       = 0U;
    uint8_t  flags     = syncTimeToMainboard(&st, before, edgeLocal, &got);
    uint64_t want      = edgeMb - 100U;
    uint64_t e         = (got > want) ? got - want : want - got;
    check("instant just before the anchor: maps backwards", e <= 1U && flags == 0U);
}

static void testToLocal(void)
{
    uint8_t flags;

    simLockedAligned(0x7FFFFF00U, 0.99);
    simEdges(5U, true);
    uint64_t target = simMb + 12345U;
    uint32_t local  = 0U;
    laneStatus_e s = syncTimeToLocal(&st, target, &local);
    uint64_t back = 0U;
    flags = syncTimeToMainboard(&st, local, simLocalNow(), &back);
    uint64_t e = (back > target) ? back - target : target - back;
    check("toLocal/toMainboard round trip", s == LANE_OK && e <= 1U && flags == 0U);

    simAdvanceTo(target);
    int64_t lerr = (int64_t)(int32_t)(simLocalNow() - local);
    check("toLocal: predicts the counter at the target", lerr >= -2 && lerr <= 2);

    check("toLocal: target beyond 2^31 counts rejected",
          syncTimeToLocal(&st, simMb + 3000000000ULL, &local) == LANE_ERR_INVALID_ARG);
}

int main(void)
{
    testInit();
    testNoLock();
    testMappingUnderDrift();
    testAlignAcrossEdge();
    testRealign();
    testWrap();
    testDriftChange();
    testGlitch();
    testMissedEdges();
    testExtrapolation();
    testBadAnchorRecovery();
    testPeriodChanged();
    testNegativeDelta();
    testToLocal();

    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
