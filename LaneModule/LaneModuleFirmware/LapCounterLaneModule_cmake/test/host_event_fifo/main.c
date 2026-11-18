/*
 * Host test for event_fifo.c: the EVENT_FIFO and RAW_FIFO rules of
 * lane_protocol.h. Overflow keeps the oldest and counts the drops, seq is
 * spent even for a dropped event, and a read pops exactly the records whose
 * bytes were all clocked out.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "event_fifo.h"

static int failures;

static void check(const char *name, int passed)
{
    printf("%-58s %s\n", name, passed ? "ok" : "FAIL");
    failures += passed ? 0 : 1;
}

static eventFifo_t ef;
static rawFifo_t   rf;
static uint8_t     buf[128];

static laneEvent_t mkEvent(uint64_t t)
{
    laneEvent_t e;
    memset(&e, 0, sizeof(e));
    e.type     = LANE_EVT_CROSSING;
    e.tEntryUs = t;
    e.dwell10us = 2000U;
    e.depth    = 3000U;
    return e;
}

static laneEvent_t recordAt(uint32_t i)
{
    laneEvent_t e;
    memcpy(&e, &buf[sizeof(laneFifoHeader_t) + i * sizeof(laneEvent_t)], sizeof(e));
    return e;
}

static void testPushAndRead(void)
{
    uint8_t offered = 0U;
    eventFifoInit(&ef);

    uint32_t n = eventFifoBuildRead(&ef, buf, sizeof(buf), &offered);
    laneFifoHeader_t h;
    memcpy(&h, buf, sizeof(h));
    check("empty: header only, count 0", n == 4U && offered == 0U && h.count == 0U && h.remaining == 0U);

    for (uint32_t i = 0U; i < 6U; i++) {
        laneEvent_t e = mkEvent(1000U * i);
        (void)eventFifoPush(&ef, &e);
        if (i == 0U) {
            check("push: caller's copy gets seq 0", e.seq == 0U);
        }
    }
    check("push: count 6", eventFifoCount(&ef) == 6U);
    check("push: next seq 6", eventFifoNextSeq(&ef) == 6U);

    n = eventFifoBuildRead(&ef, buf, sizeof(buf), &offered);
    memcpy(&h, buf, sizeof(h));
    check("read: 4 records offered (LANE_FIFO_READ_MAX)", offered == 4U && h.count == 4U);
    check("read: remaining 2", h.remaining == 2U);
    check("read: exact length, no padding", n == LANE_FIFO_READ_BYTES);
    check("read: oldest first, seq in order",
          recordAt(0).seq == 0U && recordAt(3).seq == 3U && recordAt(3).tEntryUs == 3000U);
    check("read: building does not pop", eventFifoCount(&ef) == 6U);

    check("complete: full read pops 4", eventFifoCompleteRead(&ef, offered, n) == 4U);
    n = eventFifoBuildRead(&ef, buf, sizeof(buf), &offered);
    check("read 2: the remaining two, early end",
          offered == 2U && n == 4U + 2U * 16U && recordAt(0).seq == 4U);
    check("read 2: control reads 68 bytes, lane popped only 2",
          eventFifoCompleteRead(&ef, offered, LANE_FIFO_READ_BYTES) == 2U && eventFifoCount(&ef) == 0U);
}

static void testPartialReads(void)
{
    uint8_t offered = 0U;
    eventFifoInit(&ef);
    for (uint32_t i = 0U; i < 4U; i++) {
        laneEvent_t e = mkEvent(i);
        (void)eventFifoPush(&ef, &e);
    }

    (void)eventFifoBuildRead(&ef, buf, sizeof(buf), &offered);
    check("partial: header only clocked out pops 0", eventFifoCompleteRead(&ef, offered, 4U) == 0U);
    check("partial: half the header pops 0", eventFifoCompleteRead(&ef, offered, 2U) == 0U);
    check("partial: one byte short of a record pops 0", eventFifoCompleteRead(&ef, offered, 19U) == 0U);
    check("partial: exactly one record pops 1", eventFifoCompleteRead(&ef, offered, 20U) == 1U);

    (void)eventFifoBuildRead(&ef, buf, sizeof(buf), &offered);
    check("partial: next read starts at seq 1", offered == 3U && recordAt(0).seq == 1U);
    check("partial: 2.5 records pops 2", eventFifoCompleteRead(&ef, offered, 4U + 40U) == 2U);
    check("partial: one left, seq 3", eventFifoCount(&ef) == 1U);

    /* Never more than offered, even if the bus says more went out. */
    (void)eventFifoBuildRead(&ef, buf, sizeof(buf), &offered);
    laneEvent_t e = mkEvent(9U);
    (void)eventFifoPush(&ef, &e);
    check("partial: never pops more than offered",
          eventFifoCompleteRead(&ef, offered, LANE_FIFO_READ_BYTES) == 1U && eventFifoCount(&ef) == 1U);
}

static void testOverflow(void)
{
    eventFifoInit(&ef);
    laneStatus_e last = LANE_OK;
    for (uint32_t i = 0U; i < EVENT_FIFO_DEPTH; i++) {
        laneEvent_t e = mkEvent(i);
        last = eventFifoPush(&ef, &e);
    }
    check("overflow: depth pushes all fit", last == LANE_OK && eventFifoCount(&ef) == EVENT_FIFO_DEPTH);
    check("overflow: no flag yet", !eventFifoOverflowed(&ef) && eventFifoDropped(&ef) == 0U);

    laneEvent_t e = mkEvent(999U);
    last = eventFifoPush(&ef, &e);
    check("overflow: next push rejected FIFO_FULL", last == LANE_ERR_FIFO_FULL);
    check("overflow: dropped counted", eventFifoDropped(&ef) == 1U && eventFifoOverflowed(&ef));
    check("overflow: dropped event still spent a seq",
          e.seq == EVENT_FIFO_DEPTH && eventFifoNextSeq(&ef) == EVENT_FIFO_DEPTH + 1U);

    uint8_t offered = 0U;
    (void)eventFifoBuildRead(&ef, buf, sizeof(buf), &offered);
    check("overflow: oldest kept", recordAt(0).seq == 0U && recordAt(0).tEntryUs == 0U);
    (void)eventFifoCompleteRead(&ef, offered, LANE_FIFO_READ_BYTES);

    e = mkEvent(1000U);
    (void)eventFifoPush(&ef, &e);
    check("overflow: room again, seq gap shows the drop", e.seq == EVENT_FIFO_DEPTH + 1U);

    for (uint32_t i = 0U; i < 70000U; i++) {
        laneEvent_t x = mkEvent(i);
        (void)eventFifoPush(&ef, &x);
    }
    check("overflow: drop count saturates at 0xFFFF", eventFifoDropped(&ef) == 0xFFFFU);

    eventFifoClearCounters(&ef);
    check("clear: counters and flag reset, events kept",
          eventFifoDropped(&ef) == 0U && !eventFifoOverflowed(&ef) && eventFifoCount(&ef) == EVENT_FIFO_DEPTH);
    eventFifoFlush(&ef);
    check("flush: empty, seq continues", eventFifoCount(&ef) == 0U && eventFifoNextSeq(&ef) != 0U);
}

/* The FIFO is deeper than the u8 counts on the wire: `remaining` saturates,
 * and the ring index runs past 255 without losing order. */
static void testDeepFifo(void)
{
    uint8_t offered = 0U;
    laneFifoHeader_t h;
    eventFifoInit(&ef);
    for (uint32_t i = 0U; i < 300U; i++) {
        laneEvent_t e = mkEvent(i);
        (void)eventFifoPush(&ef, &e);
    }
    (void)eventFifoBuildRead(&ef, buf, sizeof(buf), &offered);
    memcpy(&h, buf, sizeof(h));
    check("deep: 300 queued, remaining saturates at 255",
          eventFifoCount(&ef) == 300U && h.count == LANE_FIFO_READ_MAX && h.remaining == LANE_FIFO_COUNT_MAX);

    eventFifoInit(&ef);
    for (uint32_t i = 0U; i < LANE_FIFO_COUNT_MAX - 1U + LANE_FIFO_READ_MAX; i++) {
        laneEvent_t e = mkEvent(i);
        (void)eventFifoPush(&ef, &e);
    }
    (void)eventFifoBuildRead(&ef, buf, sizeof(buf), &offered);
    memcpy(&h, buf, sizeof(h));
    check("deep: below the cap remaining is exact", h.remaining == LANE_FIFO_COUNT_MAX - 1U);

    /* Fill, read half, refill: the tail wraps round the whole ring. */
    eventFifoInit(&ef);
    for (uint32_t i = 0U; i < EVENT_FIFO_DEPTH; i++) {
        laneEvent_t e = mkEvent(i);
        (void)eventFifoPush(&ef, &e);
    }
    for (uint32_t i = 0U; i < EVENT_FIFO_DEPTH / 2U; i += LANE_FIFO_READ_MAX) {
        (void)eventFifoBuildRead(&ef, buf, sizeof(buf), &offered);
        (void)eventFifoCompleteRead(&ef, offered, LANE_FIFO_READ_BYTES);
    }
    for (uint32_t i = 0U; i < EVENT_FIFO_DEPTH / 2U; i++) {
        laneEvent_t e = mkEvent(EVENT_FIFO_DEPTH + i);
        (void)eventFifoPush(&ef, &e);
    }
    bool inOrder = eventFifoCount(&ef) == EVENT_FIFO_DEPTH && eventFifoDropped(&ef) == 0U;
    uint32_t expect = EVENT_FIFO_DEPTH / 2U;
    while (inOrder && eventFifoCount(&ef) > 0U) {
        (void)eventFifoBuildRead(&ef, buf, sizeof(buf), &offered);
        for (uint32_t k = 0U; k < offered; k++) {
            inOrder = inOrder && recordAt(k).seq == (uint16_t)expect && recordAt(k).tEntryUs == expect;
            expect++;
        }
        (void)eventFifoCompleteRead(&ef, offered, LANE_FIFO_READ_BYTES);
    }
    check("deep: a full ring wrapped round reads back in order",
          inOrder && expect == EVENT_FIFO_DEPTH + EVENT_FIFO_DEPTH / 2U);
}

static void testSeqWrap(void)
{
    eventFifoInit(&ef);
    ef.nextSeq = 0xFFFFU;
    laneEvent_t a = mkEvent(1U);
    laneEvent_t b = mkEvent(2U);
    (void)eventFifoPush(&ef, &a);
    (void)eventFifoPush(&ef, &b);
    check("seq: wraps 0xFFFF -> 0", a.seq == 0xFFFFU && b.seq == 0U);
}

static void testSmallBuffer(void)
{
    uint8_t offered = 7U;
    eventFifoInit(&ef);
    laneEvent_t e = mkEvent(1U);
    (void)eventFifoPush(&ef, &e);
    check("read: buffer below LANE_FIFO_READ_BYTES refused",
          eventFifoBuildRead(&ef, buf, LANE_FIFO_READ_BYTES - 1U, &offered) == 0U && offered == 0U);
}


/* --- Raw FIFO ------------------------------------------------------------- */

static laneRawHeader_t rawHeader(void)
{
    laneRawHeader_t h;
    memcpy(&h, buf, sizeof(h));
    return h;
}

static uint16_t rawSample(uint32_t i)
{
    uint16_t v;
    memcpy(&v, &buf[sizeof(laneRawHeader_t) + 2U * i], 2U);
    return v;
}

static void testRawChunks(void)
{
    uint8_t offered = 0U;
    rawFifoInit(&rf);

    uint32_t n = rawFifoBuildRead(&rf, buf, sizeof(buf), &offered);
    check("raw empty: header only, count 0", n == 16U && offered == 0U && rawHeader().count == 0U);

    for (uint32_t i = 0U; i < 40U; i++) {
        rawFifoPush(&rf, 5000U + 1000U * i, 1000U, (uint16_t)(100U + i));
    }
    check("raw: a full chunk is waiting", rawFifoHasChunk(&rf));

    n = rawFifoBuildRead(&rf, buf, sizeof(buf), &offered);
    laneRawHeader_t h = rawHeader();
    check("raw: first chunk 32 samples", offered == 32U && h.count == 32U && n == LANE_RAW_READ_BYTES);
    check("raw: firstUs and period", h.firstUs == 5000U && h.periodUs == 1000U && h.chunkSeq == 0U);
    check("raw: samples in order", rawSample(0) == 100U && rawSample(31) == 131U);

    check("raw: full read pops the chunk", rawFifoCompleteRead(&rf, offered, n) == 32U);
    n = rawFifoBuildRead(&rf, buf, sizeof(buf), &offered);
    h = rawHeader();
    check("raw: second chunk (open) 8 samples, seq 1",
          offered == 8U && h.chunkSeq == 1U && h.firstUs == 37000U);
    check("raw: an open partial chunk is no IBI chunk", !rawFifoHasChunk(&rf));

    /* Partial read: 3.5 samples clocked out pops 3, firstUs advances. */
    check("raw partial: pops 3", rawFifoCompleteRead(&rf, offered, 16U + 7U) == 3U);
    (void)rawFifoBuildRead(&rf, buf, sizeof(buf), &offered);
    h = rawHeader();
    check("raw partial: rest keeps chunkSeq, firstUs advanced",
          offered == 5U && h.chunkSeq == 1U && h.firstUs == 40000U && rawSample(0) == 135U);

    /* The open chunk keeps growing on its grid after the partial pop. */
    rawFifoPush(&rf, 45000U, 1000U, 140U);
    (void)rawFifoBuildRead(&rf, buf, sizeof(buf), &offered);
    check("raw partial: still appends on the grid", offered == 6U && rawSample(5) == 140U);
}

static void testRawGridBreaks(void)
{
    uint8_t offered = 0U;
    rawFifoInit(&rf);
    rawFifoPush(&rf, 1000U, 1000U, 1U);
    rawFifoPush(&rf, 2200U, 1000U, 2U);   /* 200 us late: within a quarter */
    rawFifoPush(&rf, 5000U, 1000U, 3U);   /* a missed sample: new chunk */
    (void)rawFifoBuildRead(&rf, buf, sizeof(buf), &offered);
    check("raw grid: jitter within P/4 stays in the chunk", offered == 2U);
    check("raw grid: a gap closes the chunk", rawFifoHasChunk(&rf));
    (void)rawFifoCompleteRead(&rf, offered, LANE_RAW_READ_BYTES);
    (void)rawFifoBuildRead(&rf, buf, sizeof(buf), &offered);
    check("raw grid: new chunk at the gap", offered == 1U && rawHeader().firstUs == 5000U);

    rawFifoPush(&rf, 15000U, 10000U, 4U); /* period change (holdoff watch) */
    (void)rawFifoBuildRead(&rf, buf, sizeof(buf), &offered);
    check("raw grid: a period change starts a new chunk", offered == 1U && rawHeader().periodUs == 1000U);
}

static void testRawOverflow(void)
{
    uint8_t offered = 0U;
    rawFifoInit(&rf);
    uint32_t total = RAW_FIFO_CHUNKS * LANE_RAW_READ_MAX;
    for (uint32_t i = 0U; i < total + 10U; i++) {
        rawFifoPush(&rf, 1000U * i, 1000U, (uint16_t)i);
    }
    check("raw overflow: drops counted", rawFifoDroppedSamples(&rf) == 10U);
    (void)rawFifoBuildRead(&rf, buf, sizeof(buf), &offered);
    check("raw overflow: oldest kept", rawHeader().chunkSeq == 0U && rawSample(0) == 0U);
    (void)rawFifoCompleteRead(&rf, offered, LANE_RAW_READ_BYTES);

    rawFifoPush(&rf, 1000U * (total + 20U), 1000U, 7U);
    for (uint32_t i = 1U; i < RAW_FIFO_CHUNKS; i++) {
        (void)rawFifoBuildRead(&rf, buf, sizeof(buf), &offered);
        (void)rawFifoCompleteRead(&rf, offered, LANE_RAW_READ_BYTES);
    }
    (void)rawFifoBuildRead(&rf, buf, sizeof(buf), &offered);
    check("raw overflow: one chunkSeq skipped for the gap",
          rawHeader().chunkSeq == RAW_FIFO_CHUNKS + 1U && rawSample(0) == 7U);

    rawFifoFlush(&rf);
    (void)rawFifoBuildRead(&rf, buf, sizeof(buf), &offered);
    check("raw flush: empty", offered == 0U);
}

int main(void)
{
    testPushAndRead();
    testPartialReads();
    testOverflow();
    testDeepFifo();
    testSeqWrap();
    testSmallBuffer();
    testRawChunks();
    testRawGridBreaks();
    testRawOverflow();

    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
