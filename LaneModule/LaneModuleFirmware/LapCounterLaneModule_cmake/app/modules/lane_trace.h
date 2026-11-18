/*
 * Text trace over SWO (PB3, ITM stimulus port 0), for the bench: boot info,
 * state changes, every event, and on request the signal. BENCH_TESTS.md
 * names what to look for.
 *
 * Owns ITM stimulus port 0. Never blocks: lines go into a RAM ring and
 * laneTraceFlush() (once per laneRun pass) moves bytes out only while the
 * ITM FIFO has room. With no debugger the ITM is disabled and the ring is
 * simply discarded; a full ring drops whole lines and counts them.
 *
 * Main loop only (the ring is not ISR-safe). ISRs count into variables that
 * the coordinators trace from the loop.
 *
 * laneTraceLevel (volatile, set it from Live Watch without a rebuild):
 *   0 off
 *   1 boot, state changes, events, errors, 1 Hz health line (default)
 *   2 + the signal at 10 Hz (raw, baseline, thresholds)
 *   3 + every sample (about 30 bytes per sample: at 1 kHz that is 30 KB/s,
 *     which the 2 MHz SWO carries; drops are counted if it does not)
 *
 * Format: laneTrace() takes %u %d %x (all 32-bit integers), %s and %c. Pass
 * 32-bit values only; 64-bit times go through laneTraceU64().
 *
 * Compiled to nothing unless LANE_ENABLE_SWO_TRACE (debug builds).
 *
 * Low power: the ITM keeps working in Sleep (WFI); no Stop is entered.
 */

#pragma once

#include <stdint.h>

#include "lane_behavior.h"

#define LANE_TRACE_OFF      0U
#define LANE_TRACE_EVENTS   1U
#define LANE_TRACE_SIGNAL   2U
#define LANE_TRACE_SAMPLES  3U

#if LANE_ENABLE_SWO_TRACE

extern volatile uint8_t  laneTraceLevel;
extern volatile uint32_t laneTraceDropped;

/* One line; a '\n' is appended. Dropped whole if the ring is full. */
void laneTrace(uint8_t level, const char *fmt, ...);

/* One line "<label> <s>s+<µs>us" for a 64-bit µs value. */
void laneTraceU64Line(uint8_t level, const char *label, uint64_t us);

/* Moves buffered bytes to the ITM while it has room. */
void laneTraceFlush(void);

#define LANE_TRACE_ON(level)  (laneTraceLevel >= (level))

#else

/* Functions, not empty macros: the arguments stay "used" in every build, and
 * the optimiser removes the calls. */
static inline void laneTrace(uint8_t level, const char *fmt, ...)
{
    (void)level;
    (void)fmt;
}

static inline void laneTraceU64Line(uint8_t level, const char *label, uint64_t us)
{
    (void)level;
    (void)label;
    (void)us;
}

static inline void laneTraceFlush(void)
{
}
#define LANE_TRACE_ON(level)               (0)

#endif /* LANE_ENABLE_SWO_TRACE */
