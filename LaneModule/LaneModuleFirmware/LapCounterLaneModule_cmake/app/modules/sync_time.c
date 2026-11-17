/*
 * SYNC mapping. See sync_time.h for the scheme and its limits.
 */

#include "sync_time.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Period estimate filter: new = old + (measured - old) / 8. Smooths the ±1
 * count capture quantisation; at 1 kHz it still follows HSI drift within ms. */
#define SYNC_TIME_PERIOD_FILTER_SHIFT  3

static int64_t syncTimeRoundDiv(int64_t num, int64_t den)
{
    /* den > 0 everywhere this is used. */
    return (num >= 0) ? (num + den / 2) / den : -((-num + den / 2) / den);
}

/* Expected local counts per period: measured once known, else nominal (the
 * local timer runs at a nominal 1 µs). */
static uint32_t syncTimeExpectedCounts(const syncTime_t *st)
{
    return st->hasPeriod ? ((st->periodQ8 + 128U) >> 8) : st->nominalUs;
}

static void syncTimeReanchor(syncTime_t *st, uint32_t captureLocal, uint32_t periods,
                             bool keepPeriod)
{
    st->anchorLocal  = captureLocal;
    st->anchorIndex += periods;
    st->rejectRun    = 0U;
    st->hasPeriod    = st->hasPeriod && keepPeriod;
    st->aligned      = false;
    st->reanchors++;
}

laneStatus_e syncTimeInit(syncTime_t *st, uint32_t nominalUs)
{
    laneStatus_e status = LANE_OK;

    if (nominalUs < SYNC_TIME_PERIOD_MIN_US || nominalUs > SYNC_TIME_PERIOD_MAX_US) {
        nominalUs = SYNC_TIME_DEFAULT_PERIOD_US;
        status    = LANE_ERR_INVALID_ARG;
    }

    memset(st, 0, sizeof(*st));
    st->nominalUs = nominalUs;
    return status;
}

bool syncTimeOnEdge(syncTime_t *st, uint32_t captureLocal)
{
    if (!st->hasAnchor) {
        st->anchorLocal = captureLocal;
        st->hasAnchor   = true;
        st->edgesAccepted++;
        return true;
    }

    uint32_t interval = captureLocal - st->anchorLocal;
    uint32_t expected = syncTimeExpectedCounts(st);
    uint32_t periods  = (interval + expected / 2U) / expected;
    int64_t  residual = (int64_t)interval - (int64_t)periods * (int64_t)expected;
    int64_t  tol      = (int64_t)(expected / SYNC_TIME_TOLERANCE_DIV);

    bool onGrid = periods >= 1U && residual <= tol && residual >= -tol;

    if (periods > SYNC_TIME_MAX_GAP_PERIODS) {
        /* Too long to count reliably, and long enough that the grid test means
         * little: keep the clock, lose the tick. */
        syncTimeReanchor(st, captureLocal, periods, true);
        st->edgesAccepted++;
        return true;
    }

    if (onGrid && !st->hasPeriod && periods != 1U) {
        /* Rounding against the nominal period is only safe for one period:
         * the HSI error is not known yet. Start over from this edge. */
        syncTimeReanchor(st, captureLocal, periods, false);
        st->edgesAccepted++;
        return true;
    }

    if (!onGrid) {
        st->edgesRejected++;
        st->rejectRun++;
        if (st->rejectRun >= SYNC_TIME_REANCHOR_REJECTS) {
            /* The anchor is the odd one out, or P changed under us. Drop the
             * period too, so the next interval is judged against nominal. */
            syncTimeReanchor(st, captureLocal, 1U, false);
        }
        return false;
    }

    if (periods == 1U) {
        int32_t measuredQ8 = (int32_t)(interval << 8);
        if (!st->hasPeriod) {
            st->periodQ8  = (uint32_t)measuredQ8;
            st->hasPeriod = true;
        } else {
            int32_t err = measuredQ8 - (int32_t)st->periodQ8;
            st->periodQ8 = (uint32_t)((int32_t)st->periodQ8 + err / (1 << SYNC_TIME_PERIOD_FILTER_SHIFT));
        }
    } else {
        st->edgesMissed += periods - 1U;
    }

    st->anchorLocal  = captureLocal;
    st->anchorIndex += periods;
    st->rejectRun    = 0U;
    st->edgesAccepted++;
    return true;
}

bool syncTimeIsLocked(const syncTime_t *st, uint32_t nowLocal)
{
    if (!st->hasPeriod) {
        return false;
    }

    uint64_t since = (uint32_t)(nowLocal - st->anchorLocal);
    return since <= (uint64_t)SYNC_TIME_LOSS_PERIODS * syncTimeExpectedCounts(st);
}

/* Local counts since the anchor, in mainboard µs. */
static int64_t syncTimeLocalToUs(const syncTime_t *st, int32_t deltaLocal)
{
    return syncTimeRoundDiv((int64_t)deltaLocal * (int64_t)st->nominalUs * 256,
                            (int64_t)st->periodQ8);
}

laneStatus_e syncTimeAlign(syncTime_t *st, uint32_t mbTick, uint32_t mbPhaseUs,
                           uint32_t localRx)
{
    if (mbPhaseUs >= st->nominalUs) {
        return LANE_ERR_INVALID_ARG;
    }
    if (!syncTimeIsLocked(st, localRx)) {
        return LANE_ERR_SYNC_NO_LOCK;
    }

    /* Mainboard time at rx is mbTick * P + mbPhaseUs (+ a latency < P / 2);
     * the lane's is its anchor's tick * P + sinceAnchor. Their difference in
     * whole periods is how far the anchor sits from mbTick. */
    int64_t sinceAnchorUs = syncTimeLocalToUs(st, (int32_t)(localRx - st->anchorLocal));
    int64_t shift = syncTimeRoundDiv((int64_t)mbPhaseUs - sinceAnchorUs, (int64_t)st->nominalUs);

    uint32_t offset = mbTick + (uint32_t)(int32_t)shift - st->anchorIndex;
    if (st->aligned && offset != st->tickOffset) {
        st->realignShifts++;
    }
    st->tickOffset = offset;
    st->aligned    = true;
    return LANE_OK;
}

uint8_t syncTimeToMainboard(const syncTime_t *st, uint32_t local, uint32_t nowLocal,
                            uint64_t *mainboardUs)
{
    uint8_t flags = 0U;

    if (!st->aligned) {
        flags |= SYNC_TIME_FLAG_UNALIGNED;
    }

    if (!st->hasPeriod) {
        *mainboardUs = local;
        return (uint8_t)(flags | SYNC_TIME_FLAG_NO_LOCK);
    }

    if (!syncTimeIsLocked(st, nowLocal)) {
        flags |= SYNC_TIME_FLAG_EXTRAPOLATED;
    }

    uint32_t tick   = st->anchorIndex + (st->aligned ? st->tickOffset : 0U);
    int64_t  base   = (int64_t)((uint64_t)tick * st->nominalUs);
    int64_t  offset = syncTimeLocalToUs(st, (int32_t)(local - st->anchorLocal));
    int64_t  t      = base + offset;

    *mainboardUs = (t < 0) ? 0U : (uint64_t)t;
    return flags;
}

laneStatus_e syncTimeToLocal(const syncTime_t *st, uint64_t mainboardUs, uint32_t *local)
{
    if (!st->hasPeriod) {
        return LANE_ERR_SYNC_NO_LOCK;
    }
    if (!st->aligned) {
        return LANE_ERR_SYNC_UNALIGNED;
    }

    uint64_t base = (uint64_t)(st->anchorIndex + st->tickOffset) * st->nominalUs;
    int64_t  diff = (int64_t)(mainboardUs - base);

    /* Bound before scaling, so the product below cannot overflow. */
    if (diff > INT32_MAX || diff < INT32_MIN) {
        return LANE_ERR_INVALID_ARG;
    }

    int64_t counts = syncTimeRoundDiv(diff * (int64_t)st->periodQ8,
                                      (int64_t)st->nominalUs * 256);
    if (counts > INT32_MAX || counts < INT32_MIN) {
        return LANE_ERR_INVALID_ARG;
    }

    *local = st->anchorLocal + (uint32_t)(int32_t)counts;
    return LANE_OK;
}
