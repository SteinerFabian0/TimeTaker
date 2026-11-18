/*
 * Byte ring for the UART paths. Single producer, single consumer, lock-free
 * between them (one side may be an ISR). Several producers or consumers need
 * the caller's lock.
 *
 *   RX: UARTE ISR writes (byteRingWrite), the main loop reads (byteRingRead).
 *   TX: threads write whole frames under a lock (byteRingWriteAll), the
 *       UARTE DMA sends from the contiguous readable part (byteRingPeek /
 *       byteRingConsume).
 *
 * Size: a power of two, at most 32768. Head and tail run free (uint16_t) and
 * are masked on access, so the ring holds `size` bytes, not size - 1.
 *
 * Pure logic: no Zephyr. The memory ordering uses the GCC/Clang __atomic
 * builtins (acquire/release on the indices).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t  *buf;
    uint16_t  size;
    uint16_t  head;     /* written by the producer only */
    uint16_t  tail;     /* written by the consumer only */
} byteRing_t;

/* false if size is not a power of two in 1..32768. */
bool byteRingInit(byteRing_t *r, uint8_t *buf, uint16_t size);

uint16_t byteRingUsed(const byteRing_t *r);
uint16_t byteRingFree(const byteRing_t *r);

/* Producer: writes as much as fits, returns the count written. */
uint16_t byteRingWrite(byteRing_t *r, const uint8_t *data, uint16_t len);

/* Producer: writes all of it or nothing. */
bool byteRingWriteAll(byteRing_t *r, const uint8_t *data, uint16_t len);

/* Consumer: reads up to max bytes, returns the count read. */
uint16_t byteRingRead(byteRing_t *r, uint8_t *out, uint16_t max);

/* Consumer: the contiguous readable run at the tail (0 if empty). *data
 * points into the ring and stays valid until byteRingConsume() passes it. */
uint16_t byteRingPeek(const byteRing_t *r, const uint8_t **data);

/* Consumer: drops n bytes (n <= byteRingUsed). */
void byteRingConsume(byteRing_t *r, uint16_t n);

/* Consumer side: empties the ring. */
void byteRingClear(byteRing_t *r);
