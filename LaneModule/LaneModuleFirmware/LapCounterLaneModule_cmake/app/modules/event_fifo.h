/*
 * The lane's event FIFO (EVENT_FIFO register) and raw-sample FIFO (RAW_FIFO
 * register), with the read and pop rules of lane_protocol.h.
 *
 * Pure logic: no HAL, no globals, host-tested (test/host_event_fifo). The
 * owner (sense_manager) pushes from the main loop inside an irq_guard; the
 * I3C target ISR (lane_bus, priority 2) builds read responses and completes
 * reads. The main loop cannot preempt that ISR, and the ISR only touches the
 * head while the producer only appends, so the guard on the push side is all
 * the locking needed.
 *
 * Event FIFO rules (guide §3.6, lane_protocol.h):
 *   - every event gets the next per-lane seq when it is pushed, kept or not:
 *     a dropped event leaves a gap in seq that the control module sees;
 *   - full: keep the oldest, drop the new one, count it (droppedEvents
 *     saturates at 0xFFFF) and latch the overflow flag;
 *   - a read offers a header and up to LANE_FIFO_READ_MAX records; the lane
 *     pops exactly the records whose bytes were all clocked out.
 *
 * Raw FIFO: samples are grouped in chunks of up to LANE_RAW_READ_MAX, each
 * with the time of its first sample and the period; sample i was taken at
 * firstUs + i × periodUs. A sample off that grid (by more than a quarter
 * period), a period change or a full chunk starts a new chunk. Full FIFO:
 * keep the oldest, drop new samples, and skip one chunkSeq so the gap shows.
 * A read offers the oldest chunk; the samples whose bytes were all clocked
 * out are popped, the rest stay with firstUs advanced and the same chunkSeq.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lane_status.h"
#include "lane_protocol.h"

#define EVENT_FIFO_DEPTH      32U   /* >= 32, guide §3.6 */
#define RAW_FIFO_CHUNKS       8U

_Static_assert(EVENT_FIFO_DEPTH <= 255U, "fifoCount is a u8 on the wire");

typedef struct {
    laneEvent_t ring[EVENT_FIFO_DEPTH];
    uint8_t     head;
    uint8_t     count;
    uint16_t    nextSeq;
    uint16_t    dropped;      /* saturating                                  */
    bool        overflowed;   /* latched until eventFifoClearCounters()      */
} eventFifo_t;

void eventFifoInit(eventFifo_t *f);

/*
 * Assigns ev->seq (the caller's copy is updated) and appends. Returns
 * LANE_ERR_FIFO_FULL when the event was dropped; its seq is spent anyway.
 */
laneStatus_e eventFifoPush(eventFifo_t *f, laneEvent_t *ev);

uint8_t  eventFifoCount(const eventFifo_t *f);
uint16_t eventFifoNextSeq(const eventFifo_t *f);
uint16_t eventFifoDropped(const eventFifo_t *f);
bool     eventFifoOverflowed(const eventFifo_t *f);

/* Zeroes the drop count and the overflow flag; queued events and seq stay. */
void eventFifoClearCounters(eventFifo_t *f);

/* Drops every queued event; seq continues (a reset lane would restart it). */
void eventFifoFlush(eventFifo_t *f);

/*
 * Writes the EVENT_FIFO read response into buf: laneFifoHeader_t, then up to
 * LANE_FIFO_READ_MAX records, oldest first. Returns the byte count (header +
 * count records, never padding); *offered gets count. cap must be at least
 * LANE_FIFO_READ_BYTES.
 */
uint32_t eventFifoBuildRead(const eventFifo_t *f, uint8_t *buf, uint32_t cap, uint8_t *offered);

/*
 * The read that offered `offered` records ended after clockedOut bytes. Pops
 * the records whose 16 bytes were all sent, at most `offered`; returns how
 * many.
 */
uint32_t eventFifoCompleteRead(eventFifo_t *f, uint8_t offered, uint32_t clockedOut);


typedef struct {
    uint64_t firstUs;
    uint32_t periodUs;
    uint16_t chunkSeq;
    uint8_t  count;
    bool     closed;          /* no more samples go into this chunk          */
    uint16_t samples[LANE_RAW_READ_MAX];
} rawChunk_t;

typedef struct {
    rawChunk_t chunks[RAW_FIFO_CHUNKS];
    uint8_t    head;
    uint8_t    count;
    uint16_t   nextChunkSeq;
    bool       dropping;      /* full: samples are being dropped             */
    uint32_t   droppedSamples;
} rawFifo_t;

void rawFifoInit(rawFifo_t *f);

/* One raw sample taken at tUs (mainboard µs) with the stream's period. */
void rawFifoPush(rawFifo_t *f, uint64_t tUs, uint32_t periodUs, uint16_t value);

/* Drops everything (RAW mode left); chunkSeq continues. */
void rawFifoFlush(rawFifo_t *f);

/* At least one full or closed chunk is waiting (the LANE_IBI_RAW condition). */
bool rawFifoHasChunk(const rawFifo_t *f);

uint32_t rawFifoDroppedSamples(const rawFifo_t *f);

/*
 * Writes the RAW_FIFO read response: laneRawHeader_t of the oldest chunk and
 * its samples (count 0 and no samples when empty). Returns the byte count;
 * *offered gets the sample count. cap must be at least LANE_RAW_READ_BYTES.
 */
uint32_t rawFifoBuildRead(const rawFifo_t *f, uint8_t *buf, uint32_t cap, uint8_t *offered);

/* Pops the samples whose 2 bytes were all sent, at most `offered`; returns
 * how many. */
uint32_t rawFifoCompleteRead(rawFifo_t *f, uint8_t offered, uint32_t clockedOut);
