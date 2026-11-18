/*
 * EXTI latch: the one owner of the HAL EXTI trigger callback.
 *
 * CubeMX enables seven EXTI lines (CONTROL_FIRMWARE_NOTES). The HAL routes
 * all of them to one weak HAL_EXTI_TriggerCallback(), so one module has to
 * own it. It only latches; every decision is made in the main loop:
 *
 *   EXTI4  EXT_PWR_FAULT  prio 0   latched -> ctrl_master publishes EVT_FAULT
 *   EXTI12 USB_FAULT      prio 0   latched -> ctrl_master publishes EVT_FAULT
 *   EXTI5  BLE_IRQ        prio 6   latched -> nrf_link (it also polls the pin)
 *   EXTI14 BUS_INT        prio 3   ignored (reserved, HARDWARE_REFERENCE §2.7)
 *   EXTI2/13/15 buttons   prio 9   ignored (button_handler polls)
 *
 * Priority-0 lines only set a flag: no sysTimeNowUs() there (sys_time.h).
 *
 * Owns: HAL_EXTI_TriggerCallback(). Context: the EXTI ISRs write, the main
 * loop takes (read and clear under a guard).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    EXTI_LATCH_EXT_PWR_FAULT = 0,
    EXTI_LATCH_USB_FAULT,
    EXTI_LATCH_BLE_IRQ,
    EXTI_LATCH_COUNT
} extiLatch_e;

/* Returns how many edges were latched since the last take, and clears. */
uint32_t extiLatchTake(extiLatch_e line);
