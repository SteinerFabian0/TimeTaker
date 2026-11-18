/*
 * Fault containment: watchdog, fault handlers, breadcrumb, boot-loop guard.
 *
 * Port of the Yuhka hub's fault_guard (docs/ERROR_HANDLING.md), adapted to a
 * timing box that is always on USB and must stay reachable:
 *
 *   - the independent watchdog (about 2 s, CONTROL_FIRMWARE_GUIDE §4), started
 *     once in ctrlInit() and fed once per ctrlRun() pass by faultGuardTick();
 *   - HardFault, unexpected-IRQ and NMI handlers that write a .noinit
 *     breadcrumb and reset. The next boot reports RESET_FAULT in EVT_BOOT;
 *   - the NMI of a double ECC error while reading flash (a settings page cut
 *     off mid-write by a power loss) is counted and returned from: the read
 *     data is garbage, the settings CRC refuses it, defaults load;
 *   - a boot-loop guard: more than FAULT_GUARD_MAX_ABNORMAL abnormal resets
 *     (watchdog or fault) in a row PARK the firmware. Unlike the hub there is
 *     no Standby to sulk in: parked means ctrl_master starts no watchdog and
 *     powers nothing (the lanes stay off anyway until a host asks), so a host
 *     can still read STATUS and update, or, with the bootloader present, the
 *     app hands over to the bootloader. Stood down while a probe is attached.
 *
 * Context: faultGuardInit() first in ctrlInit(); faultGuardTick() every pass;
 * the handlers at fault level. No HAL in the handlers.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ctrl_status.h"

#define FAULT_GUARD_WATCHDOG_MS     2000U   /* LSI ±30 %: 1.4 .. 2.6 s */
#define FAULT_GUARD_MAX_ABNORMAL    5U
#define FAULT_GUARD_STABLE_MS       60000U  /* healthy uptime that forgets old faults */

typedef enum {
    FAULT_GUARD_ID_NONE           = 0,
    FAULT_GUARD_ID_HARDFAULT      = 1,
    FAULT_GUARD_ID_UNEXPECTED_IRQ = 2,
    FAULT_GUARD_ID_FATAL_STATUS   = 3,
    FAULT_GUARD_ID_NMI            = 4,
} faultGuardFaultId_e;

typedef struct {
    uint32_t            resetFlags;      /* RCC flags of THIS boot */
    bool                faultReboot;     /* this boot follows a breadcrumb reset */
    bool                watchdogReboot;
    faultGuardFaultId_e lastFaultId;     /* the last fault ever seen (not cleared) */
    int32_t             lastFatalStatus;
    uint32_t            lastFaultIpsr;
    uint32_t            lastFaultPc;
    uint32_t            lastFaultLr;
    uint32_t            lastFaultCfsr;
    uint32_t            lastFaultAddr;
    uint32_t            abnormalResets;
    uint32_t            eccNmis;         /* double ECC errors survived, all boots */
} faultGuardBootInfo_t;

/* Call FIRST in ctrlInit(). Reads (does not clear) the RCC reset flags,
 * validates the breadcrumb and runs the boot-loop guard. */
void faultGuardInit(void);

void faultGuardGetBootInfo(faultGuardBootInfo_t *info);

/* The boot-loop guard tripped: ctrl_master must not start the watchdog. */
bool faultGuardIsParked(void);

/* CTRL_ERR_OPTION_BYTES unless BOOT_SEL = 1 (the BOOT0 pin decides) and the
 * IWDG is frozen in Standby (tools/optprog.py). Reported, never fatal. */
ctrlStatus_e faultGuardCheckOptionBytes(void);

/* Starts the IWDG (mx_iwdg_init + HAL_IWDG_Start). Freezes it on a debug halt. */
ctrlStatus_e faultGuardStartWatchdog(void);

bool faultGuardWatchdogRunning(void);

/* Every ctrlRun() pass: feeds the IWDG, ages the abnormal-reset count. */
void faultGuardTick(void);

/* Unrecoverable: breadcrumb + reset. */
void faultGuardFatal(ctrlStatus_e status) __attribute__((noreturn));

/* Bench hooks (CTRL_ENABLE_FAULT_INJECT): a real HardFault, and a hung loop
 * the watchdog must catch. */
void faultGuardInjectHardFault(void);
void faultGuardInjectHang(void) __attribute__((noreturn));
