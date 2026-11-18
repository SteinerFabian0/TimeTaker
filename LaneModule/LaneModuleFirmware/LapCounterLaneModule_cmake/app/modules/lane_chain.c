/*
 * CHAIN pins. See lane_chain.h; the rule is chain_logic.
 */

#include "lane_chain.h"

#include <stdbool.h>
#include <stdint.h>

#include "stm32_hal.h"
#include "mx_gpio_default.h"

static chainLogic_t      laneChainState;
static bool              laneChainOutLevel;
static volatile uint32_t laneChainEdges;

static bool laneChainReadIn(void)
{
    return HAL_GPIO_ReadPin(CHAIN_IN_PORT, CHAIN_IN_PIN) == HAL_GPIO_PIN_SET;
}

void laneChainApply(void)
{
    bool out = chainLogicChainOut(&laneChainState);
    if (out != laneChainOutLevel) {
        HAL_GPIO_WritePin(CHAIN_OUT_PORT, CHAIN_OUT_PIN,
                          out ? CHAIN_OUT_ACTIVE_STATE : CHAIN_OUT_INACTIVE_STATE);
        laneChainOutLevel = out;
    }
}

laneStatus_e laneChainInit(uint32_t nowMs)
{
    chainLogicInit(&laneChainState, nowMs);
    HAL_GPIO_WritePin(CHAIN_OUT_PORT, CHAIN_OUT_PIN, CHAIN_OUT_INACTIVE_STATE);
    laneChainOutLevel = false;
    (void)chainLogicUpdate(&laneChainState, laneChainReadIn(), nowMs);
    return LANE_OK;
}

chainEvent_e laneChainRun(uint32_t nowMs)
{
    chainEvent_e e = chainLogicUpdate(&laneChainState, laneChainReadIn(), nowMs);
    laneChainApply();
    return e;
}

chainLogic_t *laneChainLogic(void)
{
    return &laneChainState;
}

bool laneChainOut(void)
{
    return laneChainOutLevel;
}

void laneChainOnEdge(void)
{
    laneChainEdges++;
}

uint32_t laneChainEdgeCount(void)
{
    return laneChainEdges;
}
