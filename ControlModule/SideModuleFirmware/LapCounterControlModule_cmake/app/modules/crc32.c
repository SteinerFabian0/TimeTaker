/*
 * CRC-32 (IEEE). See crc32.h.
 *
 * Two implementations of the one CRC-32:
 *
 *  - software (bitwise): always compiled; the host tests build only this.
 *  - hardware (C5 CRC unit): used on the target (any ARM build) unless
 *    CRC32_SOFTWARE is defined. If the unit cannot be initialised the
 *    software path is the fallback.
 *
 * The CRC unit is set up so its raw output equals the software `crc` state
 * (the value before the final XOR), which keeps the streaming interface of
 * crc32.h identical:
 *
 *   POLYSIZE = 32 bit, POL = 0x04C11DB7   (normal form of 0xEDB88320)
 *   REV_IN   = byte      (each input byte is bit-reversed: reflected input)
 *   REV_OUT  = bit       (the 32-bit result is bit-reversed: reflected output)
 *   INIT     = rbit32(crc) written before each call: the unit holds the
 *              unreflected state, and REV_OUT turns it into `crc` on read.
 *              For crc = CRC32_INIT that is 0xFFFFFFFF.
 *   data is written as 8-bit accesses (word writes would need REV_IN = word,
 *   and a tail of 1..3 bytes would still need byte writes).
 *   The final XOR 0xFFFFFFFF is done in software (crc32Final): the unit has
 *   none.
 *
 * test/host_settings_record checks a bit-exact model of this register
 * semantics against the software path, plus the "123456789" vector, so the
 * two paths agree by construction. Context: main loop only (one unit, no
 * locking).
 */

#include "crc32.h"

#include <stdbool.h>
#include <stdint.h>

#if defined(__arm__) && !defined(CRC32_SOFTWARE)
#define CRC32_USE_HW  1
#else
#define CRC32_USE_HW  0
#endif

static uint32_t crc32UpdateSw(uint32_t crc, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0U; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0U; bit < 8U; bit++) {
            crc = ((crc & 1U) != 0U) ? ((crc >> 1) ^ 0xEDB88320U) : (crc >> 1);
        }
    }
    return crc;
}

#if CRC32_USE_HW

#include "stm32_hal.h"
#include "mx_crc.h"

static bool crcReady;
static bool crcTried;

/* Generated code only: init on first use (clock + HAL init), then the
 * CRC-32 configuration (the generated defaults are poly/init only, no
 * reversal). */
static bool crcEnsure(void)
{
    if (!crcTried) {
        crcTried = true;
        if (mx_crc_init() != NULL) {
            LL_CRC_SetPolynomialSize(CRC, LL_CRC_POLY_SIZE_32B);
            LL_CRC_SetPolynomialCoef(CRC, 0x04C11DB7U);
            LL_CRC_SetInputDataReverseMode(CRC, LL_CRC_INDATA_REVERSE_BYTE);
            LL_CRC_SetOutputDataReverseMode(CRC, LL_CRC_OUTDATA_REVERSE_BIT);
            crcReady = true;
        }
    }
    return crcReady;
}

uint32_t crc32Update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    if (!crcEnsure()) {
        return crc32UpdateSw(crc, data, len);
    }
    uint32_t init;
    __asm volatile ("rbit %0, %1" : "=r" (init) : "r" (crc));
    LL_CRC_SetInitialData(CRC, init);
    LL_CRC_ResetCRCCalculationUnit(CRC);       /* DR := INIT */
    for (uint32_t i = 0U; i < len; i++) {
        LL_CRC_FeedData8(CRC, data[i]);
    }
    return LL_CRC_ReadData32(CRC);
}

#else

uint32_t crc32Update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    return crc32UpdateSw(crc, data, len);
}

#endif

uint32_t crc32Compute(const uint8_t *data, uint32_t len)
{
    return crc32Final(crc32Update(CRC32_INIT, data, len));
}
