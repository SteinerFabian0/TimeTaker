/*
 * Short critical sections for state shared between the main loop and an ISR.
 *
 * PRIMASK save/restore, so the guard nests and is safe to take from inside an
 * ISR. Keep the guarded region to a handful of register writes or a struct
 * copy: it delays every interrupt, the SYNC tick (TIM12) included.
 */

#pragma once

#include <stdint.h>

#include "stm32_hal.h"

static inline uint32_t irqGuardEnter(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    return primask;
}

static inline void irqGuardExit(uint32_t primask)
{
    __set_PRIMASK(primask);
}
