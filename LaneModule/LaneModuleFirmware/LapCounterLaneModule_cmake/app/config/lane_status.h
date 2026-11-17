/*
 * Project-wide status / error codes for the lane module firmware.
 *
 * Every function that can fail returns laneStatus_e. LANE_OK is 0 and every
 * error is negative, so `status < 0` reliably means failure. Codes are grouped
 * in tens per subsystem with intentional gaps, so a code can be added inside a
 * group without renumbering. Numeric values are stable: they go over the bus
 * in STATUS (last error), so removing an unused code leaves its number free
 * rather than shifting the others, and a value never silently changes meaning
 * between firmware versions.
 *
 * Picking a guard:
 *   LANE_ASSERT(status)        - record, and trap if armed, then continue.
 *                                Observe a failure without bailing out.
 *   LANE_ASSERT_RETURN(status) - record, trap if armed, and return the code.
 *                                Use at the originating site (e.g. the failing
 *                                HAL call) to catch it before it propagates.
 *   LANE_RETURN_IF_ERR(status) - no record, no trap, just propagate. Use in
 *                                intermediate layers that already asserted at
 *                                the source, and on genuinely transient
 *                                conditions the caller retries by design.
 *
 * With LANE_ENABLE_ASSERTS off the trap macros compile away to nothing;
 * LANE_RETURN_IF_ERR stays active in every build.
 */

#pragma once

#include "stm32_hal.h"
#include "lane_behavior.h"

#ifdef RELEASE
    #define LANE_ENABLE_ASSERTS  0
#else
    #define LANE_ENABLE_ASSERTS  1
#endif

typedef enum {
    LANE_OK                    =   0,   /* Success — the only non-negative code */

    /* Generic (-1 .. -9): apply to any module; prefer a specific code when one exists. */
    LANE_ERR_NOT_INITIALIZED   =  -2,   /* Module used before its init ran */
    LANE_ERR_INVALID_STATE     =  -3,   /* Operation not valid in the current state */
    LANE_ERR_HAL               =  -5,   /* HAL call failed with no more specific code to report */
    LANE_ERR_INVALID_ARG       =  -6,   /* NULL pointer, out-of-range, or otherwise bad parameter */

    /* Hardware / peripheral (-10 .. -19) */
    LANE_ERR_TIMER             = -10,   /* TIM1/TIM2 refused a start, stop or channel switch */

    /* Sensor I2C bus, VCNL4040 (-20 .. -29) */
    LANE_ERR_I2C               = -20,   /* Transfer failed (NACK or bus error) */
    LANE_ERR_I2C_TIMEOUT       = -21,   /* Transfer did not complete within the timeout */
    LANE_ERR_I2C_BUSY          = -22,   /* Peripheral was not idle when the transfer was requested */

    /* Timebase: SYNC mapping and TIM2 alarms (-30 .. -39) */
    LANE_ERR_SYNC_NO_LOCK      = -30,   /* No SYNC period measured yet; mainboard time unknown */
    LANE_ERR_SYNC_UNALIGNED    = -31,   /* SYNC locked, but the mainboard has not named a tick yet */
    LANE_ERR_ALARM_BUSY        = -32,   /* TIM2 alarm already armed; cancel it first */
    LANE_ERR_ALARM_RANGE       = -33,   /* Alarm target too far from now to be unambiguous */

    /* I3C target / register map (-40 .. -49): step 6 of the bring-up order. */

    /* CHAIN enumeration (-50 .. -59): step 7 of the bring-up order. */
} laneStatus_e;


/* =========================================================================
 *                           DEBUG TRAP MACROS
 *
 * In debug builds these record the failure and, if trapping is armed, hit a
 * software breakpoint (__BKPT) at the exact source line where it was detected.
 * In release builds they compile away to a bare return.
 *
 * Trapping is DISARMED by default, for the same reason as on the hub: __BKPT
 * halts the core whenever halting debug is enabled, which stays enabled after
 * the probe is unplugged until the next power-on reset. A lane that then
 * asserts stops answering the bus and drops out of the chain. Arm it from the
 * debugger (laneAssertTrapArmed in Live Watch) when sitting at the bench.
 * ========================================================================= */

#if LANE_ENABLE_ASSERTS

    /* Live: flip from the debugger to arm __BKPT. See the note above. */
    extern volatile uint8_t      laneAssertTrapArmed;

    /* Last non-OK status handed to either macro, and how many there have been. */
    extern volatile laneStatus_e laneAssertLastStatus;
    extern volatile uint32_t     laneAssertCount;

    void laneAssertRecord(laneStatus_e status);

    #define LANE_ASSERT(status) \
        do { \
            laneStatus_e _s = (status); \
            if (_s != LANE_OK) { \
                laneAssertRecord(_s); \
                if (laneAssertTrapArmed != 0U) { \
                    __BKPT(0); \
                } \
            } \
        } while (0)

    #define LANE_ASSERT_RETURN(status) \
        do { \
            laneStatus_e _s = (status); \
            if (_s != LANE_OK) { \
                laneAssertRecord(_s); \
                if (laneAssertTrapArmed != 0U) { \
                    __BKPT(0); \
                } \
                return _s; \
            } \
        } while (0)

#else

    #define LANE_ASSERT(status)          do { (void)(status); } while (0)

    #define LANE_ASSERT_RETURN(status) \
        do { \
            laneStatus_e _s = (status); \
            if (_s != LANE_OK) { \
                return _s; \
            } \
        } while (0)

#endif /* LANE_ENABLE_ASSERTS */


/* Propagate an error upward without trapping. Identical in debug and release. */
#define LANE_RETURN_IF_ERR(status) \
    do { \
        laneStatus_e _s = (status); \
        if (_s != LANE_OK) { \
            return _s; \
        } \
    } while (0)


/* =========================================================================
 *                       HELPER: HAL STATUS MAPPING
 *
 * The C5 modular HAL defines HAL_OK as 0xEAEAEAEA (not 0), so success must be
 * tested against HAL_OK rather than against zero.
 * ========================================================================= */

static inline laneStatus_e laneStatusFromHal(hal_status_t halStatus)
{
    return (halStatus == HAL_OK) ? LANE_OK : LANE_ERR_HAL;
}

static inline laneStatus_e laneStatusFromHalOr(hal_status_t halStatus, laneStatus_e onFail)
{
    return (halStatus == HAL_OK) ? LANE_OK : onFail;
}

static inline laneStatus_e laneStatusFromHalI2c(hal_status_t halStatus)
{
    switch (halStatus) {
    case HAL_OK:
        return LANE_OK;
    case HAL_TIMEOUT:
        return LANE_ERR_I2C_TIMEOUT;
    case HAL_BUSY:
        return LANE_ERR_I2C_BUSY;
    default:
        return LANE_ERR_I2C;
    }
}
