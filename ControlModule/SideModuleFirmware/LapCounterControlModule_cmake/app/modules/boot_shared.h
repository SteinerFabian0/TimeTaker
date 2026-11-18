/*
 * The 16 bytes of RAM the app and the bootloader share across a reset.
 *
 * Both linker scripts put the .boot_shared section (NOLOAD) first in RAM, at
 * 0x2000_0000, so the two images agree on its address without a symbol in
 * common. Neither startup zeroes it. Every field is valid only with its magic
 * and is cleared by whoever consumes it.
 *
 *   request   REBOOT{BOOTLOADER} in the app: BOOT_SHARED_ENTER_BOOT, then a
 *             reset. The bootloader stays and waits for an update.
 *   jump      BOOT_APP in the bootloader: BOOT_SHARED_JUMP_APP, then a reset;
 *             the bootloader's first instructions see it and jump to the app
 *             before any peripheral is touched (a clean start for the app).
 */

#pragma once

#include <stdint.h>

#define BOOT_SHARED_ENTER_BOOT  0xB0071EADU
#define BOOT_SHARED_JUMP_APP    0xA9900A99U

typedef struct {
    volatile uint32_t request;
    volatile uint32_t jump;
    volatile uint32_t reserved[2];
} bootShared_t;

extern bootShared_t bootShared;
