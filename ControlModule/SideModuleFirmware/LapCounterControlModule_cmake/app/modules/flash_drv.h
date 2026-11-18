/*
 * Flash driver: page erase and quad-word programming of the main flash, for
 * settings_store and the bootloader's update writer.
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
 * Owns: the FLASH control registers (unlock, program, erase). Never touches
 * the option bytes.
 * Context: main loop only. Programming masks interrupts for the 4-word store
 * (as the HAL does); erase can be started and polled without blocking.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ctrl_status.h"

#define FLASH_DRV_QW_BYTES  16U

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
