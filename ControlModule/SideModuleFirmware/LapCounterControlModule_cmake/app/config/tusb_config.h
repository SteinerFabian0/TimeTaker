/*
 * tusb_config.h
 *
 * @brief  TinyUSB configuration for the control module: one CDC-ACM
 *         interface, device mode only, no RTOS.
 *
 * TinyUSB (../third_party/tinyusb, 0.21.0) finds this file through the include
 * path. usb_cdc.c owns everything that uses it.
 *
 * Buffer sizes: the host protocol's largest frame is 254 bytes on the wire
 * (FRAME_CODEC_WIRE_BOUND(HP_MAX_FRAME_RAW + 1)). RX holds a few requests. TX
 * holds a burst of events plus a reserve that host_session keeps free for
 * responses. A full TX FIFO only delays the event pump; it never loses an
 * event, because events stay in the event log until they are sent.
 */

#pragma once

#define CFG_TUSB_MCU                OPT_MCU_STM32C5
#define CFG_TUSB_OS                 OPT_OS_NONE
#define CFG_TUSB_DEBUG              0

#define CFG_TUD_ENABLED             1
#define CFG_TUD_MAX_SPEED           OPT_MODE_FULL_SPEED
#define CFG_TUD_ENDPOINT0_SIZE      64

/* Buffers TinyUSB hands to the USB peripheral. The FSDEV port copies to and
 * from its own PMA, so no alignment or section rules apply. */
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN          __attribute__((aligned(4)))

#define CFG_TUD_CDC                 1
#define CFG_TUD_MSC                 0
#define CFG_TUD_HID                 0
#define CFG_TUD_MIDI                0
#define CFG_TUD_VENDOR              0

#define CFG_TUD_CDC_RX_BUFSIZE      512
#define CFG_TUD_CDC_TX_BUFSIZE      2048
#define CFG_TUD_CDC_EP_BUFSIZE      64

/* Never let TinyUSB overwrite queued TX when DTR is low: host_session only
 * writes inside an open session, and a closed port ends the session and clears
 * the FIFO (usbCdcTakeLinkReset). Backpressure instead of silent loss. */
#define CFG_TUD_CDC_TX_OVERWRITABLE_IF_NOT_CONNECTED 0
