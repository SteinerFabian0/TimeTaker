/*
 * Flash driver. See flash_drv.h.
 */

#include "flash_drv.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "stm32_hal.h"
#include "ctrl_status.h"
#include "irq_guard.h"

#define FLASH_DRV_BASE        0x08000000U
#define FLASH_DRV_PAGE        0x2000U
#define FLASH_DRV_PAGES_BANK  16U
#define FLASH_DRV_ERASE_MS    10U
#define FLASH_DRV_PROG_MS     2U

static bool flashDrvErasing;

static void flashDrvUnlock(void)
{
    if (LL_FLASH_IsLocked(FLASH) != 0U) {
        LL_FLASH_SetUnlockKey(FLASH, LL_FLASH_KEY1);
        LL_FLASH_SetUnlockKey(FLASH, LL_FLASH_KEY2);
    }
}

static bool flashDrvReady(void)
{
    if (LL_FLASH_ReadFlag(FLASH, LL_FLASH_FLAG_ERRORS_ALL) != 0U) {
        LL_FLASH_ClearFlag(FLASH, LL_FLASH_FLAG_ERRORS_ALL);   /* stale error of an earlier op */
    }
    return LL_FLASH_IsActiveFlag(FLASH, LL_FLASH_FLAG_STATUS_ALL) == 0U;
}

bool flashDrvBusy(void)
{
    return LL_FLASH_IsActiveFlag(FLASH, LL_FLASH_FLAG_STATUS_ALL) != 0U;
}

ctrlStatus_e flashDrvResult(void)
{
    if (flashDrvErasing && !flashDrvBusy()) {
        LL_FLASH_DisablePageErase(FLASH);
        flashDrvErasing = false;
        LL_FLASH_Lock(FLASH);
    }
    if (LL_FLASH_ReadFlag(FLASH, LL_FLASH_FLAG_ERRORS_ALL) != 0U) {
        LL_FLASH_ClearFlag(FLASH, LL_FLASH_FLAG_ERRORS_ALL);
        return CTRL_ERR_FLASH;
    }
    LL_FLASH_ClearFlag_EOP(FLASH);
    return CTRL_OK;
}

ctrlStatus_e flashDrvEraseStart(uint32_t addr)
{
    /* The write fence: the settings pages only, never the running image. */
    if (addr < FLASH_DRV_SETTINGS_BASE || addr >= FLASH_DRV_SETTINGS_BASE + FLASH_DRV_SETTINGS_SIZE) {
        return CTRL_ERR_INVALID_ARG;
    }
    if (!flashDrvReady()) {
        return CTRL_ERR_BUSY;
    }
    const uint32_t page = (addr - FLASH_DRV_BASE) / FLASH_DRV_PAGE;
    const uint32_t bank = (page < FLASH_DRV_PAGES_BANK) ? LL_FLASH_ERASE_BANK_1 : LL_FLASH_ERASE_BANK_2;

    flashDrvUnlock();
    flashDrvErasing = true;
    LL_FLASH_StartErasePage(FLASH, bank, LL_FLASH_ERASE_USER_AREA, page % FLASH_DRV_PAGES_BANK);
    return CTRL_OK;
}

static ctrlStatus_e flashDrvWait(uint32_t timeoutMs)
{
    const uint32_t start = HAL_GetTick();
    while (flashDrvBusy()) {
        if (HAL_GetTick() - start > timeoutMs) {
            return CTRL_ERR_TIMEOUT;
        }
    }
    return flashDrvResult();
}

ctrlStatus_e flashDrvErasePage(uint32_t addr)
{
    CTRL_RETURN_IF_ERR(flashDrvEraseStart(addr));
    return flashDrvWait(FLASH_DRV_ERASE_MS);
}

ctrlStatus_e flashDrvProgram16(uint32_t addr, const uint8_t data[FLASH_DRV_QW_BYTES])
{
    if ((addr & (FLASH_DRV_QW_BYTES - 1U)) != 0U || addr < FLASH_DRV_SETTINGS_BASE
        || addr + FLASH_DRV_QW_BYTES > FLASH_DRV_SETTINGS_BASE + FLASH_DRV_SETTINGS_SIZE) {
        return CTRL_ERR_INVALID_ARG;
    }
    if (!flashDrvReady()) {
        return CTRL_ERR_BUSY;
    }

    uint32_t words[4];
    memcpy(words, data, sizeof(words));

    flashDrvUnlock();
    LL_FLASH_EnableProgramming(FLASH);

    /* The four words of one quad-word back to back (the HAL masks IRQs here
     * too: an interrupt that touched flash in between would split it). */
    volatile uint32_t *dst = (volatile uint32_t *)addr;
    uint32_t guard = irqGuardEnter();
    dst[0] = words[0];
    dst[1] = words[1];
    dst[2] = words[2];
    dst[3] = words[3];
    irqGuardExit(guard);

    ctrlStatus_e status = flashDrvWait(FLASH_DRV_PROG_MS);
    LL_FLASH_DisableProgramming(FLASH);
    LL_FLASH_Lock(FLASH);
    if (status == CTRL_OK && memcmp((const void *)addr, data, FLASH_DRV_QW_BYTES) != 0) {
        status = CTRL_ERR_FLASH;
    }
    return status;
}
