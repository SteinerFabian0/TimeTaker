/*
 * Byte ring. See byte_ring.h.
 */

#include "byte_ring.h"

#include <stddef.h>

#define LOAD_ACQ(p)      __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define LOAD_RLX(p)      __atomic_load_n((p), __ATOMIC_RELAXED)
#define STORE_REL(p, v)  __atomic_store_n((p), (v), __ATOMIC_RELEASE)

bool byteRingInit(byteRing_t *r, uint8_t *buf, uint16_t size)
{
    if (buf == NULL || size == 0U || size > 32768U || (size & (size - 1U)) != 0U) {
        return false;
    }
    r->buf  = buf;
    r->size = size;
    r->head = 0U;
    r->tail = 0U;
    return true;
}

uint16_t byteRingUsed(const byteRing_t *r)
{
    return (uint16_t)(LOAD_ACQ(&r->head) - LOAD_ACQ(&r->tail));
}

uint16_t byteRingFree(const byteRing_t *r)
{
    return (uint16_t)(r->size - byteRingUsed(r));
}

uint16_t byteRingWrite(byteRing_t *r, const uint8_t *data, uint16_t len)
{
    const uint16_t head = LOAD_RLX(&r->head);
    const uint16_t room = (uint16_t)(r->size - (uint16_t)(head - LOAD_ACQ(&r->tail)));
    const uint16_t n    = (len < room) ? len : room;
    const uint16_t mask = (uint16_t)(r->size - 1U);

    for (uint16_t i = 0U; i < n; i++) {
        r->buf[(uint16_t)(head + i) & mask] = data[i];
    }
    STORE_REL(&r->head, (uint16_t)(head + n));
    return n;
}

bool byteRingWriteAll(byteRing_t *r, const uint8_t *data, uint16_t len)
{
    if (len > byteRingFree(r)) {
        return false;
    }
    return byteRingWrite(r, data, len) == len;
}

uint16_t byteRingRead(byteRing_t *r, uint8_t *out, uint16_t max)
{
    const uint16_t tail  = LOAD_RLX(&r->tail);
    const uint16_t avail = (uint16_t)(LOAD_ACQ(&r->head) - tail);
    const uint16_t n     = (max < avail) ? max : avail;
    const uint16_t mask  = (uint16_t)(r->size - 1U);

    for (uint16_t i = 0U; i < n; i++) {
        out[i] = r->buf[(uint16_t)(tail + i) & mask];
    }
    STORE_REL(&r->tail, (uint16_t)(tail + n));
    return n;
}

uint16_t byteRingPeek(const byteRing_t *r, const uint8_t **data)
{
    const uint16_t tail   = LOAD_RLX(&r->tail);
    const uint16_t avail  = (uint16_t)(LOAD_ACQ(&r->head) - tail);
    const uint16_t offset = (uint16_t)(tail & (uint16_t)(r->size - 1U));
    const uint16_t toEnd  = (uint16_t)(r->size - offset);

    *data = &r->buf[offset];
    return (avail < toEnd) ? avail : toEnd;
}

void byteRingConsume(byteRing_t *r, uint16_t n)
{
    const uint16_t used = byteRingUsed(r);
    if (n > used) {
        n = used;
    }
    STORE_REL(&r->tail, (uint16_t)(LOAD_RLX(&r->tail) + n));
}

void byteRingClear(byteRing_t *r)
{
    STORE_REL(&r->tail, LOAD_ACQ(&r->head));
}
