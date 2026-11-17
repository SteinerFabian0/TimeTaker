/**
 * @file lane_master.h
 * @brief Top-level coordinator: the firmware's main(), kept out of the
 *        CubeMX-owned main.c.
 *
 * Brings every module up in order and runs the super-loop. Owns the error
 * policy (laneErrorHandler: trap in debug, report and degrade in release) and,
 * as the bring-up order proceeds, the lane state machine of guide §3.1.
 *
 * Rule for everything laneRun() calls: no wait longer than about 10 ms. A
 * longer wait is a state-machine step, so the IWDG (fed once per pass, later)
 * can stay short.
 */

#pragma once

#include <stdint.h>

#include "lane_status.h"

/**
 * @brief One-time boot sequence, called from main() once mx_system_init() has
 *        brought up the HAL and the system clock.
 */
void laneInit(void);

/**
 * @brief One super-loop iteration, called from main()'s while (1).
 */
void laneRun(void);

/**
 * @brief Most recent error that reached laneErrorHandler(), and how many did.
 *        Both go into STATUS once the register map exists. LANE_OK if none.
 */
laneStatus_e laneMasterGetLastError(void);
uint32_t     laneMasterGetErrorCount(void);
