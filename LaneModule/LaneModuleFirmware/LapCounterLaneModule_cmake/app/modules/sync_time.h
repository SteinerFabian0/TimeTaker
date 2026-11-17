/*
 * SYNC edge bookkeeping and the local-time to mainboard-time mapping.
 *
 * Pure logic: no HAL, no globals, host-tested (test/host_sync_time). The TIM2
 * owner (lane_timer) feeds it hardware capture values from its ISR and guards
 * the struct against concurrent readers; nothing here is reentrant.
 *
 * Time axes:
 *   local     - TIM2 counts, 1 µs nominal from the lane's HSI (±1 %), 32-bit,
 *               wraps every ~71 min.
 *   mainboard - µs on the mainboard's axis, 64-bit. Mainboard tick k (one per
 *               SYNC period P) starts at k * P.
 *
 * Every accepted rising SYNC edge pairs a lane edge index with a local capture
 * c. An instant t maps to
 *     T = (index + tickOffset) * P + (t - c) * P / period
 * where period is the measured local length of one SYNC period, so the HSI
 * error cancels every period. tickOffset comes from syncTimeAlign(): the
 * mainboard names its tick once, and from then on the lane only counts edges.
 * No counter is ever reset.
 *
 * Robustness:
 *   - An edge that lands off the expected grid by more than P/16 is rejected
 *     as a glitch and the anchor kept, so one spike costs nothing.
 *   - Missed edges inside a gap of up to SYNC_TIME_MAX_GAP_PERIODS periods are
 *     counted by rounding, so the tick index survives short dropouts.
 *   - A longer gap, or a run of rejected edges (the anchor itself may have
 *     been a glitch, or the mainboard changed P), re-anchors on the newest
 *     edge and drops the alignment: the mainboard must align again, and the
 *     UNALIGNED flag tells it so.
 *   - Without an edge for SYNC_TIME_LOSS_PERIODS, mapping goes on with the
 *     last period and flags the result EXTRAPOLATED.
 *
 * Limits: the mainboard tick is 32-bit (49.7 days at 1 kHz), and an instant
 * more than ~35 min from the anchor maps with the wrong sign — both far
 * outside a race, and the second is flagged EXTRAPOLATED long before.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lane_status.h"

/* 1 kHz: the local 1 µs timer interpolates, so a faster SYNC only buys IRQs. */
#define SYNC_TIME_DEFAULT_PERIOD_US   1000U
#define SYNC_TIME_PERIOD_MIN_US       100U
#define SYNC_TIME_PERIOD_MAX_US       100000U

/* An edge must land within period / 16 (6 %) of the expected grid. The HSI
 * contributes ±1 %, the input filter and ISR nothing: the capture is in
 * hardware. */
#define SYNC_TIME_TOLERANCE_DIV       16U
/* Missed edges are counted by rounding up to this gap; past it the drift of
 * the period estimate could round to the wrong count. */
#define SYNC_TIME_MAX_GAP_PERIODS     32U
/* This many rejected edges in a row re-anchor on the newest one. */
#define SYNC_TIME_REANCHOR_REJECTS    4U
/* No edge for this many periods: SYNC counts as lost. */
#define SYNC_TIME_LOSS_PERIODS        4U

/* Quality flags of a mapped instant; they travel with every event. */
#define SYNC_TIME_FLAG_NO_LOCK        0x01U  /* No period measured: value is raw local µs */
#define SYNC_TIME_FLAG_UNALIGNED      0x02U  /* Tick index is lane-local, not the mainboard's */
#define SYNC_TIME_FLAG_EXTRAPOLATED   0x04U  /* SYNC lost: mapped with the last known period */

typedef struct {
    uint32_t nominalUs;     /* P: mainboard µs per SYNC period */

    uint32_t anchorLocal;   /* local capture of the last accepted edge */
    uint32_t anchorIndex;   /* lane edge count at that edge */
    uint32_t tickOffset;    /* mainboard tick = anchorIndex + tickOffset */
    uint32_t periodQ8;      /* measured local counts per SYNC period, Q24.8 */
    uint8_t  rejectRun;     /* consecutive rejected edges */

    bool hasAnchor;
    bool hasPeriod;
    bool aligned;

    /* Diagnostics for STATUS; they only count up. */
    uint32_t edgesAccepted;
    uint32_t edgesRejected;
    uint32_t edgesMissed;
    uint32_t reanchors;
    uint32_t realignShifts; /* syncTimeAlign() moved an existing alignment */
} syncTime_t;

/*
 * Forgets everything and sets P. Returns LANE_ERR_INVALID_ARG outside
 * SYNC_TIME_PERIOD_MIN_US..SYNC_TIME_PERIOD_MAX_US (the state is then
 * initialised with the default period, never left garbage).
 */
laneStatus_e syncTimeInit(syncTime_t *st, uint32_t nominalUs);

/*
 * One captured rising SYNC edge, local counts. Called from the TIM2 ISR.
 * Returns true if the edge was accepted onto the grid.
 */
bool syncTimeOnEdge(syncTime_t *st, uint32_t captureLocal);

/*
 * Pairs the lane's edge count with the mainboard's tick. The mainboard sends,
 * measured just before sending: mbTick, the tick of the last SYNC edge it
 * emitted, and mbPhaseUs, the µs since that edge. localRx is when the lane
 * received it. The lane resolves which of its own edges that was, so the
 * message may land on either side of an edge, as long as its latency stays
 * below P / 2.
 *
 * Returns LANE_ERR_SYNC_NO_LOCK if SYNC is not locked at localRx, and
 * LANE_ERR_INVALID_ARG for a phase of P or more.
 */
laneStatus_e syncTimeAlign(syncTime_t *st, uint32_t mbTick, uint32_t mbPhaseUs,
                           uint32_t localRx);

/*
 * Maps a local instant to mainboard µs. Always produces a value, so an event
 * is never dropped for lack of SYNC; the returned SYNC_TIME_FLAG_* bits say
 * how far to trust it. nowLocal decides whether SYNC counts as lost.
 */
uint8_t syncTimeToMainboard(const syncTime_t *st, uint32_t local, uint32_t nowLocal,
                            uint64_t *mainboardUs);

/*
 * Inverse: the local instant of a mainboard µs value, for scheduling. Needs
 * lock and alignment (LANE_ERR_SYNC_NO_LOCK / LANE_ERR_SYNC_UNALIGNED), and
 * returns LANE_ERR_INVALID_ARG for a target more than 2^31 local counts from
 * the anchor.
 */
laneStatus_e syncTimeToLocal(const syncTime_t *st, uint64_t mainboardUs, uint32_t *local);

/* Whether an edge arrived within the last SYNC_TIME_LOSS_PERIODS periods. */
bool syncTimeIsLocked(const syncTime_t *st, uint32_t nowLocal);
