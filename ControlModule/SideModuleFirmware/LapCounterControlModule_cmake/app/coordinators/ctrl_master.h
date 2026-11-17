/**
 * @file ctrl_master.h
 * @brief Top-level coordinator: the control module firmware's main(), kept out
 *        of the CubeMX-owned main.c.
 *
 * Brings every module up in a fixed order and runs the super-loop that ticks
 * the other coordinators. Owns the error policy. Same shape as the Yuhka hub's
 * hub_master and the lane's lane_master.
 *
 * Init order matters in one place: lane_chain parks the lane bus pins first,
 * before anything else runs, so the unpowered lanes are back-powered for as
 * short a time as possible after mx_system_init() configured them.
 */

#pragma once

#include <stdint.h>

#include "ctrl_status.h"

/**
 * @brief One-time boot sequence, called from main() once mx_system_init() has
 *        brought up the HAL and the clocks.
 */
void ctrlInit(void);

/**
 * @brief One super-loop iteration, called from main()'s while (1).
 */
void ctrlRun(void);

/* Last error the policy saw and how many there were (diagnostics). */
ctrlStatus_e ctrlMasterGetLastError(void);
uint32_t     ctrlMasterGetErrorCount(void);

/* Longest super-loop iteration since boot, in ms (target: < 1 ms). */
uint32_t ctrlMasterGetLoopMaxMs(void);
