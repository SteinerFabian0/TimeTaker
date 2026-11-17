/*
 * System time: the one clock every timestamp in TimeTaker lives on.
 *
 * Owns TIM12. It runs from the 24 MHz crystal through PSI: prescaler 143,
 * ARR 999, so a 1 µs count and a 1 ms period (CubeMX). Each update event is
 * one tick. sysUs = ticks × 1000 + TIM12->CNT, a u64 that starts at boot and
 * never wraps in practice. Crystal error: ±20 ppm, about 1 ms per 50 s, the
 * same for every lane.
 *
 * CH2 drives SYNC (PB15, J301 pin 7): a 1 kHz square wave whose rising edge
 * IS the update event. Lanes capture it and map their own clocks onto sysUs
 * (lane sync_time). The SYNC *output* is switched separately from the
 * counter. The counter runs from sysTimeInit() on, but SYNC may only be driven
 * while LANE_3V3 is up, or about 30 mA flows into the unpowered lanes through
 * their ESD diodes (HARDWARE_REFERENCE §2.7). lane_manager couples the two.
 * With the channel stopped the pin parks low.
 *
 * This module also owns the weak HAL_TIM_UpdateCallback(): no other file may
 * define it. TIM1/TIM8 (LEDs) never enable their update interrupt.
 *
 * Context: the TIM12 ISR runs at priority 1 and only counts. sysTimeNowUs() is
 * exact from the main loop and from ISRs at priority 1 or lower urgency
 * (numerically >= 1). NOT from a priority-0 ISR (the power-fault EXTIs): the
 * generated HAL handler clears UIF before calling HAL_TIM_UpdateCallback(),
 * and a priority-0 ISR that lands between the two would read 1 ms low.
 * Priority-0 handlers record a flag; the main loop stamps it.
 * Low power: TIM12 stops in Stop1, so sysUs stands still there. The control
 * module is USB-powered and never uses Stop1 while lanes run.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ctrl_status.h"

#define SYS_TIME_TICK_US  1000U

/*
 * Starts the counter and its update interrupt. SYNC output stays off. Assumes
 * mx_tim12_init() has run. Returns CTRL_ERR_TIMER if the HAL refuses.
 */
ctrlStatus_e sysTimeInit(void);

/* Microseconds since sysTimeInit(), on the system time axis. */
uint64_t sysTimeNowUs(void);

/* Whole ticks (SYNC periods, ms) since sysTimeInit(). */
uint64_t sysTimeTicks(void);

/*
 * Starts (true) or stops (false) driving SYNC on PB15. Only lane_manager calls
 * this, after the lane rail has settled. Idempotent.
 */
ctrlStatus_e sysTimeSetSyncOutput(bool on);

bool sysTimeIsSyncOutputOn(void);
