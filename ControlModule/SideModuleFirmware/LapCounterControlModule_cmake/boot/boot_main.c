/*
 * ctrl_boot: the control module's bootloader (CONTROL_FIRMWARE_GUIDE §5.7,
 * SYSTEM_ARCHITECTURE §10.1). 32 KB at 0x0800_0000; the app lives at
 * 0x0800_8000 with its header at +0x400 (image_header.h).
 *
 * Boot decision, at every reset:
 *   1. bootShared.jump == JUMP_APP (set by this bootloader just before a
 *      reset): jump to the app at once, before any peripheral is touched.
 *   2. Otherwise bring up clocks, GPIO, USB and USART1 (mx_system_init) and
 *      stay if ANY of: bootShared.request == ENTER_BOOT (REBOOT{BOOTLOADER}
 *      from the app), the app image fails imageCheck(), BOOT_REQ (PB4, from
 *      the nRF) is low, BTN1 (PC15) is held. Else set JUMP_APP and reset, so
 *      the app starts from a clean reset state.
 *
 * Staying: the same host protocol on both transports. USB CDC with
 * HP_USB_PID_BOOT and HP_USB_PRODUCT_BOOT; HELLO reports boardType
 * CONTROL_BOOT; BLE through nrf_link, where a client
 * must be on the stored bind list (read-only from the settings pages) or the
 * session stays UNBOUND. Served: HELLO/PING/RESUME (host_session), NODE_LIST
 * (CTRL node, boardType CONTROL_BOOT), UPDATE_BEGIN/CHUNK/END/ABORT
 * (update_writer), BOOT_APP, REBOOT. Everything else ERR_UNSUPPORTED.
 *
 * An interrupted update leaves the header unwritten: this bootloader stays
 * and waits. The device cannot brick (SW604 + SW605 -> ROM DFU is the last
 * resort anyway).
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "main.h"
#include "stm32_hal.h"
#include "mx_gpio_default.h"
#include "mx_cortex_mpu.h"
#include "mx_cortex_nvic.h"
#include "mx_icache.h"
#include "mx_rcc.h"
#include "mx_usart1.h"
#include "mx_usb_drd_fs.h"
#include "ctrl_status.h"
#include "ctrl_fw_version.h"
#include "host_protocol.h"
#include "ble_link_protocol.h"
#include "boot_shared.h"
#include "image_header.h"
#include "update_writer.h"
#include "host_session.h"
#include "event_log.h"
#include "usb_cdc.h"
#include "nrf_link.h"
#include "device_id.h"
#include "flash_drv.h"
#include "bind_list.h"
#include "settings_record.h"

#define BOOT_RAM_BASE        0x20000000U
#define BOOT_RAM_SIZE        0x10000U
#define BOOT_LOG_SLOTS       8U
#define BOOT_CHUNK_MAX       224U    /* multiple of 16, fits HP_UPDATE_CHUNK_REQ_MAX_ITEMS */
#define BOOT_REBOOT_DELAY_MS 100U

/* Settings pages (settings_store.h): the bind list is read, never written. */
#define BOOT_SETTINGS_PAGE0  0x0803C000U
#define BOOT_SETTINGS_PAGE1  0x0803E000U
#define BOOT_SETTINGS_BYTES  0x2000U
#define BOOT_BINDS_OFFSET    (28U + 6U * 36U + 6U + 2U)   /* settingsData_t.binds */

bootShared_t bootShared __attribute__((section(".boot_shared")));

static eventLogSlot_t bootLogSlots[BOOT_LOG_SLOTS];
static eventLog_t     bootLog;
static hostSession_t  bootUsb;
static hostSession_t  bootBle;
static updateWriter_t bootWriter;
static bindList_t     bootBinds;
static bool           bootRebootPending;
static uint32_t       bootRebootAtMs;

static const uint8_t *bootApp(void)
{
    return (const uint8_t *)IMAGE_APP_BASE;
}

static bool bootAppValid(void)
{
    return imageCheck(bootApp(), IMAGE_APP_SLOT_SIZE, HP_BOARD_CONTROL, IMAGE_APP_BASE,
                      BOOT_RAM_BASE, BOOT_RAM_SIZE) == CTRL_OK;
}

static void bootJumpToApp(void) __attribute__((noreturn));
static void bootJumpToApp(void)
{
    const uint32_t *vec = (const uint32_t *)IMAGE_APP_BASE;
    __disable_irq();
    SysTick->CTRL = 0U;
    SCB->VTOR = IMAGE_APP_BASE;
    __set_MSPLIM(0U);
    __set_MSP(vec[0]);
    __DSB();
    __ISB();
    __enable_irq();
    ((void (*)(void))vec[1])();
    while (1) { }
}

/* --- Flash I/O for the update writer -------------------------------------- */

static ctrlStatus_e bootErase(uint32_t offset, uint32_t len)
{
    for (uint32_t a = offset / IMAGE_PAGE_SIZE * IMAGE_PAGE_SIZE; a < offset + len; a += IMAGE_PAGE_SIZE) {
        IWDG->KR = 0xAAAAU;                 /* if the app started the IWDG, it still runs */
        CTRL_RETURN_IF_ERR(flashDrvErasePage(IMAGE_APP_BASE + a));
    }
    return CTRL_OK;
}

static ctrlStatus_e bootProgram16(uint32_t offset, const uint8_t data[UPDATE_WRITER_QW])
{
    return flashDrvProgram16(IMAGE_APP_BASE + offset, data);
}

static const updateFlashIo_t bootFlashIo = {
    .erase     = bootErase,
    .program16 = bootProgram16,
    .slot      = (const uint8_t *)IMAGE_APP_BASE,
    .slotSize  = IMAGE_APP_SLOT_SIZE,
    .boardType = HP_BOARD_CONTROL,
    .imageAddr = IMAGE_APP_BASE,
    .ramBase   = BOOT_RAM_BASE,
    .ramSize   = BOOT_RAM_SIZE,
};

/* --- Sessions ---------------------------------------------------------------- */

static uint64_t bootNowUs(void)
{
    return (uint64_t)HAL_GetTick() * 1000U;
}

static const hostTransport_t bootUsbIo = { usbCdcRead, usbCdcWriteSpace, usbCdcWrite, usbCdcFlush };
static const hostTransport_t bootBleIo = { nrfLinkRead, nrfLinkWriteSpace, nrfLinkWrite, nrfLinkFlush };

static uint8_t bootBleHello(hostSession_t *self, const hpHelloReq_t *hello)
{
    (void)self;
    return (bindListFind(&bootBinds, hello->clientId) >= 0) ? (uint8_t)HP_BIND_BOUND : (uint8_t)HP_BIND_UNBOUND;
}

static void bootLoadBinds(void)
{
    settingsSelect_t sel;
    bindListInit(&bootBinds);
    settingsRecordSelect((const uint8_t *)BOOT_SETTINGS_PAGE0, (const uint8_t *)BOOT_SETTINGS_PAGE1,
                         BOOT_SETTINGS_BYTES, &sel);
    if (sel.active < 0) {
        return;
    }
    settingsRecordHeader_t hdr;
    const uint8_t *payload = NULL;
    const uint32_t page = (sel.active == 0) ? BOOT_SETTINGS_PAGE0 : BOOT_SETTINGS_PAGE1;
    if (settingsRecordCheck((const uint8_t *)page, BOOT_SETTINGS_BYTES, &hdr, &payload) == SETTINGS_PAGE_VALID
        && hdr.length >= BOOT_BINDS_OFFSET + BIND_LIST_SERIAL_BYTES) {
        (void)bindListDeserialize(&bootBinds, payload + BOOT_BINDS_OFFSET, BIND_LIST_SERIAL_BYTES);
    }
}

static ctrlStatus_e bootOnRequest(hostSession_t *session, uint8_t msgId, const uint8_t *req, uint16_t reqLen,
                                  uint8_t *rsp, uint16_t rspCap, uint16_t *rspLen)
{
    (void)session;
    (void)rspCap;
    switch (msgId) {
    case HP_MSG_NODE_LIST: {
        const hpNodeInfo_t n = {
            .nodeClass = HP_NODE_CTRL, .boardType = HP_BOARD_CONTROL_BOOT, .state = HP_NODE_STATE_UPDATING,
            .fwMajor = CTRL_FW_VERSION_MAJOR, .fwMinor = CTRL_FW_VERSION_MINOR, .fwPatch = CTRL_FW_VERSION_PATCH,
            .protoMinor = HP_PROTO_MINOR, .fwGitRev = CTRL_FW_GIT_REV,
        };
        memcpy(rsp + HP_NODE_LIST_RSP_BYTES, &n, sizeof(n));
        *rspLen = HP_NODE_LIST_RSP_BYTES + HP_NODE_LIST_RSP_ITEM_BYTES;
        return CTRL_OK;
    }
    case HP_MSG_UPDATE_BEGIN: {
        if (reqLen != HP_UPDATE_BEGIN_REQ_BYTES) {
            return CTRL_ERR_BAD_LENGTH;
        }
        hpUpdateBeginReq_t r;
        memcpy(&r, req, sizeof(r));
        CTRL_RETURN_IF_ERR(updateWriterBegin(&bootWriter, r.boardType, r.size, r.crc32));
        ((hpUpdateBeginRsp_t *)rsp)->chunkMax = BOOT_CHUNK_MAX;
        *rspLen = HP_UPDATE_BEGIN_RSP_BYTES;
        return CTRL_OK;
    }
    case HP_MSG_UPDATE_CHUNK: {
        if (reqLen < HP_UPDATE_CHUNK_REQ_BYTES || reqLen - HP_UPDATE_CHUNK_REQ_BYTES > BOOT_CHUNK_MAX) {
            return CTRL_ERR_BAD_LENGTH;
        }
        uint32_t offset;
        uint32_t next = 0U;
        memcpy(&offset, req, sizeof(offset));
        CTRL_RETURN_IF_ERR(updateWriterChunk(&bootWriter, offset, req + HP_UPDATE_CHUNK_REQ_BYTES,
                                             (uint16_t)(reqLen - HP_UPDATE_CHUNK_REQ_BYTES), &next));
        ((hpUpdateChunkRsp_t *)rsp)->nextOffset = next;
        *rspLen = HP_UPDATE_CHUNK_RSP_BYTES;
        return CTRL_OK;
    }
    case HP_MSG_UPDATE_END:
        CTRL_RETURN_IF_ERR(updateWriterEnd(&bootWriter));
        *rspLen = HP_UPDATE_END_RSP_BYTES;
        return CTRL_OK;
    case HP_MSG_UPDATE_ABORT:
        updateWriterAbort(&bootWriter);
        *rspLen = HP_UPDATE_ABORT_RSP_BYTES;
        return CTRL_OK;
    case HP_MSG_BOOT_APP:
    case HP_MSG_REBOOT:
        if (msgId == HP_MSG_REBOOT && reqLen == HP_REBOOT_REQ_BYTES && req[0] == HP_REBOOT_BOOTLOADER) {
            bootShared.request = BOOT_SHARED_ENTER_BOOT;
        } else if (!bootAppValid()) {
            return CTRL_ERR_INVALID_STATE;   /* no valid app to start */
        } else {
            bootShared.jump = BOOT_SHARED_JUMP_APP;
        }
        bootRebootPending = true;
        bootRebootAtMs    = HAL_GetTick() + BOOT_REBOOT_DELAY_MS;
        *rspLen = 2U;
        return CTRL_OK;
    default:
        return CTRL_ERR_UNSUPPORTED;
    }
}

/* mx_system_init() without the timers, SPI and debug setup the bootloader
 * never uses: three timer inits pull in about 8 KB of HAL TIM. Same order. */
static bool bootSystemInit(void)
{
    return mx_cortex_mpu_init() == SYSTEM_OK && HAL_Init() == HAL_OK && mx_cortex_nvic_init() == SYSTEM_OK
        && mx_icache_init() != NULL && HAL_ICACHE_Start(mx_icache_gethandle(), HAL_ICACHE_IT_NONE) == HAL_OK
        && mx_rcc_init() == SYSTEM_OK && mx_rcc_peripherals_clock_config() == SYSTEM_OK
        && mx_usart1_uart_init() != NULL && mx_usb_drd_fs_device_init() != NULL
        && mx_gpio_default_init() == SYSTEM_OK;
}

int main(void)
{
    if (bootShared.jump == BOOT_SHARED_JUMP_APP) {
        bootShared.jump = 0U;
        if (bootAppValid()) {
            bootJumpToApp();
        }
    }

    if (!bootSystemInit()) {
        NVIC_SystemReset();
    }

    /* PB4/PC15 were analog until mx_gpio_default_init(): give the internal
     * pull-ups time to lift the lines, or a still-low pin reads as a strap
     * and every boot would stop in the bootloader. */
    HAL_Delay(2U);
    const bool requested = bootShared.request == BOOT_SHARED_ENTER_BOOT;
    bootShared.request = 0U;
    const bool strap = HAL_GPIO_ReadPin(BLE_GPIO1_PORT, BLE_GPIO1_PIN) == HAL_GPIO_PIN_RESET;   /* BOOT_REQ */
    const bool btn1  = HAL_GPIO_ReadPin(BTN1_PORT, BTN1_PIN) == HAL_GPIO_PIN_RESET;
    if (!requested && !strap && !btn1 && bootAppValid()) {
        bootShared.jump = BOOT_SHARED_JUMP_APP;
        NVIC_SystemReset();
    }

    (void)deviceIdInit();
    bootLoadBinds();
    updateWriterInit(&bootWriter, &bootFlashIo);
    eventLogInit(&bootLog, bootLogSlots, BOOT_LOG_SLOTS);

    hostSessionConfig_t cfg = {
        .transport = HP_TRANSPORT_USB, .bindState = HP_BIND_TRUSTED, .boardType = HP_BOARD_CONTROL_BOOT,
        .bootId = deviceIdBootId(),
        .log = &bootLog, .nowUs = bootNowUs, .onRequest = bootOnRequest,
    };
    hostSessionInit(&bootUsb, &bootUsbIo, &cfg);
    cfg.transport = HP_TRANSPORT_BLE;
    cfg.bindState = HP_BIND_UNBOUND;
    cfg.onHello   = bootBleHello;
    hostSessionInit(&bootBle, &bootBleIo, &cfg);

    const hpEvtBoot_t boot = { .resetCause = deviceIdResetCause(), .bootId = deviceIdBootId() };
    (void)eventLogAppend(&bootLog, HP_MSG_EVT_BOOT, &boot, sizeof(boot), NULL);

    (void)usbCdcInit();
    (void)nrfLinkInit();

    while (1) {
        IWDG->KR = 0xAAAAU;
        usbCdcRun();
        nrfLinkRun();
        if (usbCdcTakeLinkReset()) {
            hostSessionReset(&bootUsb);
        }
        if (nrfLinkTakeLinkReset()) {
            hostSessionReset(&bootBle);
        }
        nrfLinkEvent_t ev;
        while (nrfLinkTakeEvent(&ev)) { }   /* no bind policy here: list read-only */
        hostSessionRun(&bootUsb);
        hostSessionRun(&bootBle);
        if (bootRebootPending && (int32_t)(HAL_GetTick() - bootRebootAtMs) >= 0) {
            NVIC_SystemReset();
        }
    }
}
