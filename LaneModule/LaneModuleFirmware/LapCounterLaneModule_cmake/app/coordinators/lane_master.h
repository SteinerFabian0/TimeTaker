/**
 * @file lane_master.h
 * @brief Top-level coordinator: the firmware's main(), kept out of the
 *        CubeMX-owned main.c.
 *
 * Brings every module up in order and runs the super-loop. Owns the error
 * policy (laneErrorHandler: trap in debug, report and degrade in release),
 * the lane state machine of guide §3.1 (laneState_e of lane_protocol.h,
 * derived each pass from the coordinators), the STATUS snapshot, the boot
 * banner and the 1 Hz health trace, and the dispatch of the shared HAL EXTI
 * callback (EXTI0 SENS_INT -> vcnl4040, EXTI12 CHAIN_IN -> lane_chain).
 *
 *   sense_manager  sampler -> vcnl4040 -> lane_detect -> event FIFO
 *   bus_manager    lane_bus + lane_chain + register map + schedule queue
 *
 * Rule for everything laneRun() calls: no wait longer than about 10 ms. A
 * longer wait is a state-machine step, so the IWDG (about 1.5 s, fed once per
 * pass) can stay short. The pass ends in WFI (sys_power): every input is an
 * interrupt, and SysTick wakes the loop at least once per ms.
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
 * @brief Most recent error that reached laneErrorHandler() or
 *        laneMasterRecordError(), and how many did. lastError goes into
 *        STATUS. LANE_OK if none.
 */
laneStatus_e laneMasterGetLastError(void);
uint32_t     laneMasterGetErrorCount(void);

/**
 * @brief Records an error that is not a firmware fault (a rejected bus
 *        write, a refused hot-join) for STATUS, without the debug trap.
 */
void laneMasterRecordError(laneStatus_e statusCode);
