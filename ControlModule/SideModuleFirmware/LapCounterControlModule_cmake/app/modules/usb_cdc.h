/*
 * USB CDC-ACM device: how the control module shows up on a laptop.
 *
 * The OS sees a virtual serial port named "TimeTaker" (product string), with
 * the MCU UID as its serial number. Chrome/Edge's Web Serial picker lists it
 * as "TimeTaker (COMx)" / "TimeTaker (ttyACM0)", and the web app filters on
 * USB_CDC_VID/USB_CDC_PID. CDC-ACM needs no driver on Windows 10+, macOS or
 * Linux. On Linux the user needs access to /dev/ttyACM* (dialout group, or
 * the udev rule in WebApp/README.md).
 *
 * Stack: TinyUSB 0.21.0 (../third_party/tinyusb), config in tusb_config.h.
 * This module is the only one that touches TinyUSB.
 *
 * Owns:
 *   - the USB_DRD_FS peripheral, its IRQ handler, and the CK48 kernel clock
 *     selection;
 *   - mx_usb_drd_fs_device_init(): the generated mx_usb_drd_fs.c is excluded
 *     from the build (cmake/ctrl_app.cmake) because it also defines
 *     USB_DRD_FS_IRQHandler for the HAL PCD driver. This file supplies the
 *     one function mx_system_init() calls from it. It does the clock/NVIC
 *     part and leaves the peripheral to TinyUSB.
 *
 * Byte stream semantics: usbCdcWrite() takes whole frames or nothing (the
 * caller checks usbCdcWriteSpace() first), so a frame never interleaves with
 * another. A link reset means unplug, unmount, or the host closing the port
 * (DTR falling). It is latched until usbCdcTakeLinkReset() reads it; the
 * session above treats it as "a new host may be on the other end".
 *
 * Context: main loop only (usbCdcRun() runs TinyUSB's task). The IRQ only
 * queues events for that task.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ctrl_status.h"
#include "host_protocol.h"

/* VID/PID and product string come from [usb] in protocol/host_protocol.toml,
 * the same source as the web app's port filter, so the two cannot drift.
 * Today: the pid.codes test pair (https://pid.codes/1209/0001/), fine for
 * private development, NOT for a published design (SYSTEM_ARCHITECTURE §8). */
#define USB_CDC_VID   HP_USB_VID
#ifdef CTRL_BOOTLOADER
/* The bootloader shows up with its own PID and product string ([usb] pid_boot). */
#define USB_CDC_PID   HP_USB_PID_BOOT
#define USB_CDC_PRODUCT HP_USB_PRODUCT_BOOT
#else
#define USB_CDC_PID   HP_USB_PID
#define USB_CDC_PRODUCT HP_USB_PRODUCT
#endif

/* Starts the USB device stack. Assumes deviceIdInit() has run (serial string). */
ctrlStatus_e usbCdcInit(void);

/* TinyUSB's task: enumeration, class requests, transfers. Every loop pass. */
void usbCdcRun(void);

/* Configured by a host (enumerated and set up). */
bool usbCdcIsMounted(void);

/* Reads up to max received bytes; returns how many. */
uint16_t usbCdcRead(uint8_t *buf, uint16_t max);

/* Bytes the TX FIFO can take right now. */
uint16_t usbCdcWriteSpace(void);

/* Queues len bytes if they all fit (returns len), else nothing (returns 0). */
uint16_t usbCdcWrite(const uint8_t *buf, uint16_t len);

/* Sends what is queued now instead of waiting for a full packet. */
void usbCdcFlush(void);

/* True once after unplug/unmount or the host closing the port; clears it. */
bool usbCdcTakeLinkReset(void);
