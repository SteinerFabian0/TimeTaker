/* Host shim for the CubeMX TIM8 handle; host_led defines it. */

#pragma once

#include "stm32_hal.h"

hal_tim_handle_t *mx_tim8_gethandle(void);
