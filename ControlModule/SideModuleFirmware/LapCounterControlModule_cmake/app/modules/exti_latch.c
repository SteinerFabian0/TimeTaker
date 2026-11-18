/*
 * EXTI latch. See exti_latch.h.
 */

#include "exti_latch.h"

#include <stdbool.h>
#include <stdint.h>

#include "stm32_hal.h"
#include "irq_guard.h"

static volatile uint32_t extiLatchCount[EXTI_LATCH_COUNT];

void HAL_EXTI_TriggerCallback(hal_exti_handle_t *hexti, hal_exti_trigger_t trigger)
{
    (void)trigger;
    switch (hexti->line) {
    case HAL_EXTI_LINE_4:
        extiLatchCount[EXTI_LATCH_EXT_PWR_FAULT]++;
        break;
    case HAL_EXTI_LINE_12:
        extiLatchCount[EXTI_LATCH_USB_FAULT]++;
        break;
    case HAL_EXTI_LINE_5:
        extiLatchCount[EXTI_LATCH_BLE_IRQ]++;
        break;
    default:
        break;   /* buttons and BUS_INT: polled or reserved */
    }
}

uint32_t extiLatchTake(extiLatch_e line)
{
    if (line >= EXTI_LATCH_COUNT) {
        return 0U;
    }
    uint32_t guard = irqGuardEnter();
    uint32_t n = extiLatchCount[line];
    extiLatchCount[line] = 0U;
    irqGuardExit(guard);
    return n;
}
