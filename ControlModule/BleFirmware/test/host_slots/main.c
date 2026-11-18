/*
 * Host test for data_slots.c: DATA slots, credits and the MTU split.
 * BLE_FIRMWARE_GUIDE.md §5.2, §5.4.
 *
 * A model STM holds credits the way the control firmware does (set on
 * NRF_HELLO, +n on CREDIT, -1 per DATA frame) and every scenario checks the
 * invariant: STM credits + owed + used slots == n. Rejection cases break one
 * rule each.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../check.h"
#include "data_slots.h"

#define N 4U

static dataSlots_t s;
static int         stmCredits;      /* the model STM's counter */

static uint8_t usedSlots(void)
{
    return (uint8_t)(slotsCount(&s) - slotsFree(&s));
}

static bool invariant(void)
{
    return stmCredits + slotsOwed(&s) + usedSlots() == (int)slotsCount(&s);
}

static void hello(void)
{
    stmCredits = slotsHelloCredits(&s);
    slotsHelloSent(&s);
}

static void flushCredits(void)
{
    const uint8_t n = slotsOwed(&s);
    stmCredits += n;
    slotsCreditSent(&s, n);
}

/* The STM sends a DATA frame of len bytes, filled with `fill`. */
static bool stmSend(uint16_t len, uint8_t fill)
{
    uint8_t buf[BLE_LINK_DATA_MAX + 8];
    memset(buf, fill, sizeof(buf));
    stmCredits--;
    return slotsPut(&s, buf, len);
}

/* Notifies everything possible; records chunk lengths and slots. */
static int  chunkLen[64];
static int  chunkSlot[64];
static int  chunkCount;
static bool chunkData[64];          /* chunk bytes all equal the expected fill */

static void pump(uint16_t mtuPayload, uint8_t maxInFlight, uint8_t expectFill)
{
    slotChunk_t c;
    while (slotsNextChunk(&s, mtuPayload, maxInFlight, &c)) {
        bool same = true;
        for (uint16_t i = 0; i < c.len; i++) {
            same = same && (c.data[i] == expectFill);
        }
        if (chunkCount < 64) {
            chunkLen[chunkCount]  = c.len;
            chunkSlot[chunkCount] = c.slot;
            chunkData[chunkCount] = same;
            chunkCount++;
        }
        slotsChunkQueued(&s, &c);
    }
}

static void fresh(void)
{
    slotsInit(&s, N);
    stmCredits = 0;
    chunkCount = 0;
    hello();
}

static void basics(void)
{
    fresh();
    check("hello grants n credits", stmCredits == (int)N && invariant());

    /* Take: one slot per frame. */
    check("put 1", stmSend(100, 0xA1));
    check("one slot used", usedSlots() == 1 && invariant());

    /* Split to MTU 23 (20 B payload): 100 B -> 5 notifications. */
    pump(20, 8, 0xA1);
    check("split 100 B at 20 B: 5 chunks of 20", chunkCount == 5 && chunkLen[0] == 20 && chunkLen[4] == 20);
    check("chunks carry the frame's bytes", chunkData[0] && chunkData[4]);
    check("in flight 5", slotsInFlight(&s) == 5);

    /* Credit only after the LAST completion, not when queued. */
    for (int i = 0; i < 4; i++) {
        slotsChunkSent(&s, (uint8_t)chunkSlot[i]);
    }
    check("4 of 5 completions: credit not yet owed", slotsOwed(&s) == 0 && usedSlots() == 1);
    slotsChunkSent(&s, (uint8_t)chunkSlot[4]);
    check("last completion: slot free, credit owed", slotsOwed(&s) == 1 && usedSlots() == 0 && invariant());
    flushCredits();
    check("CREDIT{1} returns the credit", stmCredits == (int)N && invariant());

    /* A remainder chunk. */
    fresh();
    stmSend(BLE_LINK_DATA_MAX, 0xB2);
    pump(20, 32, 0xB2);
    check("244 B at 20 B: 13 chunks, last 4", chunkCount == 13 && chunkLen[12] == 4);

    /* Full MTU: one notification per frame. */
    fresh();
    stmSend(BLE_LINK_DATA_MAX, 0xC3);
    pump(BLE_LINK_DATA_MAX, 8, 0xC3);
    check("244 B at MTU 247: one chunk", chunkCount == 1 && chunkLen[0] == (int)BLE_LINK_DATA_MAX);
}

static void ordering(void)
{
    /* Frames go out in arrival order, even when slots are reused. */
    fresh();
    stmSend(30, 0x01);
    stmSend(30, 0x02);
    pump(20, 8, 0x01);                          /* only checks fill of the first */
    check("order: chunks of frame 1 before frame 2",
          chunkCount == 4 && chunkSlot[0] == chunkSlot[1] && chunkSlot[2] == chunkSlot[3]
          && chunkSlot[0] != chunkSlot[2] && chunkData[0] && chunkData[1] && !chunkData[2]);

    /* Complete frame 1, then a new frame lands in its slot: it must still go
     * after frame 2's (already queued) chunks, and after any unsent ones. */
    fresh();
    stmSend(40, 0x11);                          /* slot A */
    stmSend(40, 0x22);                          /* slot B */
    pump(20, 2, 0x11);                          /* in-flight cap 2: only A's 2 chunks */
    check("in-flight cap holds back B", chunkCount == 2 && slotsInFlight(&s) == 2);
    slotsChunkSent(&s, (uint8_t)chunkSlot[0]);
    slotsChunkSent(&s, (uint8_t)chunkSlot[1]);  /* A free */
    const int slotA = chunkSlot[0];
    flushCredits();
    stmSend(40, 0x33);                          /* lands in A */
    chunkCount = 0;
    pump(20, 8, 0x22);
    check("reused slot goes after the older frame",
          chunkCount == 4 && chunkData[0] && chunkData[1] && chunkSlot[2] == slotA && !chunkData[2]);
    check("invariant after reuse", invariant());
}

static void rejections(void)
{
    /* Each case breaks one rule. */
    fresh();
    check("reject: empty DATA frame", !slotsPut(&s, (const uint8_t *)"", 0) && usedSlots() == 0);

    fresh();
    {
        uint8_t big[BLE_LINK_DATA_MAX + 1];
        memset(big, 0, sizeof(big));
        check("reject: 245 B DATA frame", !slotsPut(&s, big, BLE_LINK_DATA_MAX + 1) && usedSlots() == 0);
        check("accept: 244 B DATA frame", slotsPut(&s, big, BLE_LINK_DATA_MAX));
    }

    /* No credit: all n slots used. */
    fresh();
    for (unsigned i = 0; i < N; i++) {
        stmSend(10, 0x40);
    }
    check("n frames fill n slots", slotsFree(&s) == 0 && stmCredits == 0);
    check("reject: frame n+1 without a credit", !slotsPut(&s, (const uint8_t *)"x", 1));

    /* No credit: a slot is free but its credit is owed, not yet returned. */
    fresh();
    for (unsigned i = 0; i < N; i++) {
        stmSend(10, 0x50);
    }
    pump(20, 8, 0x50);
    slotsChunkSent(&s, (uint8_t)chunkSlot[0]);
    check("one slot free, credit owed", slotsFree(&s) == 1 && slotsOwed(&s) == 1);
    check("reject: frame on an owed (unsent) credit", !slotsPut(&s, (const uint8_t *)"x", 1));
    flushCredits();
    check("accept: once the CREDIT went out", stmSend(1, 0x51));

    /* Drop-and-credit while it cannot be sent, only for frames with a
     * credit. */
    fresh();
    stmCredits--;
    check("drop while disconnected: credit owed", slotsDropIncoming(&s) && slotsOwed(&s) == 1);
    check("drop while disconnected: invariant", invariant());
    fresh();
    for (unsigned i = 0; i < N; i++) {
        stmCredits--;
        slotsDropIncoming(&s);
    }
    check("reject: drop-and-credit beyond the STM's credits", !slotsDropIncoming(&s) && slotsOwed(&s) == N);

    /* Stray completions. */
    fresh();
    check("reject: completion with nothing pending", !slotsChunkSent(&s, 0) && s.strayCompletions == 1);
    check("reject: completion for a slot out of range", !slotsChunkSent(&s, SLOTS_MAX + 1));

    /* Nothing to send: no chunk; zero MTU: no chunk. */
    fresh();
    slotChunk_t c;
    check("no data: no chunk", !slotsNextChunk(&s, 20, 8, &c));
    stmSend(10, 0x60);
    check("zero chunk size: no chunk", !slotsNextChunk(&s, 0, 8, &c));
    check("in-flight cap 0: no chunk", !slotsNextChunk(&s, 20, 0, &c));
}

static void failures(void)
{
    /* A refused notify drops the rest of the slot; the credit comes back
     * only when its earlier chunks are no longer in flight. */
    fresh();
    stmSend(60, 0x70);
    slotChunk_t c;
    slotsNextChunk(&s, 20, 8, &c);
    slotsChunkQueued(&s, &c);                   /* chunk 1 in flight */
    slotsNextChunk(&s, 20, 8, &c);
    slotsChunkFailed(&s, &c);                   /* chunk 2 refused */
    check("failed: no more chunks from that slot", !slotsNextChunk(&s, 20, 8, &c));
    check("failed: slot held while chunk 1 in flight", usedSlots() == 1 && slotsOwed(&s) == 0);
    slotsChunkSent(&s, 0);
    check("failed: freed and credited after chunk 1 completes", usedSlots() == 0 && slotsOwed(&s) == 1);
    check("failed: invariant", invariant());

    /* Refused at the first chunk: freed at once. */
    fresh();
    stmSend(60, 0x71);
    slotsNextChunk(&s, 20, 8, &c);
    slotsChunkFailed(&s, &c);
    check("failed first chunk: freed and credited at once", usedSlots() == 0 && slotsOwed(&s) == 1);
}

static void disconnect(void)
{
    /* Disconnect: every slot freed, every credit owed, nothing in flight. */
    fresh();
    stmSend(100, 0x80);
    stmSend(50, 0x81);
    pump(20, 3, 0x80);
    slotsDropAll(&s);
    check("drop all: no slot used", usedSlots() == 0);
    check("drop all: nothing in flight", slotsInFlight(&s) == 0);
    check("drop all: both credits owed", slotsOwed(&s) == 2 && invariant());
    flushCredits();
    check("drop all: STM back to n", stmCredits == (int)N);

    /* Hello while slots are busy: grant the free ones; the busy one
     * returns its credit later. */
    fresh();
    stmSend(40, 0x90);
    pump(20, 8, 0x90);
    stmSend(10, 0x91);
    slotsDropAll(&s);                            /* both freed and owed... */
    stmSend(40, 0x92);                           /* ...one used again */
    pump(20, 8, 0x92);
    hello();
    check("hello: credits = free slots", stmCredits == (int)(N - 1) && slotsOwed(&s) == 0);
    check("hello: invariant", invariant());
    slotsChunkSent(&s, (uint8_t)chunkSlot[chunkCount - 1]);
    slotsChunkSent(&s, (uint8_t)chunkSlot[chunkCount - 2]);
    flushCredits();
    check("hello: busy slot's credit returns afterwards", stmCredits == (int)N && invariant());

    /* Init clamps. */
    slotsInit(&s, 0);
    check("init: n clamped to >= 1", slotsCount(&s) == 1);
    slotsInit(&s, 200);
    check("init: n clamped to SLOTS_MAX", slotsCount(&s) == SLOTS_MAX);
}

int main(void)
{
    basics();
    ordering();
    rejections();
    failures();
    disconnect();
    return checkSummary("host_slots");
}
