/*
 * Lane manager: the lane chain's lifecycle.
 *
 * Step 2 scope: the lane rail and SYNC, sequenced so that no signal is ever
 * driven into unpowered lanes (HARDWARE_REFERENCE §2.7):
 *
 *   power on:   rail on → wait LANE_MANAGER_SETTLE_MS (U403 ramps in ~16 ms)
 *               → SYNC on.
 *   power off:  SYNC off, bus pins parked, CHAIN_OUT low → rail off. All at
 *               once, signals first.
 *
 * Step 3 adds the rest of the lifecycle: hand the bus pins to I3C, CHAIN
 * enumeration, config push, SYNC alignment, event collection and lane health.
 * Lanes start unpowered at boot. Powering them is the host's decision (the
 * LANE_POWER request), until step 3 decides when to do it automatically.
 *
 * Owns: the policy over lane_chain and the SYNC output of sys_time.
 * Context: main loop only.
 */

#pragma once

#include <stdbool.h>

#include "ctrl_status.h"

/* U403 TPS22917 with CT = 10 nF ramps in about 16 ms into ~600 µF of lane
 * capacitance; the lanes' own boot needs a few ms more before SYNC matters. */
#define LANE_MANAGER_SETTLE_MS  30U

ctrlStatus_e laneManagerInit(void);

ctrlStatus_e laneManagerRun(void);

/* Starts the power-on sequence or performs the power-off one. Idempotent. */
ctrlStatus_e laneManagerSetPower(bool on);

/* Rail settled and SYNC running. */
bool laneManagerIsPowered(void);

/* Rail on, settled or not. */
bool laneManagerIsRailOn(void);
