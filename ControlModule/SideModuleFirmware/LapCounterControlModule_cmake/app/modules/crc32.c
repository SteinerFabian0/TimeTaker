/*
 * CRC-32 (IEEE). See crc32.h.
 */

#include "crc32.h"

#include <stdint.h>

uint32_t crc32Update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0U; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0U; bit < 8U; bit++) {
            crc = ((crc & 1U) != 0U) ? ((crc >> 1) ^ 0xEDB88320U) : (crc >> 1);
        }
    }
    return crc;
}

uint32_t crc32Compute(const uint8_t *data, uint32_t len)
{
    return crc32Final(crc32Update(CRC32_INIT, data, len));
}
