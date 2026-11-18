/*
 * Sleep / Standby for the lane. See sys_power.h for the modes.
 */

#include "sys_power.h"
#include "lane_behavior.h"

#include <stdint.h>

#include "stm32_hal.h"
#include "stm32c5xx_ll_dbgmcu.h"
#include "mx_gpio_default.h"
#include "fault_guard.h"
#include "indicator_led.h"

static const uint32_t sysPowerWakeSensInt = HAL_PWR_WAKEUP_PIN_1;   /* PA0, SENS_INT */
static const uint32_t sysPowerWakeBusInt  = HAL_PWR_WAKEUP_PIN_2;   /* PA2, BUS_INT  */

void sysPowerInit(void)
{
    /* Guide §8.1: mx_pwr_init() armed WKUP1 at boot; disarm both. */
    HAL_PWR_LP_DisableWakeupPin(sysPowerWakeSensInt | sysPowerWakeBusInt);

    /* Never DBG_STANDBY: it keeps the core domain powered through Standby. */
#if LANE_DEBUG_DURING_LOW_POWER
    LL_DBGMCU_EnableDebugLowPowerMode(LL_DBGMCU_SLEEP_MODE_DEBUG | LL_DBGMCU_STOP_MODE_DEBUG);
#else
    LL_DBGMCU_DisableDebugLowPowerMode(LL_DBGMCU_SLEEP_MODE_DEBUG | LL_DBGMCU_STOP_MODE_DEBUG
                                       | LL_DBGMCU_STANDBY_MODE_DEBUG);
#endif
}

void sysPowerIdle(void)
{
    HAL_PWR_EnterSleepMode(HAL_PWR_LOW_PWR_MODE_WFI);
}

laneStatus_e sysPowerEnterStandby(void)
{
    if (faultGuardCheckOptionBytes() != LANE_OK) {
        return LANE_ERR_OPTION_BYTES;
    }

    /* BUS_INT is an open-drain output released high; reading the pad gives
     * the line. Already low: the wake would be lost, so refuse. */
    if (HAL_GPIO_ReadPin(BUS_INT_PORT, BUS_INT_PIN) == HAL_GPIO_PIN_RESET) {
        return LANE_ERR_LOW_POWER_REFUSED;
    }

    (void)indicatorLedSet(false);

    hal_pwr_wakeup_pin_config_t cfg;
    cfg.polarity = HAL_PWR_WAKEUP_PIN_POLARITY_LOW;
    cfg.pull     = HAL_PWR_WAKEUP_PIN_PULL_NO;    /* 10 k pull-up at the control module */
    if (HAL_PWR_LP_SetConfigWakeupPin(sysPowerWakeBusInt, &cfg) != HAL_OK) {
        return LANE_ERR_HAL;
    }
    HAL_PWR_LP_EnableWakeupPin(sysPowerWakeBusInt);
    HAL_PWR_LP_CleanWakeupSource(sysPowerWakeSensInt | sysPowerWakeBusInt);

    if (HAL_GPIO_ReadPin(BUS_INT_PORT, BUS_INT_PIN) == HAL_GPIO_PIN_RESET) {
        HAL_PWR_LP_DisableWakeupPin(sysPowerWakeBusInt);
        return LANE_ERR_LOW_POWER_REFUSED;
    }

    faultGuardMarkStandbyEntry();
    LL_DBGMCU_DisableDebugLowPowerMode(LL_DBGMCU_STANDBY_MODE_DEBUG);

    SysTick->CTRL &= ~SysTick_CTRL_TICKINT_Msk;
    for (uint32_t i = 0U; i < 8U; i++) {
        NVIC->ICER[i] = 0xFFFFFFFFU;
        NVIC->ICPR[i] = 0xFFFFFFFFU;
    }
    __DSB();
    __ISB();

    HAL_PWR_EnterStandbyMode();

    /* Not reached; a refused entry falls through to a clean reset. */
    NVIC_SystemReset();
    while (1) { }
}
