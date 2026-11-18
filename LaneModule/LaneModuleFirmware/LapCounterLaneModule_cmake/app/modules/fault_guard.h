/*
 * Last-line fault containment for the lane, ported from the Yuhka hub's
 * fault_guard (guide §7).
 *
 * Every unexpected condition ends in one of: a module-local retry (never seen
 * here), a report in STATUS (laneErrorHandler), a clean reboot (this module),
 * or a park. Never a hang, never a hot reboot loop. Unlike the hub, the lane
 * has a channel back, so a parked lane stays on the bus and says why.
 *
 * This module owns:
 *   - the IWDG (LL, no CubeMX instance: the HAL IWDG module is off in this
 *     project), about 1.5 s, started once and fed once per laneRun() pass;
 *     frozen while a debugger halts the core;
 *   - HardFault_Handler and Default_IRQHandler_Hook (breadcrumb + reset);
 *   - a .noinit breadcrumb that survives every reset but power-on;
 *   - the reset cause for STATUS (host protocol resetCause values) and the
 *     fault id of the last fault boot;
 *   - the boot-loop guard: more than FAULT_GUARD_MAX_ABNORMAL_RESETS abnormal
 *     resets in a row PARK the lane (sensor off, LED off, still enumerable,
 *     STATUS state PARKED + faultId). Stood down while a probe is attached.
 *     A commanded reset (COMMAND RESET) is not abnormal and un-parks.
 *
 * Option bytes: IWDG_STDBY must freeze the IWDG in Standby before Standby is
 * ever used (sys_power refuses otherwise). The lane does not enter Standby in
 * v1, so an unprogrammed chip still boots.
 *
 * Context: init and tick from the main loop; the fault handlers run in any
 * context and touch nothing but the breadcrumb and the reset.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lane_status.h"

typedef enum {
    FAULT_GUARD_ID_NONE           = 0,
    FAULT_GUARD_ID_HARDFAULT      = 1,  /* HardFault_Handler ran                 */
    FAULT_GUARD_ID_UNEXPECTED_IRQ = 2,  /* an unhandled/aliased ISR fired        */
    FAULT_GUARD_ID_FATAL_STATUS   = 3,  /* faultGuardFatal(laneStatus_e) called  */
    FAULT_GUARD_ID_WATCHDOG       = 4,  /* the IWDG bit (no breadcrumb written)  */
    FAULT_GUARD_ID_BOOT_LOOP      = 5,  /* this boot parked the lane             */
} faultGuardFaultId_e;

/* Host protocol resetCause (host_protocol.toml), sent in STATUS.resetCause. */
typedef enum {
    FAULT_GUARD_RESET_UNKNOWN  = 0,
    FAULT_GUARD_RESET_POWER_ON = 1,
    FAULT_GUARD_RESET_PIN      = 2,
    FAULT_GUARD_RESET_SOFTWARE = 3,
    FAULT_GUARD_RESET_WATCHDOG = 4,
    FAULT_GUARD_RESET_BROWNOUT = 5,
    FAULT_GUARD_RESET_FAULT    = 6,
} faultGuardResetCause_e;

#define FAULT_GUARD_MAX_ABNORMAL_RESETS  5U

typedef struct {
    uint32_t               resetFlags;      /* raw RCC flags of this boot          */
    faultGuardResetCause_e resetCause;
    faultGuardFaultId_e    lastFaultId;     /* fault behind this boot, else NONE    */
    int32_t                lastFatalStatus;
    uint32_t               lastFaultPc;
    uint32_t               lastFaultLr;
    uint32_t               lastFaultCfsr;
    uint32_t               abnormalResets;
    bool                   parked;
} faultGuardBootInfo_t;

/* Call FIRST in laneInit: reads and clears the reset cause, validates the
 * breadcrumb and runs the boot-loop guard. */
void faultGuardInit(void);

void faultGuardGetBootInfo(faultGuardBootInfo_t *info);

/* Boot-loop guard tripped this boot. */
bool faultGuardIsParked(void);

/* LANE_ERR_OPTION_BYTES unless the option bytes freeze the IWDG in Standby. */
laneStatus_e faultGuardCheckOptionBytes(void);

/* Starts the IWDG (about 1.5 s). Once started it cannot be stopped. */
void faultGuardStartWatchdog(void);

/* Once per laneRun(): feeds the IWDG and, after a stretch of healthy uptime,
 * clears the abnormal-reset count. */
void faultGuardTick(void);

/* Unrecoverable: records the status in the breadcrumb and reboots. */
void faultGuardFatal(laneStatus_e statusCode) __attribute__((noreturn));

/* COMMAND RESET: a clean, commanded reboot that the boot-loop guard does not
 * count (and that un-parks a parked lane). */
void faultGuardCommandedReset(void) __attribute__((noreturn));

/* sys_power calls this right before Standby entry. */
void faultGuardMarkStandbyEntry(void);
