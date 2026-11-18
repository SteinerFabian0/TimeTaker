/*
 * Image CRC32. See image_crc.h.
 */

#include "image_crc.h"

#include <stdint.h>

#if !defined(LANE_HOST_TEST) && !defined(IMAGE_CRC_SOFTWARE)

#include "stm32_hal.h"
#include "stm32c5xx_ll_crc.h"   /* HAL CRC module is not enabled in the lane project */

/* The C5 CRC unit, by LL registers: the lane's CubeMX project has no CRC
 * block (CubeMX change, optional: enable CRC so mx_crc.c does the clock).
 * Settings: POLYSIZE 32, POL 0x04C11DB7, REV_IN byte, REV_OUT bit, 8-bit
 * writes. The unit holds the unreflected state and REV_OUT reflects it on
 * read, so INIT = rbit32(~crc) continues a CRC exactly like the software
 * path; the final XOR (~) is done in software. For crc = 0 that is
 * INIT = 0xFFFFFFFF. Called once at boot from the main loop, no locking. */
uint32_t imageCrc32(uint32_t crc, const uint8_t *data, uint32_t len)
{
    uint32_t init;

    LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_CRC);
    LL_CRC_SetPolynomialSize(CRC, LL_CRC_POLY_SIZE_32B);
    LL_CRC_SetPolynomialCoef(CRC, 0x04C11DB7U);
    LL_CRC_SetInputDataReverseMode(CRC, LL_CRC_INDATA_REVERSE_BYTE);
    LL_CRC_SetOutputDataReverseMode(CRC, LL_CRC_OUTDATA_REVERSE_BIT);
    __asm volatile ("rbit %0, %1" : "=r" (init) : "r" (~crc));
    LL_CRC_SetInitialData(CRC, init);
    LL_CRC_ResetCRCCalculationUnit(CRC);       /* DR := INIT */
    for (uint32_t i = 0U; i < len; i++) {
        LL_CRC_FeedData8(CRC, data[i]);
    }
    return ~LL_CRC_ReadData32(CRC);
}

#else

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

#endif

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
