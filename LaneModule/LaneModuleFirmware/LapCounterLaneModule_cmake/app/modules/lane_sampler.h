/*
 * The sampling schedule (guide §3.4): fires the VCNL4040 force-mode trigger
 * from a TIM2 compare, so the sample instant is deterministic, and reads the
 * result back after the conversion time.
 *
 * Owns TIM2 alarm LANE_TIMER_ALARM_SAMPLE (CH2) and, with
 * LANE_ENABLE_SCOPE_MARKER, the CS pad TP6 (PB4): low from trigger to data,
 * so a scope shows the sample rate and the sensor latency.
 *
 * One sample, all in interrupts:
 *   CH2 alarm at the due instant (TIM2 ISR, prio 1) -> vcnl4040Trigger
 *   trigger written (I2C ISR, prio 3)                -> CH2 alarm at due + conversion
 *   CH2 alarm                                        -> vcnl4040ReadPs
 *   data read (I2C ISR)                              -> sample into the ring, next due armed
 *
 * Schedule: sample k is due at mainboard time k × period + phase, so lanes
 * with staggered phases never fire their 200 mA IR pulses together. Mapped
 * to local time through sync_time once SYNC is locked and aligned; before
 * that (and in the bench standalone mode) the grid runs on local time with
 * the same period. A change of alignment, period or a fall-behind re-grids
 * from now, so a sample is skipped rather than fired twice.
 *
 * Timestamp of a sample: the trigger instant (the TIM2 counter when the
 * CH2 ISR ran, not the compare's due time) + the trigger write time + half
 * the conversion (BENCH: LANE_SAMPLER_LATENCY_TRIM_US corrects it once
 * measured, BENCH_TESTS.md procedure 4).
 *
 * A trigger that finds the previous sample still on the bus is skipped and
 * counted (overrun). Failed transfers are counted and skipped; vcnl4040
 * counts the run of failures for the recovery decision.
 *
 * Main loop: start/stop/period and laneSamplerPop(). Everything else runs in
 * the ISRs above. The ring is single-producer (I2C ISR), single-consumer.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lane_status.h"

/* BENCH: added to the computed latency once the scope says otherwise. */
#define LANE_SAMPLER_LATENCY_TRIM_US  0

/* First sample at least this far ahead of a start, so the alarm arms cleanly. */
#define LANE_SAMPLER_START_LEAD_US    200U

#define LANE_SAMPLER_RING             64U

typedef struct {
    uint32_t tLocal;        /* sample instant, local µs                      */
    uint16_t raw;           /* PS_DATA                                       */
    uint32_t periodUs;      /* schedule period the sample belongs to         */
} laneSample_t;

laneStatus_e laneSamplerInit(void);

/*
 * Starts the schedule. convUs is the sensor conversion time for the current
 * settings (lane_regs). oneShot takes a single sample and stops (COMMAND
 * SINGLE_SAMPLE). The sensor must be ACTIVE.
 */
laneStatus_e laneSamplerStart(uint32_t periodUs, uint32_t phaseUs, uint32_t convUs, bool oneShot);

/* Stops; a transfer in flight finishes and is discarded. */
void laneSamplerStop(void);

/* New period (holdoff watch rate), same phase; takes effect at the next
 * scheduling step. */
void laneSamplerSetPeriod(uint32_t periodUs);

bool laneSamplerRunning(void);

uint32_t laneSamplerPeriod(void);

/* Next finished sample, oldest first. */
bool laneSamplerPop(laneSample_t *out);

/* Diagnostics; they only count up. */
uint32_t laneSamplerCount(void);
uint32_t laneSamplerOverruns(void);
uint32_t laneSamplerErrors(void);
uint32_t laneSamplerRingDrops(void);
