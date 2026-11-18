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
#include "host_protocol.h"
#include "image_header.h"
#include "boot_shared.h"
#include "fault_guard.h"
#include "sys_power.h"
#include "exti_latch.h"
#include "lane_chain.h"
#include "device_id.h"
#include "rng.h"
#include "indicator_led.h"
#include "button_handler.h"
#include "sys_time.h"
#include "usb_cdc.h"
#include "nrf_link.h"
#include "settings_store.h"
#include "log_line.h"
#include "host_manager.h"
#include "lane_manager.h"
#include "sequence_manager.h"
#include "panel_manager.h"

#include <stdbool.h>
#include <stdint.h>


/* Release number + git revision (cmake/version.cmake), so a .bin or a SWD
 * read of the chip names its build. NODE_LIST reports the numbers; the string
 * is kept alive through the volatile pointer below, or --gc-sections would
 * drop it. */
__attribute__((used)) static const char ctrlFwVersion[] = CTRL_FW_VERSION_STRING;
static const char *volatile ctrlFwVersionRef;

/* The image header at +0x400 (image_header.h). size and imageCrc are filled
 * after the link by tools/image_stamp.py; the bootloader checks them. */
__attribute__((section(".image_header"), used))
const imageHeader_t ctrlImageHeader = {
    .magic         = IMAGE_HEADER_MAGIC,
    .boardType     = (uint8_t)HP_BOARD_CONTROL,
    .fwMajor       = (uint8_t)CTRL_FW_VERSION_MAJOR,
    .fwMinor       = (uint8_t)CTRL_FW_VERSION_MINOR,
    .fwPatch       = (uint8_t)CTRL_FW_VERSION_PATCH,
    .gitRev        = (uint32_t)CTRL_FW_GIT_REV,
    .size          = 0U,
    .imageCrc      = 0U,
    .headerVersion = IMAGE_HEADER_VERSION,
};

/* Production values of ctrl_behavior.h. Warnings, so a release image still
 * builds for the bench. Add a check per new switch. */
#ifdef RELEASE
    #if CTRL_FW_TREE_DIRTY
    #warning "release build from uncommitted changes (or outside git) - its revision cannot be rebuilt"
    #endif
    #if CTRL_ENABLE_INIT_DONE_FLASH
    #warning "release build flashes the panel LEDs at boot - see ctrl_behavior.h"
    #endif
    #if CTRL_ENABLE_DBG_LOG
    #warning "release build sends DBG_LOG lines - see ctrl_behavior.h"
    #endif
    #if CTRL_ENABLE_NRF_LINK_ECHO
    #warning "release build ECHOES BLE data instead of running the BLE session - see ctrl_behavior.h"
    #endif
    #if CTRL_ENABLE_FAULT_INJECT
    #warning "release build reboots on a held BTN1/BTN3 (fault injection) - see ctrl_behavior.h"
    #endif
    #if CTRL_ENABLE_LANE_AUTO_POWER
    #warning "release build powers the lanes at boot - see ctrl_behavior.h"
    #endif
    #if CTRL_DISABLE_WATCHDOG
    #warning "release build runs WITHOUT the watchdog - see ctrl_behavior.h"
    #endif
    #if CTRL_DISABLE_IDLE_WFI
    #warning "release build spins instead of WFI - see ctrl_behavior.h"
    #endif
#endif


#if CTRL_ENABLE_INIT_DONE_FLASH
/* Non-blocking: ctrlRun() plays it out. The link pattern takes over after. */
static const uint32_t ctrlInitFlashPeriodMs   = 120U;
static const uint16_t ctrlInitFlashOnPermille = 500U;
static const uint16_t ctrlInitFlashCount      = 4U;
#endif

#define CTRL_MASTER_FAULT_HOLDOFF_MS  1000U


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
static uint32_t     ctrlUsbFaults;
static uint32_t     ctrlExtFaults;
static uint32_t     ctrlFaultLastMs[2];
static bool         ctrlFaultSeen[2];


/*
 * The single error policy point. Debug: record and (if armed) trap. Release:
 * record and keep running degraded; the host sees what failed through its
 * responses and STATUS (lastError, errorCount). fault_guard reboots only for
 * what cannot run on: a HardFault, an unexpected IRQ, a hung loop (IWDG).
 * docs/ERROR_HANDLING.md.
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

uint32_t ctrlMasterGetUsbFaults(void)
{
    return ctrlUsbFaults;
}

uint32_t ctrlMasterImageCrc(void)
{
    /* Read through volatile: the C initialiser says 0, the stamped flash does not. */
    const volatile imageHeader_t *h = &ctrlImageHeader;
    return (h->magic == IMAGE_HEADER_MAGIC && h->size != 0U) ? h->imageCrc : 0U;
}

/* USB_FAULT: the TPD4S201 cut VBUS; if we still run, it has cleared.
 * EXT_PWR_FAULT: nothing is connected to J302 yet; reported all the same so a
 * floating line shows up on the bench (SYSTEM_ARCHITECTURE §12.2 makes it the
 * first reflex once the power board exists). */
static void ctrlFaultInput(uint8_t idx, uint32_t edges, uint8_t source, ctrlStatus_e status)
{
    if (edges == 0U) {
        return;
    }
    const uint32_t now = HAL_GetTick();
    if (ctrlFaultSeen[idx] && now - ctrlFaultLastMs[idx] < CTRL_MASTER_FAULT_HOLDOFF_MS) {
        return;
    }
    ctrlFaultSeen[idx]   = true;
    ctrlFaultLastMs[idx] = now;

    const hpEvtFault_t evt = { .source = source, .status = (int16_t)status, .sysUs = sysTimeNowUs() };
    ctrlErrorHandler(hostManagerPublishEvent(HP_MSG_EVT_FAULT, &evt, sizeof(evt)));

    logLine_t l;
    logLineInit(&l, "FAULT INPUT");
    logLineKv(&l, "source", source);
    logLineKv(&l, "edges", (int32_t)edges);
    hostManagerLog(HP_LOG_WARN, &l);
}

static void ctrlFaultInputs(void)
{
    const uint32_t usb = extiLatchTake(EXTI_LATCH_USB_FAULT);
    const uint32_t ext = extiLatchTake(EXTI_LATCH_EXT_PWR_FAULT);
    ctrlUsbFaults += usb;
    ctrlExtFaults += ext;
    ctrlFaultInput(0U, usb, (uint8_t)HP_FAULT_SRC_USB, CTRL_ERR_USB_FAULT);
    ctrlFaultInput(1U, ext, (uint8_t)HP_FAULT_SRC_EXT_POWER, CTRL_ERR_POWER_FAULT);
}

void ctrlInit(void)
{
    ctrlFwVersionRef = ctrlFwVersion;

    /* Before anything can change them: reset cause and breadcrumb. */
    faultGuardInit();

    /* Safety first: lane bus pins parked, rail off. */
    ctrlErrorHandler(laneChainInit());

    ctrlErrorHandler(deviceIdInit());
    faultGuardBootInfo_t boot;
    faultGuardGetBootInfo(&boot);
    if (boot.faultReboot) {
        deviceIdOverrideResetCause((uint8_t)HP_RESET_FAULT);
    }
    uint32_t entropy = 0U;
    ctrlErrorHandler(rngInit());
    ctrlErrorHandler(rngGet(&entropy));
    deviceIdMixEntropy(entropy);

    ctrlErrorHandler(indicatorLedInit());
    buttonHandlerInit();
    ctrlErrorHandler(sysTimeInit());

    /* Settings before their owners read them; a corrupt store is reported
     * once, as the last error, after the event log exists. */
    const ctrlStatus_e settings = settingsStoreInit();

    /* The event log must exist before anything publishes; EVT_BOOT goes first. */
    ctrlErrorHandler(hostManagerInit());
    ctrlErrorHandler(settings);
    ctrlErrorHandler(laneManagerInit());
    ctrlErrorHandler(sequenceManagerInit());
    ctrlErrorHandler(nrfLinkInit());
    ctrlErrorHandler(usbCdcInit());

    /* Reported, never fatal: a timing box that refuses to boot helps nobody. */
    ctrlErrorHandler(faultGuardCheckOptionBytes());

    if (faultGuardIsParked()) {
        ctrlErrorHandler(CTRL_ERR_INVALID_STATE);
#ifdef CTRL_APP_WITH_BOOTLOADER
        /* Too many fault reboots in a row: let the bootloader wait for an update. */
        bootShared.request = BOOT_SHARED_ENTER_BOOT;
        NVIC_SystemReset();
#endif
        /* Standalone: run on without the watchdog, reachable over USB. */
    } else {
#if !CTRL_DISABLE_WATCHDOG
        ctrlErrorHandler(faultGuardStartWatchdog());
#endif
    }

#if CTRL_ENABLE_LANE_AUTO_POWER
    ctrlErrorHandler(laneManagerSetPower(true));
#endif

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

    faultGuardTick();
    buttonHandlerPoll(loopStartMs);
    usbCdcRun();
    nrfLinkRun();

    /* Lanes first, host second: a crossing read in this pass is pumped in it. */
    ctrlErrorHandler(laneManagerRun());
    ctrlErrorHandler(sequenceManagerRun());
    ctrlErrorHandler(hostManagerRun());
    ctrlErrorHandler(panelManagerRun());
    ctrlErrorHandler(indicatorLedRun(HAL_GetTick()));

    ctrlFaultInputs();
    /* Never write flash while a start sequence runs. */
    ctrlErrorHandler(settingsStoreRun(!sequenceManagerIsRunning()));

    const uint32_t loopMs = HAL_GetTick() - loopStartMs;
    if (loopMs > ctrlLoopMaxMs) {
        ctrlLoopMaxMs = loopMs;
    }

    sysPowerIdle();
}
