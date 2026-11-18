/*
 * Image CRC32. See image_crc.h.
 */

#include "image_crc.h"

#include <stdint.h>

uint32_t imageCrc32(uint32_t crc, const uint8_t *data, uint32_t len)
{
    crc = ~crc;
    for (uint32_t i = 0U; i < len; i++) {
        crc ^= data[i];
        for (uint32_t bit = 0U; bit < 8U; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

#ifndef LANE_HOST_TEST

/* From the linker script: the image starts with .vectors at the flash base
 * and ends with the load image of .data (_sidata, _sdata.._edata long). */
#define IMAGE_CRC_FLASH_BASE  0x08000000UL

extern const uint8_t _sidata[];
extern uint8_t       _sdata[];
extern uint8_t       _edata[];

uint32_t imageCrcOfRunningImage(void)
{
    const uint8_t *start = (const uint8_t *)IMAGE_CRC_FLASH_BASE;
    uint32_t       end   = (uint32_t)(uintptr_t)_sidata + (uint32_t)(_edata - _sdata);

    return imageCrc32(0U, start, end - (uint32_t)(uintptr_t)start);
}

#endif /* LANE_HOST_TEST */
