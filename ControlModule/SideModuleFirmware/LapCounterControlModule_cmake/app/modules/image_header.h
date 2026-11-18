/*
 * Firmware image header and flash layout of the control module
 * (SYSTEM_ARCHITECTURE §10.1, CONTROL_FIRMWARE_GUIDE §5.7).
 *
 * Flash: 256 KB in two banks of 16 pages of 8 KB (FLASH_PAGE_SIZE in the
 * DFP; datasheet DS15125 §3.4: "Each bank contains 16 pages of 8 Kbytes").
 *
 *   0x0800_0000  ctrl_boot    32 KB  (4 pages)  bootloader, write-protected later
 *   0x0800_8000  app         208 KB  (26 pages) image header at +0x400
 *   0x0803_C000  settings     16 KB  (2 pages)  bank 2, pages 14 and 15
 *
 * The bootloader was planned at 24 KB (SYSTEM_ARCHITECTURE §10.1 left it
 * open "to confirm against the erase-page size"); USB (TinyUSB + the HAL USB
 * core), the USART1 HAL and nrf_link need about 29 KB at -Os, so it takes
 * four 8 KB pages and the app starts at 0x0800_8000.
 *
 * The standalone bring-up image links the app at 0x0800_0000 instead (no
 * bootloader); its header sits at 0x0800_0400 and is simply never checked.
 *
 * Why +0x400: the vector table must stay at the start of the image (VTOR,
 * which SystemInit sets from the linker's __VECTOR_TABLE), and it is about
 * 0x270 bytes. VTOR alignment needs the table at the image base, so the
 * header follows at the next fixed 1 KB boundary. The linker script asserts
 * that the vectors fit below it.
 *
 * Header (32 B, two flash quad-words, written LAST by the bootloader):
 *   magic, boardType, fw version, git rev, size (whole image, header
 *   included), imageCrc = CRC-32 (crc32.h) over the image with these 32 bytes
 *   skipped. The post-build step (tools/image_stamp.py) fills size and
 *   imageCrc into the .bin and the .elf.
 *
 * Pure logic: no HAL. Host-tested (test/host_update).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "host_protocol.h"

#define IMAGE_FLASH_BASE        0x08000000U
#define IMAGE_FLASH_SIZE        0x00040000U
#define IMAGE_PAGE_SIZE         0x2000U
#define IMAGE_BOOT_SIZE         0x8000U
#define IMAGE_SETTINGS_SIZE     (2U * IMAGE_PAGE_SIZE)
#define IMAGE_APP_BASE          (IMAGE_FLASH_BASE + IMAGE_BOOT_SIZE)
#define IMAGE_APP_SLOT_SIZE     (IMAGE_FLASH_SIZE - IMAGE_BOOT_SIZE - IMAGE_SETTINGS_SIZE)
#define IMAGE_SETTINGS_BASE     (IMAGE_FLASH_BASE + IMAGE_FLASH_SIZE - IMAGE_SETTINGS_SIZE)

#define IMAGE_HEADER_OFFSET     0x400U
#define IMAGE_HEADER_BYTES      32U
#define IMAGE_HEADER_MAGIC      0x4D495454U   /* "TTIM" little-endian */
#define IMAGE_HEADER_VERSION    1U

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  boardType;       /* hpBoardType_e: CONTROL for the app        */
    uint8_t  fwMajor;
    uint8_t  fwMinor;
    uint8_t  fwPatch;
    uint32_t gitRev;
    uint32_t size;            /* image bytes from the image base           */
    uint32_t imageCrc;        /* CRC-32 over the image, header skipped     */
    uint16_t headerVersion;   /* IMAGE_HEADER_VERSION                      */
    uint16_t reserved0;
    uint32_t reserved1;
    uint32_t reserved2;
} imageHeader_t;

_Static_assert(sizeof(imageHeader_t) == IMAGE_HEADER_BYTES, "imageHeader_t");

/*
 * Checks a header for this slot: CTRL_ERR_IMAGE_CRC for a bad magic, version
 * or a size that cannot hold the header, CTRL_ERR_IMAGE_BOARD for another
 * board, CTRL_ERR_IMAGE_TOO_BIG for a size beyond the slot.
 */
ctrlStatus_e imageHeaderCheck(const imageHeader_t *h, uint8_t boardType, uint32_t slotSize);

/* CRC-32 over size bytes at image with the header's 32 bytes skipped. */
uint32_t imageCrcSkipHeader(const uint8_t *image, uint32_t size);

/*
 * Full check of an image in memory (the bootloader's boot decision): header
 * as above, imageCrc matches, and the vector table is plausible (initial SP
 * inside RAM, reset vector inside the image and Thumb).
 */
ctrlStatus_e imageCheck(const uint8_t *image, uint32_t slotSize, uint8_t boardType,
                        uint32_t imageAddr, uint32_t ramBase, uint32_t ramSize);
