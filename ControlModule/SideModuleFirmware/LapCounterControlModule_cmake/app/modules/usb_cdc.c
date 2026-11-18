/*
 * USB CDC-ACM device on TinyUSB. See usb_cdc.h.
 */

#include "usb_cdc.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "stm32_hal.h"
#include "mx_usb_drd_fs.h"
#include "tusb.h"
#include "device_id.h"
#include "ctrl_status.h"

#define USB_CDC_BCD_DEVICE   0x0100U   /* device release 1.00 */
#define USB_CDC_MAX_POWER_MA 500U      /* the lanes run from USB power */

/* Endpoints: notification IN, data OUT, data IN. */
#define USB_CDC_EP_NOTIF     0x81U
#define USB_CDC_EP_OUT       0x02U
#define USB_CDC_EP_IN        0x82U
#define USB_CDC_EP_NOTIF_SZ  8U
#define USB_CDC_EP_DATA_SZ   64U

static volatile bool usbCdcLinkReset;


/* --- Replacement for the excluded generated init (see usb_cdc.h) --------- */

/* mx_system_init() checks the result for NULL only; TinyUSB has no use for a
 * HAL PCD handle, so this one is never initialised or touched. */
static hal_pcd_handle_t usbCdcUnusedPcdHandle;

hal_pcd_handle_t *mx_usb_drd_fs_device_init(void)
{
    /* What the generated function did before HAL_PCD_*, minus the PCD: the
     * bus clock, the 48 MHz kernel clock (PSI/3, enabled by
     * mx_rcc_peripherals_clock_config()) and the NVIC priority (4, see
     * CONTROL_FIRMWARE_NOTES). TinyUSB enables the IRQ itself in tusb_init(). */
    HAL_RCC_USB_EnableClock();
    if (HAL_RCC_CK48_SetKernelClkSource(HAL_RCC_CK48_CLK_SRC_PSIDIV3) != HAL_OK) {
        return NULL;
    }
    HAL_CORTEX_NVIC_SetPriority(USB_DRD_FS_IRQn, HAL_CORTEX_NVIC_PREEMP_PRIORITY_4,
                                HAL_CORTEX_NVIC_SUB_PRIORITY_0);
    return &usbCdcUnusedPcdHandle;
}

void USB_DRD_FS_IRQHandler(void)
{
    tusb_int_handler(0, true);
}

/* TinyUSB's clock (timeouts in the control pipe). */
uint32_t tusb_time_millis_api(void)
{
    return HAL_GetTick();
}


/* --- Descriptors ----------------------------------------------------------- */

enum {
    USB_STR_LANGID = 0,
    USB_STR_MANUFACTURER,
    USB_STR_PRODUCT,
    USB_STR_SERIAL,
    USB_STR_CDC_ITF,
    USB_STR_COUNT
};

static const tusb_desc_device_t usbCdcDeviceDesc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    /* IAD-based composite class, as TUD_CDC_DESCRIPTOR uses an IAD. */
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_CDC_VID,
    .idProduct          = USB_CDC_PID,
    .bcdDevice          = USB_CDC_BCD_DEVICE,
    .iManufacturer      = USB_STR_MANUFACTURER,
    .iProduct           = USB_STR_PRODUCT,
    .iSerialNumber      = USB_STR_SERIAL,
    .bNumConfigurations = 1,
};

enum { USB_ITF_CDC_CTRL = 0, USB_ITF_CDC_DATA, USB_ITF_COUNT };

#define USB_CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

static const uint8_t usbCdcConfigDesc[] = {
    TUD_CONFIG_DESCRIPTOR(1, USB_ITF_COUNT, 0, USB_CONFIG_TOTAL_LEN, 0x00, USB_CDC_MAX_POWER_MA),
    TUD_CDC_DESCRIPTOR(USB_ITF_CDC_CTRL, USB_STR_CDC_ITF, USB_CDC_EP_NOTIF, USB_CDC_EP_NOTIF_SZ,
                       USB_CDC_EP_OUT, USB_CDC_EP_IN, USB_CDC_EP_DATA_SZ),
};

static const char *const usbCdcStrings[USB_STR_COUNT] = {
    [USB_STR_LANGID]       = NULL,          /* handled below: English (0x0409) */
    [USB_STR_MANUFACTURER] = "TimeTaker",
    [USB_STR_PRODUCT]      = USB_CDC_PRODUCT,
    [USB_STR_SERIAL]       = NULL,          /* handled below: the MCU UID */
    [USB_STR_CDC_ITF]      = "TimeTaker Control",
};

/* UTF-16LE string descriptor buffer: header + up to 31 characters. */
static uint16_t usbCdcStrDesc[32];

const uint8_t *tud_descriptor_device_cb(void)
{
    return (const uint8_t *)&usbCdcDeviceDesc;
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return usbCdcConfigDesc;
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    size_t chars;

    if (index == USB_STR_LANGID) {
        usbCdcStrDesc[1] = 0x0409U;
        chars = 1U;
    } else {
        if (index >= USB_STR_COUNT) {
            return NULL;
        }
        const char *s = (index == USB_STR_SERIAL) ? deviceIdSerial() : usbCdcStrings[index];
        chars = strlen(s);
        if (chars > 31U) {
            chars = 31U;
        }
        for (size_t i = 0U; i < chars; i++) {
            usbCdcStrDesc[1U + i] = (uint16_t)(uint8_t)s[i];   /* ASCII → UTF-16 */
        }
    }

    usbCdcStrDesc[0] = (uint16_t)(((uint16_t)TUSB_DESC_STRING << 8) | (uint16_t)(2U * chars + 2U));
    return usbCdcStrDesc;
}


/* --- Link state -------------------------------------------------------------- */

void tud_umount_cb(void)
{
    usbCdcLinkReset = true;
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    usbCdcLinkReset = true;   /* laptop asleep or cable pulled with VBUS still present */
}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    (void)itf;
    (void)rts;
    if (!dtr) {
        usbCdcLinkReset = true;   /* the host closed the port */
    }
}


/* --- API --------------------------------------------------------------------- */

ctrlStatus_e usbCdcInit(void)
{
    usbCdcLinkReset = false;

    tusb_rhport_init_t init = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_AUTO };
    return tusb_init(0, &init) ? CTRL_OK : CTRL_ERR_USB;
}

void usbCdcRun(void)
{
    tud_task();
}

bool usbCdcIsMounted(void)
{
    return tud_mounted();
}

uint16_t usbCdcRead(uint8_t *buf, uint16_t max)
{
    if (!tud_mounted() || tud_cdc_available() == 0U) {
        return 0U;
    }
    return (uint16_t)tud_cdc_read(buf, max);
}

uint16_t usbCdcWriteSpace(void)
{
    if (!tud_mounted()) {
        return 0U;
    }
    uint32_t space = tud_cdc_write_available();
    return (space > 0xFFFFU) ? 0xFFFFU : (uint16_t)space;
}

uint16_t usbCdcWrite(const uint8_t *buf, uint16_t len)
{
    if (len == 0U || usbCdcWriteSpace() < len) {
        return 0U;
    }
    return (uint16_t)tud_cdc_write(buf, len);
}

void usbCdcFlush(void)
{
    if (tud_mounted()) {
        (void)tud_cdc_write_flush();
    }
}

bool usbCdcTakeLinkReset(void)
{
    if (!usbCdcLinkReset) {
        return false;
    }
    usbCdcLinkReset = false;
    if (tud_mounted()) {
        tud_cdc_write_clear();   /* nothing queued for the previous host goes to the next */
        tud_cdc_read_flush();
    }
    return true;
}
