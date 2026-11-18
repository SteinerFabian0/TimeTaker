/*
 * System power. See sys_power.h.
 */

#include "sys_power.h"

#include "stm32_hal.h"
#include "ctrl_behavior.h"

void sysPowerIdle(void)
{
#if !CTRL_DISABLE_IDLE_WFI
    HAL_PWR_EnterSleepMode(HAL_PWR_LOW_PWR_MODE_WFI);
#endif
}
