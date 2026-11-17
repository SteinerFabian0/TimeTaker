/*
 * Status / error codes for the control module firmware.
 *
 * Every function that can fail returns ctrlStatus_e. The enum itself is
 * GENERATED into host_protocol.h from protocol/host_protocol.toml, because the
 * same codes travel to the web app in every response: one numbering for the
 * firmware and the protocol, so the app can show the name of what failed.
 * CTRL_OK is 0, every error is negative, grouped in tens with gaps, and values
 * are stable across versions. Add a code in the TOML, never here.
 *
 * Picking a guard (same semantics as the hub's hub_status.h):
 *   CTRL_ASSERT(status)        - record, and trap if armed, then continue.
 *   CTRL_ASSERT_RETURN(status) - record, trap if armed, and return the code. Use
 *                                at the originating site (the failing HAL call).
 *   CTRL_RETURN_IF_ERR(status) - no record, no trap, just propagate. Use in
 *                                intermediate layers that already asserted at
 *                                the source, and on transient conditions the
 *                                caller retries by design.
 *
 * With CTRL_ENABLE_ASSERTS off the trap macros compile away to nothing;
 * CTRL_RETURN_IF_ERR stays active in every build.
 */

#pragma once

#include "stm32_hal.h"
#include "ctrl_behavior.h"
#include "host_protocol.h"

#ifdef RELEASE
    #define CTRL_ENABLE_ASSERTS  0
#else
    #define CTRL_ENABLE_ASSERTS  1
#endif


/* =========================================================================
 *                           DEBUG TRAP MACROS
 *
 * In debug builds these record the failure and, if trapping is armed, hit a
 * software breakpoint (__BKPT) at the exact source line where it was detected.
 * In release builds they compile away to a bare return.
 *
 * Trapping is DISARMED by default, for the same reason as on the hub: __BKPT
 * halts the core whenever halting debug is enabled, which stays enabled after
 * the probe is unplugged until the next power-on reset. A control module that
 * then asserts stops the whole timing system. Arm it from the debugger
 * (ctrlAssertTrapArmed in Live Watch) when sitting at the bench.
 * ========================================================================= */

#if CTRL_ENABLE_ASSERTS

    /* Live: flip from the debugger to arm __BKPT. See the note above. */
    extern volatile uint8_t      ctrlAssertTrapArmed;

    /* Last non-OK status handed to either macro, and how many there have been. */
    extern volatile ctrlStatus_e ctrlAssertLastStatus;
    extern volatile uint32_t     ctrlAssertCount;

    void ctrlAssertRecord(ctrlStatus_e status);

    #define CTRL_ASSERT(status) \
        do { \
            ctrlStatus_e _s = (status); \
            if (_s != CTRL_OK) { \
                ctrlAssertRecord(_s); \
                if (ctrlAssertTrapArmed != 0U) { \
                    __BKPT(0); \
                } \
            } \
        } while (0)

    #define CTRL_ASSERT_RETURN(status) \
        do { \
            ctrlStatus_e _s = (status); \
            if (_s != CTRL_OK) { \
                ctrlAssertRecord(_s); \
                if (ctrlAssertTrapArmed != 0U) { \
                    __BKPT(0); \
                } \
                return _s; \
            } \
        } while (0)

#else

    #define CTRL_ASSERT(status)          do { (void)(status); } while (0)

    #define CTRL_ASSERT_RETURN(status) \
        do { \
            ctrlStatus_e _s = (status); \
            if (_s != CTRL_OK) { \
                return _s; \
            } \
        } while (0)

#endif /* CTRL_ENABLE_ASSERTS */


/* Propagate an error upward without trapping. Identical in debug and release. */
#define CTRL_RETURN_IF_ERR(status) \
    do { \
        ctrlStatus_e _s = (status); \
        if (_s != CTRL_OK) { \
            return _s; \
        } \
    } while (0)


/* =========================================================================
 *                       HELPER: HAL STATUS MAPPING
 *
 * The C5 modular HAL defines HAL_OK as 0xEAEAEAEA (not 0), so success must be
 * tested against HAL_OK rather than against zero.
 * ========================================================================= */

static inline ctrlStatus_e ctrlStatusFromHal(hal_status_t halStatus)
{
    return (halStatus == HAL_OK) ? CTRL_OK : CTRL_ERR_HAL;
}

static inline ctrlStatus_e ctrlStatusFromHalOr(hal_status_t halStatus, ctrlStatus_e onFail)
{
    return (halStatus == HAL_OK) ? CTRL_OK : onFail;
}
