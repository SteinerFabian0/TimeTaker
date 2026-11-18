/*
 * The nRF's control lines to the STM32. BLE_FIRMWARE_GUIDE.md §3.2, §7.3.
 *
 *   BLE_IRQ  (P0.05 -> PB5)  push-pull, high while a central is connected
 *   BOOT_REQ (P0.04 -> PB4)  push-pull, active low, inactive (high) at init
 *   NRST     (P0.14 -> NRST) OPEN DRAIN ONLY, active low, released at init
 *   BOOT0    (P0.12 -> BOOT0) never driven in v1: left a disconnected input
 *
 * The reset sequences run from a delayable work item on the system work
 * queue: no caller blocks. NOTHING triggers them in v1; the trigger is task
 * B5, decided together with the control bootloader (control task C7).
 */

#pragma once

#include <stdbool.h>

int stmCtrlInit(void);

/* BLE_IRQ: true while a central is connected. */
void stmCtrlSetBleIrq(bool connected);

/* Pulls NRST low for 10 ms, then releases it. false if a sequence is
 * already running. */
bool stmCtrlResetPulse(void);

/* BOOT_REQ low, NRST pulse (10 ms), BOOT_REQ held low 200 ms after the
 * pulse, then released. The control bootloader samples BOOT_REQ at reset.
 * false if a sequence is already running. B5: implemented, not wired. */
bool stmCtrlResetIntoBootloader(void);

bool stmCtrlBusy(void);
