/*
 * Fault containment. Policy in fault_guard.h. The .noinit breadcrumb (see the
 * linker script) survives every reset except power-on; a magic word tells.
 */

#include "fault_guard.h"
#include "lane_behavior.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "stm32_hal.h"
#include "stm32c5xx_ll_iwdg.h"
#include "stm32c5xx_ll_dbgmcu.h"

/* LSI 32 kHz / 32 = 1 kHz counter; ±30 % LSI makes the real bite 1.05-1.95 s.
 * Every laneRun() pass is far below 10 ms (guide §7). */
#define FAULT_GUARD_IWDG_RELOAD          1500U
#define FAULT_GUARD_IWDG_READY_SPINS     100000U

/* A lane that ran this long counts as healthy again. */
#define FAULT_GUARD_STABLE_UPTIME_MS     60000U

#define FAULT_GUARD_MAGIC                0x1A9EFA17U

typedef struct {
    uint32_t magic;
    uint32_t abnormalResetCount;   /* consecutive watchdog/fault/fatal resets      */
    int32_t  lastFatalStatus;      /* laneStatus_e passed to faultGuardFatal       */
    uint32_t lastFaultId;          /* faultGuardFaultId_e written before the reset */
    uint32_t lastFaultIpsr;
    uint32_t lastFaultPc;
    uint32_t lastFaultLr;
    uint32_t lastFaultCfsr;
    uint32_t lastFaultAddr;
    uint32_t faultPending;         /* set with lastFaultId, consumed at boot        */
    uint32_t commandedReset;       /* COMMAND RESET: not abnormal                   */
    uint32_t standbyPending;
    uint32_t lastResetFlags;
} faultGuardBreadcrumb_t;

/* Not static: easy to find in the .map and in Live Watch. */
volatile faultGuardBreadcrumb_t faultGuardBreadcrumb __attribute__((section(".noinit")));

static faultGuardBootInfo_t faultGuardBoot;
static bool                 faultGuardWatchdogStarted;
static bool                 faultGuardUptimeStable;

static bool faultGuardDebuggerAttached(void)
{
    return (DCB->DHCSR & DCB_DHCSR_C_DEBUGEN_Msk) != 0U;
}

static faultGuardResetCause_e faultGuardCause(uint32_t flags, bool faultPending)
{
    if ((flags & HAL_RCC_RESET_FLAG_IWDG) != 0U || (flags & HAL_RCC_RESET_FLAG_WWDG) != 0U) {
        return FAULT_GUARD_RESET_WATCHDOG;
    }
    if ((flags & HAL_RCC_RESET_FLAG_SW) != 0U) {
        return faultPending ? FAULT_GUARD_RESET_FAULT : FAULT_GUARD_RESET_SOFTWARE;
    }
    if ((flags & HAL_RCC_RESET_FLAG_PWR) != 0U) {
        /* BOR flag covers POR/PDR too; without the magic word RAM was lost,
         * so it was a real power-up. */
        return (faultGuardBreadcrumb.magic == FAULT_GUARD_MAGIC)
               ? FAULT_GUARD_RESET_BROWNOUT : FAULT_GUARD_RESET_POWER_ON;
    }
    if ((flags & HAL_RCC_RESET_FLAG_PIN) != 0U) {
        return FAULT_GUARD_RESET_PIN;
    }
    return FAULT_GUARD_RESET_UNKNOWN;
}

void faultGuardInit(void)
{
    uint32_t flags = HAL_RCC_GetResetSource();
    HAL_RCC_ClearResetFlags();

    bool magicOk = faultGuardBreadcrumb.magic == FAULT_GUARD_MAGIC;
    bool pending = magicOk && faultGuardBreadcrumb.faultPending != 0U;

    faultGuardBoot.resetFlags = flags;
    faultGuardBoot.resetCause = faultGuardCause(flags, pending);

    if (!magicOk) {
        faultGuardBreadcrumb.magic              = FAULT_GUARD_MAGIC;
        faultGuardBreadcrumb.abnormalResetCount = 0U;
        faultGuardBreadcrumb.lastFatalStatus    = 0;
        faultGuardBreadcrumb.lastFaultId        = FAULT_GUARD_ID_NONE;
        faultGuardBreadcrumb.lastFaultIpsr      = 0U;
        faultGuardBreadcrumb.lastFaultPc        = 0U;
        faultGuardBreadcrumb.lastFaultLr        = 0U;
        faultGuardBreadcrumb.lastFaultCfsr      = 0U;
        faultGuardBreadcrumb.lastFaultAddr      = 0U;
        faultGuardBreadcrumb.commandedReset     = 0U;
        faultGuardBreadcrumb.standbyPending     = 0U;
    }
    faultGuardBreadcrumb.faultPending   = 0U;
    faultGuardBreadcrumb.lastResetFlags = flags;

    faultGuardBoot.lastFaultId     = FAULT_GUARD_ID_NONE;
    faultGuardBoot.lastFatalStatus = faultGuardBreadcrumb.lastFatalStatus;
    faultGuardBoot.lastFaultPc     = faultGuardBreadcrumb.lastFaultPc;
    faultGuardBoot.lastFaultLr     = faultGuardBreadcrumb.lastFaultLr;
    faultGuardBoot.lastFaultCfsr   = faultGuardBreadcrumb.lastFaultCfsr;
    if (pending) {
        faultGuardBoot.lastFaultId = (faultGuardFaultId_e)faultGuardBreadcrumb.lastFaultId;
    } else if (faultGuardBoot.resetCause == FAULT_GUARD_RESET_WATCHDOG) {
        faultGuardBoot.lastFaultId = FAULT_GUARD_ID_WATCHDOG;
    }

    bool commanded = faultGuardBreadcrumb.commandedReset != 0U;
    faultGuardBreadcrumb.commandedReset = 0U;

    if (faultGuardBreadcrumb.standbyPending != 0U) {
        faultGuardBreadcrumb.standbyPending = 0U;
        faultGuardBoot.abnormalResets = faultGuardBreadcrumb.abnormalResetCount;
        return;
    }

    bool abnormal = !commanded
                    && (faultGuardBoot.resetCause == FAULT_GUARD_RESET_WATCHDOG
                        || faultGuardBoot.resetCause == FAULT_GUARD_RESET_FAULT);
    if (abnormal) {
        faultGuardBreadcrumb.abnormalResetCount++;
    } else {
        faultGuardBreadcrumb.abnormalResetCount = 0U;
    }
    faultGuardBoot.abnormalResets = faultGuardBreadcrumb.abnormalResetCount;

    if (faultGuardBreadcrumb.abnormalResetCount > FAULT_GUARD_MAX_ABNORMAL_RESETS
        && !faultGuardDebuggerAttached()) {
        /* Park: stays parked until a commanded reset or a power cycle, so the
         * control module can read why. The count is kept, so a lane that is
         * reset by the IWDG while parked parks again. */
        faultGuardBoot.parked = true;
        if (faultGuardBoot.lastFaultId == FAULT_GUARD_ID_NONE) {
            faultGuardBoot.lastFaultId = FAULT_GUARD_ID_BOOT_LOOP;
        }
    }
}

void faultGuardGetBootInfo(faultGuardBootInfo_t *info)
{
    if (info != NULL) {
        *info = faultGuardBoot;
    }
}

bool faultGuardIsParked(void)
{
    return faultGuardBoot.parked;
}

laneStatus_e faultGuardCheckOptionBytes(void)
{
    /* IWDG_STDBY = 0 freezes the IWDG in Standby. */
    if ((FLASH->OPTSR_CUR & FLASH_OPTSR_CUR_IWDG_STDBY) != 0U) {
        return LANE_ERR_OPTION_BYTES;
    }
    return LANE_OK;
}

void faultGuardStartWatchdog(void)
{
    /* Only acts on a halted core, so safe in every build. */
    LL_DBGMCU_APB1_GRP1_FreezePeriph(LL_DBGMCU_IWDG_STOP);

    LL_IWDG_Enable(IWDG);
    LL_IWDG_EnableWriteAccess(IWDG);
    LL_IWDG_SetPrescaler(IWDG, LL_IWDG_PRESCALER_32);
    LL_IWDG_SetReloadCounter(IWDG, FAULT_GUARD_IWDG_RELOAD);

    /* Bounded: the registers update in the LSI domain within a few LSI
     * cycles. The IWDG runs regardless; this only makes the reload take. */
    for (uint32_t i = 0U; i < FAULT_GUARD_IWDG_READY_SPINS && LL_IWDG_IsReady(IWDG) == 0U; i++) {
    }
    LL_IWDG_ReloadCounter(IWDG);

    faultGuardWatchdogStarted = true;
}

void faultGuardTick(void)
{
    if (faultGuardWatchdogStarted) {
        LL_IWDG_ReloadCounter(IWDG);
    }

    if (!faultGuardUptimeStable && HAL_GetTick() >= FAULT_GUARD_STABLE_UPTIME_MS
        && !faultGuardBoot.parked) {
        faultGuardUptimeStable = true;
        faultGuardBreadcrumb.abnormalResetCount = 0U;
    }
}

void faultGuardFatal(laneStatus_e statusCode)
{
    faultGuardBreadcrumb.magic           = FAULT_GUARD_MAGIC;
    faultGuardBreadcrumb.lastFaultId     = FAULT_GUARD_ID_FATAL_STATUS;
    faultGuardBreadcrumb.lastFatalStatus = (int32_t)statusCode;
    faultGuardBreadcrumb.faultPending    = 1U;

    NVIC_SystemReset();
    while (1) { }
}

void faultGuardCommandedReset(void)
{
    faultGuardBreadcrumb.magic              = FAULT_GUARD_MAGIC;
    faultGuardBreadcrumb.commandedReset     = 1U;
    faultGuardBreadcrumb.abnormalResetCount = 0U;

    NVIC_SystemReset();
    while (1) { }
}

void faultGuardMarkStandbyEntry(void)
{
    faultGuardBreadcrumb.magic          = FAULT_GUARD_MAGIC;
    faultGuardBreadcrumb.standbyPending = 1U;
}


/* =========================================================================
 *          FAULT HANDLERS (override the startup file's weak ones)
 *
 * Every other exception and unclaimed IRQ aliases Default_IRQHandler, whose
 * hook lands here. No HAL: the system state is unknown.
 * ========================================================================= */

__attribute__((used, noreturn))
void faultGuardCaptureHardFault(uint32_t pc, uint32_t lr, uint32_t xpsr)
{
    uint32_t cfsr = SCB->CFSR;
    uint32_t addr = 0U;

    if ((cfsr & SCB_CFSR_BFARVALID_Msk) != 0U) {
        addr = SCB->BFAR;
    } else if ((cfsr & SCB_CFSR_MMARVALID_Msk) != 0U) {
        addr = SCB->MMFAR;
    }

    faultGuardBreadcrumb.magic         = FAULT_GUARD_MAGIC;
    faultGuardBreadcrumb.lastFaultId   = FAULT_GUARD_ID_HARDFAULT;
    faultGuardBreadcrumb.lastFaultIpsr = xpsr & IPSR_ISR_Msk;
    faultGuardBreadcrumb.lastFaultPc   = pc;
    faultGuardBreadcrumb.lastFaultLr   = lr;
    faultGuardBreadcrumb.lastFaultCfsr = cfsr;
    faultGuardBreadcrumb.lastFaultAddr = addr;
    faultGuardBreadcrumb.faultPending  = 1U;

    NVIC_SystemReset();
    while (1) { }
}

/* Naked: a prologue would move SP off the frame before it is captured. MSP is
 * reset before C, so a stack-overflow fault cannot fault again (hub lesson). */
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

void Default_IRQHandler_Hook(void)
{
    faultGuardBreadcrumb.magic         = FAULT_GUARD_MAGIC;
    faultGuardBreadcrumb.lastFaultId   = FAULT_GUARD_ID_UNEXPECTED_IRQ;
    faultGuardBreadcrumb.lastFaultIpsr = __get_IPSR();
    faultGuardBreadcrumb.faultPending  = 1U;

    NVIC_SystemReset();
    while (1) { }
}
