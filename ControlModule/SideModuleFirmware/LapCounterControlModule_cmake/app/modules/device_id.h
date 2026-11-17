/*
 * Who this board is, and which boot this is.
 *
 *   UID       The 96-bit MCU unique ID. As 24 hex digits it becomes the USB
 *             serial number, so the OS and the browser can tell two boxes
 *             apart.
 *   bootId    32 bits that differ from one boot to the next. Every event
 *             sequence number and every sysUs is only meaningful within one
 *             boot. The web app keys its event store on (bootId, evSeq) and
 *             sees a reboot as a new bootId (SYSTEM_ARCHITECTURE §3).
 *   reset     Why this boot happened (RCC reset flags), for EVT_BOOT.
 *
 * bootId without an RNG: a boot counter in .noinit RAM (survives resets,
 * garbage after power-on), mixed with the UID through a 32-bit hash. After a
 * power-on the "counter" starts from whatever the RAM held, which is as good
 * as random for this purpose. Collisions only matter between two consecutive
 * boots of the same box, where the counter guarantees a change. Swap in the
 * TRNG later if a stronger ID is ever needed; the API stays.
 *
 * Owns: the RCC reset flags (reads and clears them once, at init).
 */

#pragma once

#include <stdint.h>

#include "ctrl_status.h"

#define DEVICE_ID_SERIAL_CHARS  24U   /* 96-bit UID as hex, without terminator */

/* Reads UID and reset cause, advances the boot counter. Call once, early. */
ctrlStatus_e deviceIdInit(void);

uint32_t deviceIdBootId(void);

/* hpResetCause_e value of this boot. */
uint8_t deviceIdResetCause(void);

/* The UID as DEVICE_ID_SERIAL_CHARS uppercase hex digits plus a terminator. */
const char *deviceIdSerial(void);
