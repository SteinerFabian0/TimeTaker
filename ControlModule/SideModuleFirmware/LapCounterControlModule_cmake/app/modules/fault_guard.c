/*
 * Fault containment. See fault_guard.h; policy in docs/ERROR_HANDLING.md.
 * The .noinit breadcrumb survives every reset except power-on (magic word).
 */

#include "fault_guard.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "stm32_hal.h"
#include "stm32c5xx_hal_dbgmcu.h"   /* not pulled in by the umbrella header in this config */
#include "mx_iwdg.h"
#include "ctrl_status.h"
#include "boot_shared.h"

#define FAULT_GUARD_MAGIC  0xFA17C0DEU

typedef struct {
    uint32_t magic;
    uint32_t abnormalResetCount;
    int32_t  lastFatalStatus;
    uint32_t lastFaultId;
    uint32_t lastFaultIpsr;
    uint32_t lastFaultPc;
    uint32_t lastFaultLr;
    uint32_t lastFaultCfsr;
    uint32_t lastFaultAddr;
    uint32_t pendingFault;      /* set by a handler right before its reset */
    uint32_t lastResetFlags;
    uint32_t eccNmis;
} faultGuardBreadcrumb_t;

/* Not static: easy to find in the .map and the debugger. */
volatile faultGuardBreadcrumb_t faultGuardBreadcrumb __attribute__((section(".noinit")));

/* The app's half of the shared block; boot_shared.h. */
bootShared_t bootShared __attribute__((section(".boot_shared")));

static bool faultGuardFaultBoot;
static bool faultGuardWatchdogBoot;
static bool faultGuardParked;
static bool faultGuardWatchdogOn;
static bool faultGuardStable;

static bool faultGuardDebuggerAttached(void)
{
    return (DCB->DHCSR & DCB_DHCSR_C_DEBUGEN_Msk) != 0U;
}

void faultGuardInit(void)
{
    const uint32_t flags = HAL_RCC_GetResetSource();   /* device_id reads and clears them */

    if (faultGuardBreadcrumb.magic != FAULT_GUARD_MAGIC) {
        faultGuardBreadcrumb.magic              = FAULT_GUARD_MAGIC;
        faultGuardBreadcrumb.abnormalResetCount = 0U;
        faultGuardBreadcrumb.lastFatalStatus    = 0;
        faultGuardBreadcrumb.lastFaultId        = FAULT_GUARD_ID_NONE;
        faultGuardBreadcrumb.lastFaultIpsr      = 0U;
        faultGuardBreadcrumb.lastFaultPc        = 0U;
        faultGuardBreadcrumb.lastFaultLr        = 0U;
        faultGuardBreadcrumb.lastFaultCfsr      = 0U;
        faultGuardBreadcrumb.lastFaultAddr      = 0U;
        faultGuardBreadcrumb.pendingFault       = 0U;
        faultGuardBreadcrumb.eccNmis            = 0U;
    }
    faultGuardBreadcrumb.lastResetFlags = flags;

    faultGuardFaultBoot    = faultGuardBreadcrumb.pendingFault != 0U;
    faultGuardWatchdogBoot = (flags & (HAL_RCC_RESET_FLAG_IWDG | HAL_RCC_RESET_FLAG_WWDG)) != 0U;
    faultGuardBreadcrumb.pendingFault = 0U;

    /* A REBOOT request is a software reset too, so only faults and the
     * watchdog count as abnormal. A power-on clears the count (magic). */
    if (faultGuardFaultBoot || faultGuardWatchdogBoot) {
        faultGuardBreadcrumb.abnormalResetCount++;
    } else {
        faultGuardBreadcrumb.abnormalResetCount = 0U;
    }

    faultGuardParked = faultGuardBreadcrumb.abnormalResetCount > FAULT_GUARD_MAX_ABNORMAL
                       && !faultGuardDebuggerAttached();
}

void faultGuardGetBootInfo(faultGuardBootInfo_t *info)
{
    info->resetFlags      = faultGuardBreadcrumb.lastResetFlags;
    info->faultReboot     = faultGuardFaultBoot;
    info->watchdogReboot  = faultGuardWatchdogBoot;
    info->lastFaultId     = (faultGuardFaultId_e)faultGuardBreadcrumb.lastFaultId;
    info->lastFatalStatus = faultGuardBreadcrumb.lastFatalStatus;
    info->lastFaultIpsr   = faultGuardBreadcrumb.lastFaultIpsr;
    info->lastFaultPc     = faultGuardBreadcrumb.lastFaultPc;
    info->lastFaultLr     = faultGuardBreadcrumb.lastFaultLr;
    info->lastFaultCfsr   = faultGuardBreadcrumb.lastFaultCfsr;
    info->lastFaultAddr   = faultGuardBreadcrumb.lastFaultAddr;
    info->abnormalResets  = faultGuardBreadcrumb.abnormalResetCount;
    info->eccNmis         = faultGuardBreadcrumb.eccNmis;
}

bool faultGuardIsParked(void)
{
    return faultGuardParked;
}

ctrlStatus_e faultGuardCheckOptionBytes(void)
{
    const uint32_t cur = FLASH->OPTSR_CUR;
    /* BOOT_SEL = 1: the BOOT0 pin decides (LL_FLASH_OB_BOOT0_BOOTPIN). This
     * board has a real pull-down (R201) and the SW604 BOOT button. */
    const bool bootPin     = (cur & FLASH_OPTSR_CUR_BOOT_SEL) != 0U;
    const bool iwdgStdbyFz = (cur & FLASH_OPTSR_CUR_IWDG_STDBY) == 0U;
    return (bootPin && iwdgStdbyFz) ? CTRL_OK : CTRL_ERR_OPTION_BYTES;
}

ctrlStatus_e faultGuardStartWatchdog(void)
{
    hal_iwdg_handle_t *h = mx_iwdg_init();
    if (h == NULL) {
        return CTRL_ERR_HAL;
    }
    HAL_DBGMCU_IWDG_Freeze();   /* only acts on a halted core: safe in every build */
    if (HAL_IWDG_Start(h, 0U, FAULT_GUARD_WATCHDOG_MS, 0U) != HAL_OK) {
        return CTRL_ERR_HAL;
    }
    faultGuardWatchdogOn = true;
    return CTRL_OK;
}

bool faultGuardWatchdogRunning(void)
{
    return faultGuardWatchdogOn;
}

void faultGuardTick(void)
{
    if (faultGuardWatchdogOn) {
        (void)HAL_IWDG_Refresh(mx_iwdg_gethandle());
    }
    if (!faultGuardStable && HAL_GetTick() >= FAULT_GUARD_STABLE_MS) {
        faultGuardStable = true;
        faultGuardBreadcrumb.abnormalResetCount = 0U;
    }
}

void faultGuardFatal(ctrlStatus_e status)
{
    faultGuardBreadcrumb.magic           = FAULT_GUARD_MAGIC;
    faultGuardBreadcrumb.lastFaultId     = FAULT_GUARD_ID_FATAL_STATUS;
    faultGuardBreadcrumb.lastFatalStatus = (int32_t)status;
    faultGuardBreadcrumb.pendingFault    = 1U;
    NVIC_SystemReset();
    while (1) { }
}

void faultGuardInjectHardFault(void)
{
    /* An execute from the XN peripheral region: a precise, always-faulting
     * access whatever the MPU setup. */
    void (*bad)(void) = (void (*)(void))0xE0000001U;
    bad();
}

void faultGuardInjectHang(void)
{
    while (1) {
        __NOP();
    }
}


/* =========================================================================
 *             FAULT HANDLERS (override the startup file's weak ones)
 * No HAL here: the system state is unknown.
 * ========================================================================= */

static void faultGuardRecord(faultGuardFaultId_e id, uint32_t pc, uint32_t lr, uint32_t xpsr)
{
    uint32_t cfsr = SCB->CFSR;
    uint32_t addr = 0U;
    if ((cfsr & SCB_CFSR_BFARVALID_Msk) != 0U) {
        addr = SCB->BFAR;
    } else if ((cfsr & SCB_CFSR_MMARVALID_Msk) != 0U) {
        addr = SCB->MMFAR;
    }
    faultGuardBreadcrumb.magic         = FAULT_GUARD_MAGIC;
    faultGuardBreadcrumb.lastFaultId   = id;
    faultGuardBreadcrumb.lastFaultIpsr = xpsr & IPSR_ISR_Msk;
    faultGuardBreadcrumb.lastFaultPc   = pc;
    faultGuardBreadcrumb.lastFaultLr   = lr;
    faultGuardBreadcrumb.lastFaultCfsr = cfsr;
    faultGuardBreadcrumb.lastFaultAddr = addr;
    faultGuardBreadcrumb.pendingFault  = 1U;
}

__attribute__((used, noreturn))
void faultGuardCaptureHardFault(uint32_t pc, uint32_t lr, uint32_t xpsr)
{
    faultGuardRecord(FAULT_GUARD_ID_HARDFAULT, pc, lr, xpsr);
    NVIC_SystemReset();
    while (1) { }
}

/* Naked: any prologue would move SP off the frame before it is captured. MSP
 * reset before C, as on the hub: after a stack overflow C on the old MSP
 * faults again and locks up with no breadcrumb. */
__attribute__((naked))
void HardFault_Handler(void)
{
    __asm volatile(
        "tst   lr, #4                      \n"
        "ite   eq                          \n"
        "mrseq r3, msp                     \n"
        "mrsne r3, psp                     \n"
        "ldr   r0, [r3, #24]               \n"   /* stacked PC   */
        "ldr   r1, [r3, #20]               \n"   /* stacked LR   */
        "ldr   r2, [r3, #28]               \n"   /* stacked xPSR */
        "ldr   r3, =0xE000ED08             \n"   /* SCB->VTOR    */
        "ldr   r3, [r3]                    \n"
        "ldr   r3, [r3]                    \n"   /* initial MSP  */
        "msr   msp, r3                     \n"
        "b     faultGuardCaptureHardFault  \n");
}

/* The flash raises NMI on a double ECC error. On a settings page that a power
 * loss cut off mid-program this is expected once: clear it and carry on (the
 * read returned garbage that the record CRC rejects). Anything else is a fault. */
void NMI_Handler(void)
{
    if (LL_FLASH_IsActiveFlag_ECCD(FLASH) != 0U) {
        LL_FLASH_ClearFlag_ECCD(FLASH);
        faultGuardBreadcrumb.eccNmis++;
        return;
    }
    faultGuardRecord(FAULT_GUARD_ID_NMI, 0U, 0U, __get_IPSR());
    NVIC_SystemReset();
    while (1) { }
}

void Default_IRQHandler_Hook(void)
{
    faultGuardRecord(FAULT_GUARD_ID_UNEXPECTED_IRQ, 0U, 0U, __get_IPSR());
    NVIC_SystemReset();
    while (1) { }
}
