/*
 * ctrl_master.c
 *
 * @brief  Top-level coordinator for the TimeTaker control module firmware.
 */

#include "ctrl_master.h"
#include "main.h"
#include "ctrl_status.h"
#include "ctrl_behavior.h"
#include "ctrl_fw_version.h"

#include "stm32_hal.h"
#include "lane_chain.h"
#include "device_id.h"
#include "indicator_led.h"
#include "button_handler.h"
#include "sys_time.h"
#include "usb_cdc.h"
#include "host_manager.h"
#include "lane_manager.h"
#include "panel_manager.h"

#include <stdbool.h>
#include <stdint.h>


/* Release number + git revision (cmake/version.cmake), so a .bin or a SWD
 * read of the chip names its build. NODE_LIST reports the numbers; the string
 * is kept alive through the volatile pointer below, or --gc-sections would
 * drop it. */
__attribute__((used)) static const char ctrlFwVersion[] = CTRL_FW_VERSION_STRING;
static const char *volatile ctrlFwVersionRef;

/* Production values of ctrl_behavior.h. Warnings, so a release image still
 * builds for the bench. Add a check per new switch. */
#ifdef RELEASE
    #if CTRL_FW_TREE_DIRTY
    #warning "release build from uncommitted changes (or outside git) - its revision cannot be rebuilt"
    #endif
    #if CTRL_ENABLE_INIT_DONE_FLASH
    #warning "release build flashes the panel LEDs at boot - see ctrl_behavior.h"
    #endif
#endif


#if CTRL_ENABLE_INIT_DONE_FLASH
/* Non-blocking: ctrlRun() plays it out. The link pattern takes over after. */
static const uint32_t ctrlInitFlashPeriodMs   = 120U;
static const uint16_t ctrlInitFlashOnPermille = 500U;
static const uint16_t ctrlInitFlashCount      = 4U;
#endif


#if CTRL_ENABLE_ASSERTS
volatile uint8_t      ctrlAssertTrapArmed  = 0U;
volatile ctrlStatus_e ctrlAssertLastStatus = CTRL_OK;
volatile uint32_t     ctrlAssertCount      = 0U;

void ctrlAssertRecord(ctrlStatus_e status)
{
    ctrlAssertLastStatus = status;
    ctrlAssertCount++;
}
#endif

static ctrlStatus_e ctrlLastError = CTRL_OK;
static uint32_t     ctrlErrorCount;
static uint32_t     ctrlLoopMaxMs;


/*
 * The single error policy point. Debug: record and (if armed) trap. Release:
 * record and keep running degraded; the host sees what failed through its
 * responses and, later, the STATUS request. A fault_guard reboot path joins
 * when fault_guard is ported (watchdog stage), for truly unrecoverable states
 * only.
 */
static void ctrlErrorHandler(ctrlStatus_e statusCode)
{
    if (statusCode == CTRL_OK) {
        return;
    }

    ctrlLastError = statusCode;
    ctrlErrorCount++;

    CTRL_ASSERT(statusCode);
}

ctrlStatus_e ctrlMasterGetLastError(void)
{
    return ctrlLastError;
}

uint32_t ctrlMasterGetErrorCount(void)
{
    return ctrlErrorCount;
}

uint32_t ctrlMasterGetLoopMaxMs(void)
{
    return ctrlLoopMaxMs;
}

void ctrlInit(void)
{
    ctrlFwVersionRef = ctrlFwVersion;

    /* Safety first: lane bus pins parked, rail off. */
    ctrlErrorHandler(laneChainInit());

    ctrlErrorHandler(deviceIdInit());
    ctrlErrorHandler(indicatorLedInit());
    buttonHandlerInit();
    ctrlErrorHandler(sysTimeInit());

    /* The event log must exist before anything publishes; EVT_BOOT goes first. */
    ctrlErrorHandler(hostManagerInit());
    ctrlErrorHandler(laneManagerInit());
    ctrlErrorHandler(usbCdcInit());

    uint32_t ledsFreeAtMs = HAL_GetTick();
#if CTRL_ENABLE_INIT_DONE_FLASH
    ctrlErrorHandler(indicatorLedBlink(INDICATOR_LED_STATUS, ctrlInitFlashPeriodMs,
                                       ctrlInitFlashOnPermille, ctrlInitFlashCount));
    ctrlErrorHandler(indicatorLedBlink(INDICATOR_LED_ACT, ctrlInitFlashPeriodMs,
                                       ctrlInitFlashOnPermille, ctrlInitFlashCount));
    ledsFreeAtMs += ctrlInitFlashPeriodMs * ctrlInitFlashCount;
#endif
    ctrlErrorHandler(panelManagerInit(ledsFreeAtMs));
}

void ctrlRun(void)
{
    const uint32_t loopStartMs = HAL_GetTick();

    buttonHandlerPoll(loopStartMs);
    usbCdcRun();

    ctrlErrorHandler(hostManagerRun());
    ctrlErrorHandler(laneManagerRun());
    ctrlErrorHandler(panelManagerRun());

    ctrlErrorHandler(indicatorLedRun(HAL_GetTick()));

    const uint32_t loopMs = HAL_GetTick() - loopStartMs;
    if (loopMs > ctrlLoopMaxMs) {
        ctrlLoopMaxMs = loopMs;
    }
}
