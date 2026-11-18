/*
 * Flash driver: page erase and quad-word programming of the main flash, for
 * settings_store. Its write fence admits the two settings pages only.
 *
 * Why LL and not the HAL flash driver: the generated stm32c5xx_hal_conf.h has
 * USE_HAL_FLASH_PROGRAM_BY_ADDR / ERASE_PAGE at 0, so the HAL's program and
 * erase functions are not compiled. Switching them on is a CubeMX change; the
 * register sequence is short, so this module does it with the LL (the same
 * steps as the HAL's FLASH_Program / FLASH_ErasePage).
 *
 * Geometry (DFP stm32c542xx.h, datasheet DS15125 §3.4): 2 banks x 16 pages of
 * 8 KB, programmed in 128-bit quad-words, erase 2.0-2.1 ms per page. The two
 * banks are read-while-write: erasing a page of bank 2 does not stall code
 * running from bank 1, which is why the settings live at the end of bank 2.
 *
 * Flash map (v1 flashes the image over SWD; no bootloader, no field update):
 *
 *   0x0800_0000  app        240 KB  (30 pages) linker script stm32c542xc_flash.ld
 *   0x0803_C000  settings    16 KB  (2 pages)  bank 2, pages 14 and 15
 *
 * The bootloader layout that put ctrl_boot first is at git tag
 * archive/bootloader-update.
 *
 * Owns: the FLASH control registers (unlock, program, erase). Never touches
 * the option bytes, never erases or programs outside the settings pages.
 * Context: main loop only. Programming masks interrupts for the 4-word store
 * (as the HAL does); erase can be started and polled without blocking.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ctrl_status.h"

#define FLASH_DRV_QW_BYTES        16U
#define FLASH_DRV_SETTINGS_BASE   0x0803C000U   /* the last two 8 KB pages */
#define FLASH_DRV_SETTINGS_SIZE   0x4000U

/* Everything below targets the settings pages only; any other address is
 * CTRL_ERR_INVALID_ARG. */

/* Starts erasing the page that holds addr (main flash). Poll flashDrvBusy(). */
ctrlStatus_e flashDrvEraseStart(uint32_t addr);

/* True while an erase or program runs. */
bool flashDrvBusy(void);

/* After flashDrvBusy() went false: CTRL_ERR_FLASH if the operation failed. */
ctrlStatus_e flashDrvResult(void);

/* Blocking erase of one page (about 2 ms), with a 10 ms timeout. */
ctrlStatus_e flashDrvErasePage(uint32_t addr);

/* Programs one quad-word at a 16-aligned, erased address. Blocking, about
 * 20-160 µs. */
ctrlStatus_e flashDrvProgram16(uint32_t addr, const uint8_t data[FLASH_DRV_QW_BYTES]);
