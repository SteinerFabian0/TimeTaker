/*
 * Lane chain pins: the lane rail switch and the signals that must never be
 * driven into unpowered lanes.
 *
 *   LANE_PWR_EN  PA6   U403 TPS22917 ON (100 k pull-down: lanes off by default)
 *   CHAIN_OUT    PA10  enumeration line to lane 1, via 47 Ω
 *   BUS_SCL/SDA  PA9/PA8  I3C1, via 47 Ω; SDA/BUS_INT pull-ups go to LANE_3V3
 *
 * The rule (HARDWARE_REFERENCE §2.7): while LANE_3V3 is off, SCL, SDA, SYNC
 * and CHAIN_OUT are low or Hi-Z. Otherwise about 30 mA flows into the lanes
 * through their ESD diodes. This module parks the I3C pins as analog (Hi-Z)
 * and holds CHAIN_OUT low. SYNC belongs to sys_time. The power sequencing,
 * which needs time and the SYNC switch, belongs to lane_manager. This module
 * only does what each call says, at once.
 *
 * I3C1 init is "generated only" (CubeMX pass of 2026-11-17): mx_system_init()
 * no longer touches PA8/PA9, so they stay in their reset state (analog) until
 * laneChainInit() parks them explicitly. Step 3 calls mx_i3c1_init() itself,
 * after the rail has settled; that call also sets PA8/PA9 to AF2.
 *
 * Owns: PA6 (LANE_PWR_EN), PA10 (CHAIN_OUT), and the I3C pins PA8/PA9 while
 *       parked. Step 3 hands PA8/PA9 back to the I3C controller through
 *       laneChainReleaseBusPins().
 */

#pragma once

#include <stdbool.h>

#include "ctrl_status.h"

/* Parks the bus pins, CHAIN_OUT low, rail off. Call first in ctrlInit(). */
ctrlStatus_e laneChainInit(void);

/* Switches the rail. No sequencing here: see lane_manager. */
void laneChainSetRail(bool on);
bool laneChainIsRailOn(void);

/* I3C pins to analog (Hi-Z), CHAIN_OUT low. */
ctrlStatus_e laneChainParkBusPins(void);

/* I3C pins back to their I3C alternate function (step 3, rail up only). */
ctrlStatus_e laneChainReleaseBusPins(void);
