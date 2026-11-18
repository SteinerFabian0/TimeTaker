/*
 * Byte ring: a single-producer, single-consumer FIFO of bytes.
 *
 * One side may be an ISR (the USART1 receive callback writes, the main loop
 * reads), so head and tail each have exactly one writer, and the data store
 * is fenced before the index that publishes it. No lock is needed on a
 * single-core M33 as long as that rule holds: never call put from two
 * contexts, never call get from two contexts.
 *
 * The capacity is size - 1 (one slot stays free to tell full from empty).
 * size must be a power of two, at most 32768.
 *
 * Pure logic: no HAL. Host-tested (test/host_nrf_link).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t          *buf;
    uint16_t          mask;     /* size - 1 */
    volatile uint16_t head;     /* written by the producer only */
    volatile uint16_t tail;     /* written by the consumer only */
} byteRing_t;

/* Returns false if size is not a power of two (the ring is then unusable). */
bool byteRingInit(byteRing_t *r, uint8_t *buf, uint16_t size);

uint16_t byteRingCount(const byteRing_t *r);
uint16_t byteRingFree(const byteRing_t *r);

/* Producer side. Put copies all len bytes or nothing. */
bool byteRingPut(byteRing_t *r, const uint8_t *data, uint16_t len);

/* Consumer side. Returns the bytes copied (up to max). Peek leaves them. */
uint16_t byteRingGet(byteRing_t *r, uint8_t *out, uint16_t max);
uint16_t byteRingPeek(const byteRing_t *r, uint8_t *out, uint16_t max);
void     byteRingDrop(byteRing_t *r, uint16_t n);

/* Consumer side: empties the ring (only safe while the producer is quiet). */
void byteRingClear(byteRing_t *r);
