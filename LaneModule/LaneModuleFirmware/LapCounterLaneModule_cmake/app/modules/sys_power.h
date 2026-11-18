/*
 * Power modes of the lane, ported from the Yuhka hub's sys_power (guide §3.8,
 * §8.1-8.3).
 *
 * Owns the PWR wake-pin configuration and the low-power entry points.
 *
 *   IDLE (Sleep, WFI)  sysPowerIdle(): the super-loop's tail. Core stops,
 *                      every peripheral and IRQ keeps running; any interrupt
 *                      (SysTick 1 ms at the latest) resumes the loop. This is
 *                      the only mode the lane uses in v1, and the per-lane
 *                      "sleep" of COMMAND SLEEP 1 is IDLE plus sensor and LED
 *                      off (sense_manager / bus_manager), still addressable.
 *   NAP (Stop1)        NOT IMPLEMENTED: an I3C target wakes from Stop only on
 *                      a reset pattern or a missed start, never on its
 *                      address (datasheet §3.24), so a napping lane cannot be
 *                      woken individually (guide §9). The hub's sysPowerStopFor
 *                      is the template once the bench answers §9.
 *   STANDBY            sysPowerEnterStandby(): whole-system decision, wake =
 *                      reset via WKUP2 (BUS_INT low). Implemented to the hub's
 *                      rules but not reachable from the bus in v1 (COMMAND
 *                      SLEEP 3 is rejected). CHAIN_OUT keeps its level through
 *                      Standby by the CubeMX GPIO retention.
 *
 * Hub lesson §8.1: an armed WKUP pin shadows its GPIO input. The generated
 * mx_pwr_init() arms WKUP1 (PA0, SENS_INT) at boot; sysPowerInit() disarms
 * it. Wake pins are armed only at the edge of Standby.
 *
 * Context: main loop only.
 */

#pragma once

#include "lane_status.h"

/* Disarms the boot-armed WKUP1 and, per LANE_DEBUG_DURING_LOW_POWER, keeps
 * SWD alive through Sleep. Call early in laneInit. */
void sysPowerInit(void);

/* WFI until the next interrupt. */
void sysPowerIdle(void);

/*
 * Standby with wake on BUS_INT low (WKUP2). Turns the LED off first, refuses
 * (returns LANE_ERR_LOW_POWER_REFUSED) when BUS_INT is already low, and
 * LANE_ERR_OPTION_BYTES when the IWDG would keep running in Standby. On
 * success it does not return: the wake is a reset.
 */
laneStatus_e sysPowerEnterStandby(void);
