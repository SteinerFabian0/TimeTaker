/*
 * The nRF's control lines to the STM32. BLE_FIRMWARE_GUIDE.md §3.2, §7.3.
 *
 *   BLE_IRQ  (P0.05 -> PB5)  push-pull, high while a central is connected
 *   BOOT_REQ (P0.04 -> PB4)  push-pull, driven high (inactive) for good: unused
 *                            in v1, which has no STM bootloader (SWD only)
 *   NRST     (P0.14 -> NRST) OPEN DRAIN ONLY, active low, released at init
 *   BOOT0    (P0.12 -> BOOT0) never driven in v1: left a disconnected input
 *
 * The reset pulse runs from a delayable work item on the system work queue:
 * no caller blocks. Nothing triggers it in v1. The bootloader strap
 * (BOOT_REQ + NRST, task B5) was removed with the control bootloader; it is
 * at git tag archive/bootloader-update.
 */

#pragma once

#include <stdbool.h>

int stmCtrlInit(void);

/* BLE_IRQ: true while a central is connected. */
void stmCtrlSetBleIrq(bool connected);

/* Pulls NRST low for 10 ms, then releases it. false if a sequence is
 * already running. */
bool stmCtrlResetPulse(void);

bool stmCtrlBusy(void);
