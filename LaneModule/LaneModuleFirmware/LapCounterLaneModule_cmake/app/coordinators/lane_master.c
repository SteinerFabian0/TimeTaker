/*
 * lane_master.c
 *
 * @brief  Top-level coordinator for the lane module firmware.
 *
 */

#include "lane_master.h"
#include "main.h"
#include "lane_status.h"
#include "lane_behavior.h"
#include "lane_fw_version.h"

#include "stm32_hal.h"
#include "indicator_led.h"
#include "lane_timer.h"

#include <stdbool.h>
#include <stdint.h>


/* Release number + git revision (cmake/version.cmake), so a .bin or a SWD
 * read of the chip names its build. `used` only stops the compiler: until the
 * ID register reads the string, --gc-sections would still drop it, so
 * laneInit() stores its address through the volatile pointer below. */
__attribute__((used)) static const char laneFwVersion[] = LANE_FW_VERSION_STRING;
static const char *volatile laneFwVersionRef;

/* Production values of lane_behavior.h. Warnings, so a release image still
 * builds for the bench. Add a check per new switch. */
#ifdef RELEASE
    #if LANE_FW_TREE_DIRTY
    #warning "release build from uncommitted changes - its revision cannot be rebuilt"
    #endif
    #if LANE_DEBUG_DURING_LOW_POWER
    #warning "release build keeps SWD through Sleep/Stop1 - see lane_behavior.h"
    #endif
    #if LANE_ENABLE_INIT_DONE_FLASH
    #warning "release build flashes the LED at boot - see lane_behavior.h"
    #endif
#endif


#if LANE_ENABLE_INIT_DONE_FLASH
/* Non-blocking: laneRun() plays it out while the rest of the lane starts. */
static const uint32_t laneInitFlashPeriodMs  = 160U;
static const uint16_t laneInitFlashOnPermille = 500U;
static const uint16_t laneInitFlashCount      = 4U;
#endif


#if LANE_ENABLE_ASSERTS
volatile uint8_t      laneAssertTrapArmed  = 0U;
volatile laneStatus_e laneAssertLastStatus = LANE_OK;
volatile uint32_t     laneAssertCount      = 0U;

void laneAssertRecord(laneStatus_e status)
{
    laneAssertLastStatus = status;
    laneAssertCount++;
}
#endif

static laneStatus_e laneLastError = LANE_OK;
static uint32_t     laneErrorCount;


/*
 * The single error policy point. Debug: record and (if armed) trap. Release:
 * record for STATUS and keep running degraded; the lane has a channel back,
 * so the mainboard learns what failed instead of watching a reboot loop.
 * faultGuardFatal() (bring-up step 10) is for truly unrecoverable states only.
 */
static void laneErrorHandler(laneStatus_e statusCode)
{
    if (statusCode == LANE_OK) {
        return;
    }

    laneLastError = statusCode;
    laneErrorCount++;

    LANE_ASSERT(statusCode);
}

laneStatus_e laneMasterGetLastError(void)
{
    return laneLastError;
}

uint32_t laneMasterGetErrorCount(void)
{
    return laneErrorCount;
}

void laneInit(void)
{
    laneFwVersionRef = laneFwVersion;

    /* LED first: it is the only thing a bench board can show. */
    laneErrorHandler(indicatorLedInit());
    laneErrorHandler(laneTimerInit());

#if LANE_ENABLE_INIT_DONE_FLASH
    laneErrorHandler(indicatorLedBlink(laneInitFlashPeriodMs, laneInitFlashOnPermille,
                                       laneInitFlashCount));
#endif
}

void laneRun(void)
{
    laneErrorHandler(indicatorLedRun(HAL_GetTick()));
}
