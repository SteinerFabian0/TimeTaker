/*
 * CHAIN pins: CHAIN_IN (PB12, input with pull-down, EXTI12 both edges,
 * priority 6) and CHAIN_OUT (PA1, push-pull, low at boot).
 *
 * Owns both pins. The rule itself (debounce, CHAIN_OUT = address AND
 * CHAIN_IN, hot-join backoff, the RSTDAA hold) is chain_logic, host-tested;
 * this module only samples the pin and drives the output from the main loop.
 * The EXTI12 edge only wakes the loop out of WFI and is counted.
 *
 * Low power: CHAIN_OUT keeps its level through Standby (CubeMX GPIO
 * retention), so a sleeping lane does not drop its downstream.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lane_status.h"
#include "chain_logic.h"

/* Reads CHAIN_IN, drives CHAIN_OUT low. */
laneStatus_e laneChainInit(uint32_t nowMs);

/* Samples CHAIN_IN, updates the rule, drives CHAIN_OUT. Returns the
 * debounced edge, if any. */
chainEvent_e laneChainRun(uint32_t nowMs);

/* The rule state, for bus_manager to tell it about addresses and joins. */
chainLogic_t *laneChainLogic(void);

/* Drives CHAIN_OUT from the rule now (after an address change). */
void laneChainApply(void);

bool laneChainOut(void);

/* EXTI12 edge (HAL_EXTI_TriggerCallback dispatch). */
void laneChainOnEdge(void);

uint32_t laneChainEdgeCount(void);
