/* Host shim for the CubeMX TIM1 handle; host_led defines it. */

#pragma once

#include "stm32_hal.h"

hal_tim_handle_t *mx_tim1_gethandle(void);
