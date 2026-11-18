/*
 * Event and raw-sample FIFOs. See event_fifo.h for the rules.
 */

#include "event_fifo.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* =========================================================================
 *                               EVENT FIFO
 * ========================================================================= */

void eventFifoInit(eventFifo_t *f)
{
    memset(f, 0, sizeof(*f));
}

laneStatus_e eventFifoPush(eventFifo_t *f, laneEvent_t *ev)
{
    ev->seq = f->nextSeq;
    f->nextSeq++;

    if (f->count >= EVENT_FIFO_DEPTH) {
        if (f->dropped < 0xFFFFU) {
            f->dropped++;
        }
        f->overflowed = true;
        return LANE_ERR_FIFO_FULL;
    }

    uint32_t tail = ((uint32_t)f->head + f->count) % EVENT_FIFO_DEPTH;
    f->ring[tail] = *ev;
    f->count++;
    return LANE_OK;
}

uint8_t eventFifoCount(const eventFifo_t *f)
{
    return f->count;
}

uint16_t eventFifoNextSeq(const eventFifo_t *f)
{
    return f->nextSeq;
}

uint16_t eventFifoDropped(const eventFifo_t *f)
{
    return f->dropped;
}

bool eventFifoOverflowed(const eventFifo_t *f)
{
    return f->overflowed;
}

void eventFifoClearCounters(eventFifo_t *f)
{
    f->dropped    = 0U;
    f->overflowed = false;
}

void eventFifoFlush(eventFifo_t *f)
{
    f->head  = 0U;
    f->count = 0U;
}

uint32_t eventFifoBuildRead(const eventFifo_t *f, uint8_t *buf, uint32_t cap, uint8_t *offered)
{
    uint8_t n = (f->count < LANE_FIFO_READ_MAX) ? f->count : (uint8_t)LANE_FIFO_READ_MAX;

    if (cap < LANE_FIFO_READ_BYTES) {
        *offered = 0U;
        return 0U;
    }

    laneFifoHeader_t hdr = {
        .count     = n,
        .remaining = (uint8_t)(f->count - n),
        .reserved  = 0U,
    };
    memcpy(buf, &hdr, sizeof(hdr));

    uint32_t pos = sizeof(hdr);
    for (uint32_t i = 0U; i < n; i++) {
        memcpy(&buf[pos], &f->ring[(f->head + i) % EVENT_FIFO_DEPTH], sizeof(laneEvent_t));
        pos += sizeof(laneEvent_t);
    }

    *offered = n;
    return pos;
}

uint32_t eventFifoCompleteRead(eventFifo_t *f, uint8_t offered, uint32_t clockedOut)
{
    uint32_t n = 0U;

    if (clockedOut > sizeof(laneFifoHeader_t)) {
        n = (clockedOut - sizeof(laneFifoHeader_t)) / sizeof(laneEvent_t);
    }
    if (n > offered) {
        n = offered;
    }
    if (n > f->count) {
        n = f->count;
    }

    f->head   = (uint8_t)((f->head + n) % EVENT_FIFO_DEPTH);
    f->count -= (uint8_t)n;
    return n;
}


/* =========================================================================
 *                                RAW FIFO
 * ========================================================================= */

void rawFifoInit(rawFifo_t *f)
{
    memset(f, 0, sizeof(*f));
}

static rawChunk_t *rawFifoTail(rawFifo_t *f)
{
    return &f->chunks[((uint32_t)f->head + f->count - 1U) % RAW_FIFO_CHUNKS];
}

/* Whether a sample at tUs continues the tail chunk's grid. */
static bool rawFifoContinues(const rawChunk_t *c, uint64_t tUs, uint32_t periodUs)
{
    if (c->closed || c->count >= LANE_RAW_READ_MAX || c->periodUs != periodUs) {
        return false;
    }

    uint64_t expected = c->firstUs + (uint64_t)c->count * periodUs;
    uint64_t diff     = (tUs > expected) ? (tUs - expected) : (expected - tUs);
    return diff <= periodUs / 4U;
}

void rawFifoPush(rawFifo_t *f, uint64_t tUs, uint32_t periodUs, uint16_t value)
{
    if (f->count > 0U) {
        rawChunk_t *tail = rawFifoTail(f);
        if (rawFifoContinues(tail, tUs, periodUs)) {
            tail->samples[tail->count] = value;
            tail->count++;
            if (tail->count >= LANE_RAW_READ_MAX) {
                tail->closed = true;
            }
            return;
        }
        tail->closed = true;
    }

    if (f->count >= RAW_FIFO_CHUNKS) {
        /* Keep the oldest. One skipped chunkSeq per dropping episode marks
         * the gap for the reader. */
        if (!f->dropping) {
            f->dropping = true;
            f->nextChunkSeq++;
        }
        f->droppedSamples++;
        return;
    }

    f->dropping = false;
    rawChunk_t *c = &f->chunks[((uint32_t)f->head + f->count) % RAW_FIFO_CHUNKS];
    c->firstUs    = tUs;
    c->periodUs   = periodUs;
    c->chunkSeq   = f->nextChunkSeq;
    c->count      = 1U;
    c->closed     = false;
    c->samples[0] = value;
    f->nextChunkSeq++;
    f->count++;
}

void rawFifoFlush(rawFifo_t *f)
{
    f->head     = 0U;
    f->count    = 0U;
    f->dropping = false;
}

bool rawFifoHasChunk(const rawFifo_t *f)
{
    return f->count > 1U || (f->count == 1U && f->chunks[f->head].closed);
}

uint32_t rawFifoDroppedSamples(const rawFifo_t *f)
{
    return f->droppedSamples;
}

uint32_t rawFifoBuildRead(const rawFifo_t *f, uint8_t *buf, uint32_t cap, uint8_t *offered)
{
    laneRawHeader_t hdr;

    if (cap < LANE_RAW_READ_BYTES) {
        *offered = 0U;
        return 0U;
    }

    memset(&hdr, 0, sizeof(hdr));
    if (f->count == 0U) {
        hdr.chunkSeq = f->nextChunkSeq;
        memcpy(buf, &hdr, sizeof(hdr));
        *offered = 0U;
        return sizeof(hdr);
    }

    const rawChunk_t *c = &f->chunks[f->head];
    hdr.count    = c->count;
    hdr.chunkSeq = c->chunkSeq;
    hdr.periodUs = c->periodUs;
    hdr.firstUs  = c->firstUs;
    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(&buf[sizeof(hdr)], c->samples, (uint32_t)c->count * sizeof(uint16_t));

    *offered = c->count;
    return sizeof(hdr) + (uint32_t)c->count * sizeof(uint16_t);
}

uint32_t rawFifoCompleteRead(rawFifo_t *f, uint8_t offered, uint32_t clockedOut)
{
    if (f->count == 0U) {
        return 0U;
    }

    rawChunk_t *c = &f->chunks[f->head];
    uint32_t    n = 0U;

    if (clockedOut > sizeof(laneRawHeader_t)) {
        n = (clockedOut - sizeof(laneRawHeader_t)) / sizeof(uint16_t);
    }
    if (n > offered) {
        n = offered;
    }
    if (n > c->count) {
        n = c->count;
    }
    if (n == 0U) {
        return 0U;
    }

    if (n == c->count) {
        /* The whole chunk went out; an open one is closed by leaving. */
        f->head = (uint8_t)((f->head + 1U) % RAW_FIFO_CHUNKS);
        f->count--;
        return n;
    }

    memmove(c->samples, &c->samples[n], (c->count - n) * sizeof(uint16_t));
    c->count   -= (uint8_t)n;
    c->firstUs += (uint64_t)n * c->periodUs;
    return n;
}
