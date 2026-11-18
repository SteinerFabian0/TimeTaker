/*
 * CRC-32 (IEEE 802.3 / zlib / PNG): poly 0x04C11DB7 reflected (0xEDB88320),
 * init 0xFFFFFFFF, reflected in and out, xorout 0xFFFFFFFF. Check value of
 * "123456789" = 0xCBF43926.
 *
 * The one CRC-32 of the system: the settings records (settings_record) use
 * this exact variant, so a `zlib.crc32()` on the laptop matches.
 *
 * On the target this runs on the C5 hardware CRC unit (initialised on first
 * use); the host tests build the bitwise software version (no table), and a
 * test checks a model of the unit's register settings against it. See
 * crc32.c for the unit settings. -DCRC32_SOFTWARE forces software on the
 * target. The CRC-16/CCITT in frame_codec.c stays software on purpose: that
 * file is shared byte-identically with the nRF build and the host tests (and
 * is also a different CRC, over frames of a few bytes, where setting up the
 * unit would cost more than it saves).
 *
 * Host-tested (test/host_settings_record). Main loop only.
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
