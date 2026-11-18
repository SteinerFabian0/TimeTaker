/*
 * Board identity and boot identity. See device_id.h.
 */

#include "device_id.h"

#include <stdint.h>

#include "stm32_hal.h"
#include "host_protocol.h"
#include "ctrl_status.h"

#define DEVICE_ID_NOINIT_MAGIC  0x7155BEEFU

typedef struct {
    uint32_t magic;
    uint32_t bootCount;
} deviceIdNoinit_t;

/* .noinit: not zeroed by startup, kept across resets (linker script). */
__attribute__((section(".noinit"))) static deviceIdNoinit_t deviceIdNoinit;

static uint32_t deviceIdBoot;
static uint8_t  deviceIdReset = (uint8_t)HP_RESET_UNKNOWN;
static char     deviceIdSerialStr[DEVICE_ID_SERIAL_CHARS + 1U];

/* murmur3 finalizer: spreads every input bit over the result. */
static uint32_t deviceIdMix(uint32_t h)
{
    h ^= h >> 16;
    h *= 0x85EBCA6BU;
    h ^= h >> 13;
    h *= 0xC2B2AE35U;
    h ^= h >> 16;
    return h;
}

static uint8_t deviceIdResetFromFlags(uint32_t flags)
{
    /* Most specific first: a watchdog reset also sets PINRSTF on many STM32s. */
    if ((flags & (HAL_RCC_RESET_FLAG_IWDG | HAL_RCC_RESET_FLAG_WWDG)) != 0U) {
        return (uint8_t)HP_RESET_WATCHDOG;
    }
    if ((flags & HAL_RCC_RESET_FLAG_SW) != 0U) {
        return (uint8_t)HP_RESET_SOFTWARE;
    }
    if ((flags & HAL_RCC_RESET_FLAG_PWR) != 0U) {
        return (uint8_t)HP_RESET_POWER_ON;   /* BOR/POR/PDR: the board lost power */
    }
    if ((flags & HAL_RCC_RESET_FLAG_PIN) != 0U) {
        return (uint8_t)HP_RESET_PIN;
    }
    return (uint8_t)HP_RESET_UNKNOWN;
}

static void deviceIdHex(char *out, uint32_t word)
{
    static const char digits[] = "0123456789ABCDEF";
    for (int i = 7; i >= 0; i--) {
        out[i] = digits[word & 0xFU];
        word >>= 4;
    }
}

ctrlStatus_e deviceIdInit(void)
{
    hal_device_uid_t uid = { 0 };
    ctrlStatus_e     status = ctrlStatusFromHal(HAL_GetDeviceUniqueID(&uid));

    deviceIdHex(&deviceIdSerialStr[0], uid.uid_2);
    deviceIdHex(&deviceIdSerialStr[8], uid.uid_1);
    deviceIdHex(&deviceIdSerialStr[16], uid.uid_0);
    deviceIdSerialStr[DEVICE_ID_SERIAL_CHARS] = '\0';

    deviceIdReset = deviceIdResetFromFlags(HAL_RCC_GetResetSource());
    HAL_RCC_ClearResetFlags();

    if (deviceIdNoinit.magic != DEVICE_ID_NOINIT_MAGIC) {
        deviceIdNoinit.magic = DEVICE_ID_NOINIT_MAGIC;   /* keep the garbage count: it is our seed */
    }
    deviceIdNoinit.bootCount++;

    uint32_t h = deviceIdMix(uid.uid_0 ^ 0x9E3779B9U);
    h = deviceIdMix(h ^ uid.uid_1);
    h = deviceIdMix(h ^ uid.uid_2);
    h = deviceIdMix(h ^ deviceIdNoinit.bootCount);
    deviceIdBoot = (h == 0U) ? 1U : h;   /* 0 reads as "no boot" in the app */

    return status;
}

void deviceIdMixEntropy(uint32_t entropy)
{
    uint32_t h = deviceIdMix(deviceIdBoot ^ entropy);
    deviceIdBoot = (h == 0U) ? 1U : h;
}

uint32_t deviceIdBootId(void)
{
    return deviceIdBoot;
}

uint8_t deviceIdResetCause(void)
{
    return deviceIdReset;
}

void deviceIdOverrideResetCause(uint8_t cause)
{
    deviceIdReset = cause;
}

const char *deviceIdSerial(void)
{
    return deviceIdSerialStr;
}
