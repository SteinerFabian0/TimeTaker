/*
 * Lane chain pins. See lane_chain.h.
 */

#include "lane_chain.h"

#include <stdbool.h>

#include "stm32_hal.h"
#include "mx_gpio_default.h"
#include "mx_i3c1.h"
#include "ctrl_status.h"

static bool laneChainRailOn;

ctrlStatus_e laneChainParkBusPins(void)
{
    HAL_GPIO_WritePin(CHAIN_OUT_PORT, CHAIN_OUT_PIN, HAL_GPIO_PIN_RESET);

    hal_gpio_config_t cfg = {
        .mode        = HAL_GPIO_MODE_ANALOG,
        .pull        = HAL_GPIO_PULL_NO,
        .speed       = HAL_GPIO_SPEED_FREQ_LOW,
        .output_type = HAL_GPIO_OUTPUT_PUSHPULL,
        .alternate   = HAL_GPIO_AF_0,
        .init_state  = HAL_GPIO_PIN_RESET,
    };
    return ctrlStatusFromHal(HAL_GPIO_Init(BUS_SCL_PORT, BUS_SCL_PIN | BUS_SDA_PIN, &cfg));
}

ctrlStatus_e laneChainReleaseBusPins(void)
{
    if (!laneChainRailOn) {
        return CTRL_ERR_INVALID_STATE;
    }

    /* Same settings as mx_i3c1_init(). */
    hal_gpio_config_t cfg = {
        .mode        = HAL_GPIO_MODE_ALTERNATE,
        .pull        = HAL_GPIO_PULL_NO,
        .speed       = HAL_GPIO_SPEED_FREQ_HIGH,
        .output_type = HAL_GPIO_OUTPUT_PUSHPULL,
        .alternate   = HAL_GPIO_AF_2,
        .init_state  = HAL_GPIO_PIN_RESET,
    };
    return ctrlStatusFromHal(HAL_GPIO_Init(BUS_SCL_PORT, BUS_SCL_PIN | BUS_SDA_PIN, &cfg));
}

ctrlStatus_e laneChainSetChainOut(bool high)
{
    if (high && !laneChainRailOn) {
        return CTRL_ERR_INVALID_STATE;
    }
    HAL_GPIO_WritePin(CHAIN_OUT_PORT, CHAIN_OUT_PIN, high ? HAL_GPIO_PIN_SET : HAL_GPIO_PIN_RESET);
    return CTRL_OK;
}

void laneChainSetRail(bool on)
{
    HAL_GPIO_WritePin(LANE_PWR_EN_PORT, LANE_PWR_EN_PIN, on ? HAL_GPIO_PIN_SET : HAL_GPIO_PIN_RESET);
    laneChainRailOn = on;
}

bool laneChainIsRailOn(void)
{
    return laneChainRailOn;
}

ctrlStatus_e laneChainInit(void)
{
    /* Signals before rail, as on every power-down. */
    ctrlStatus_e status = laneChainParkBusPins();
    laneChainSetRail(false);
    return status;
}
