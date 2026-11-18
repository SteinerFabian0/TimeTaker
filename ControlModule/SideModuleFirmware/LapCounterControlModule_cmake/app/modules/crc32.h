/*
 * CRC-32 (IEEE 802.3 / zlib / PNG): poly 0x04C11DB7 reflected (0xEDB88320),
 * init 0xFFFFFFFF, reflected in and out, xorout 0xFFFFFFFF. Check value of
 * "123456789" = 0xCBF43926.
 *
 * The one CRC-32 of the system: the settings records (settings_record) use
 * this exact variant, so a `zlib.crc32()` on the laptop matches.
 *
 * Bitwise, no table: 1 KB of flash saved; a settings record is a few hundred
 * bytes, so speed does not matter. The C5 CRC unit could do it in
 * hardware (CONTROL_FIRMWARE_NOTES "CRC"), but then the host tests would test
 * something else than the target runs.
 *
 * Pure logic: no HAL, no state. Host-tested (test/host_settings_record).
 */

#pragma once

#include <stdint.h>

#define CRC32_INIT  0xFFFFFFFFU

/* Streaming: crc = CRC32_INIT; crc = crc32Update(crc, ...)...; result = crc32Final(crc). */
uint32_t crc32Update(uint32_t crc, const uint8_t *data, uint32_t len);

static inline uint32_t crc32Final(uint32_t crc)
{
    return crc ^ 0xFFFFFFFFU;
}

/* One-shot. */
uint32_t crc32Compute(const uint8_t *data, uint32_t len);
