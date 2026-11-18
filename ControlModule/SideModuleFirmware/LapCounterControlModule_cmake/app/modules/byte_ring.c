/*
 * Byte ring. See byte_ring.h.
 */

#include "byte_ring.h"

#include <stdbool.h>
#include <stdint.h>

/* Orders the data copy before the index store that publishes it (compiler
 * barrier; on a single core that is all the ordering needed). */
#define BYTE_RING_FENCE()  __atomic_signal_fence(__ATOMIC_SEQ_CST)

bool byteRingInit(byteRing_t *r, uint8_t *buf, uint16_t size)
{
    r->buf  = buf;
    r->head = 0U;
    r->tail = 0U;
    if (size < 2U || size > 32768U || (size & (uint16_t)(size - 1U)) != 0U) {
        r->mask = 0U;
        return false;
    }
    r->mask = (uint16_t)(size - 1U);
    return true;
}

uint16_t byteRingCount(const byteRing_t *r)
{
    return (uint16_t)((r->head - r->tail) & r->mask);
}

uint16_t byteRingFree(const byteRing_t *r)
{
    return (r->mask == 0U) ? 0U : (uint16_t)(r->mask - byteRingCount(r));
}

bool byteRingPut(byteRing_t *r, const uint8_t *data, uint16_t len)
{
    if (len > byteRingFree(r)) {
        return false;
    }
    uint16_t h = r->head;
    for (uint16_t i = 0U; i < len; i++) {
        r->buf[h] = data[i];
        h = (uint16_t)((h + 1U) & r->mask);
    }
    BYTE_RING_FENCE();
    r->head = h;
    return true;
}

uint16_t byteRingPeek(const byteRing_t *r, uint8_t *out, uint16_t max)
{
    uint16_t n = byteRingCount(r);
    if (n > max) {
        n = max;
    }
    BYTE_RING_FENCE();
    uint16_t t = r->tail;
    for (uint16_t i = 0U; i < n; i++) {
        out[i] = r->buf[t];
        t = (uint16_t)((t + 1U) & r->mask);
    }
    return n;
}

void byteRingDrop(byteRing_t *r, uint16_t n)
{
    uint16_t have = byteRingCount(r);
    if (n > have) {
        n = have;
    }
    BYTE_RING_FENCE();
    r->tail = (uint16_t)((r->tail + n) & r->mask);
}

uint16_t byteRingGet(byteRing_t *r, uint8_t *out, uint16_t max)
{
    uint16_t n = byteRingPeek(r, out, max);
    byteRingDrop(r, n);
    return n;
}

void byteRingClear(byteRing_t *r)
{
    r->tail = r->head;
}
