/*
 * lane_manager.c
 *
 * @brief  Lane chain lifecycle. See lane_manager.h.
 */

#include "lane_manager.h"

#include <stdbool.h>
#include <stdint.h>

#include "stm32_hal.h"
#include "ctrl_status.h"
#include "lane_chain.h"
#include "sys_time.h"

typedef enum {
    LANE_POWER_OFF = 0,
    LANE_POWER_SETTLING,
    LANE_POWER_ON,
} lanePowerState_e;

static lanePowerState_e laneManagerPower;
static uint32_t         laneManagerRailOnMs;

static ctrlStatus_e laneManagerPowerOff(void)
{
    /* Signals before rail. Every step runs even if an earlier one fails: a
     * half-done power-down is worse than a reported error. */
    ctrlStatus_e status = sysTimeSetSyncOutput(false);
    ctrlStatus_e park   = laneChainParkBusPins();
    laneChainSetRail(false);
    laneManagerPower = LANE_POWER_OFF;
    return (status != CTRL_OK) ? status : park;
}

ctrlStatus_e laneManagerInit(void)
{
    /* lane_chain already parked everything at its init; make the state agree. */
    return laneManagerPowerOff();
}

ctrlStatus_e laneManagerSetPower(bool on)
{
    if (!on) {
        return laneManagerPowerOff();
    }
    if (laneManagerPower == LANE_POWER_OFF) {
        laneChainSetRail(true);
        laneManagerRailOnMs = HAL_GetTick();
        laneManagerPower    = LANE_POWER_SETTLING;
    }
    return CTRL_OK;
}

ctrlStatus_e laneManagerRun(void)
{
    if (laneManagerPower == LANE_POWER_SETTLING
        && HAL_GetTick() - laneManagerRailOnMs >= LANE_MANAGER_SETTLE_MS) {
        ctrlStatus_e status = sysTimeSetSyncOutput(true);
        if (status != CTRL_OK) {
            return status;               /* stays SETTLING: retried next pass */
        }
        laneManagerPower = LANE_POWER_ON;
    }
    return CTRL_OK;
}

bool laneManagerIsPowered(void)
{
    return laneManagerPower == LANE_POWER_ON;
}

bool laneManagerIsRailOn(void)
{
    return laneManagerPower != LANE_POWER_OFF;
}
