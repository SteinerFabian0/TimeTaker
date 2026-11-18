/*
 * CRC32 of the running image, for the ID register (imageCrc), so the
 * control module and the app can tell which build a lane runs.
 *
 * The algorithm is host-tested (test/host_regs): IEEE 802.3 CRC32,
 * reflected, init and final XOR 0xFFFFFFFF; crc32("123456789") = 0xCBF43926,
 * the same as zlib / Python binascii.crc32. Over the image it equals the
 * CRC32 of `arm-none-eabi-objcopy -O binary --gap-fill 0xFF` of the .elf
 * (flash start to the end of the .data load image): the alignment gaps
 * between sections stay erased (0xFF) when the .elf is flashed.
 *
 * imageCrcOfRunningImage() needs the linker symbols and is target-only.
 * On the target the CRC runs on the C5 hardware CRC unit (LL registers,
 * settings in image_crc.c); the host tests build the bitwise software
 * version, and test/host_regs checks a bit-exact model of the unit's
 * settings against it. -DIMAGE_CRC_SOFTWARE forces software on the target.
 */

#pragma once

#include <stdint.h>

/* Continues a CRC: start with crc = 0. */
uint32_t imageCrc32(uint32_t crc, const uint8_t *data, uint32_t len);

/* CRC32 of the flash image this firmware runs from (target only). */
uint32_t imageCrcOfRunningImage(void);
