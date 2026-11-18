/*
 * DATA slots and credits. See data_slots.h.
 */

#include "data_slots.h"

#include <string.h>

static void slotRelease(dataSlots_t *s, uint8_t idx)
{
    dataSlot_t *sl = &s->slot[idx];
    sl->used    = false;
    sl->failed  = false;
    sl->len     = 0U;
    sl->queued  = 0U;
    sl->pending = 0U;

    uint8_t w = 0U;
    for (uint8_t i = 0U; i < s->orderCount; i++) {
        if (s->order[i] != idx) {
            s->order[w++] = s->order[i];
        }
    }
    s->orderCount = w;
    s->owed++;
}

/* Frees the slot once nothing more will be sent from it and nothing of it is
 * in flight. */
static void slotMaybeRelease(dataSlots_t *s, uint8_t idx)
{
    const dataSlot_t *sl = &s->slot[idx];
    if (sl->used && sl->pending == 0U && (sl->failed || sl->queued >= sl->len)) {
        slotRelease(s, idx);
    }
}

void slotsInit(dataSlots_t *s, uint8_t n)
{
    memset(s, 0, sizeof(*s));
    if (n < 1U) {
        n = 1U;
    }
    if (n > SLOTS_MAX) {
        n = SLOTS_MAX;
    }
    s->n = n;
}

uint8_t slotsCount(const dataSlots_t *s)
{
    return s->n;
}

uint8_t slotsFree(const dataSlots_t *s)
{
    return (uint8_t)(s->n - s->orderCount);
}

uint8_t slotsInFlight(const dataSlots_t *s)
{
    return s->inFlight;
}

bool slotsPut(dataSlots_t *s, const uint8_t *data, uint16_t len)
{
    if (len == 0U || len > BLE_LINK_DATA_MAX || (uint16_t)(s->orderCount + s->owed) >= s->n) {
        return false;
    }
    for (uint8_t i = 0U; i < s->n; i++) {
        dataSlot_t *sl = &s->slot[i];
        if (!sl->used) {
            memcpy(sl->data, data, len);
            sl->len     = len;
            sl->queued  = 0U;
            sl->pending = 0U;
            sl->failed  = false;
            sl->used    = true;
            s->order[s->orderCount++] = i;
            return true;
        }
    }
    return false;                       /* not reached: orderCount < n */
}

bool slotsDropIncoming(dataSlots_t *s)
{
    /* The STM holds (n - used - owed) credits; a frame beyond those was sent
     * without one and earns nothing back. */
    if (s->owed >= slotsFree(s)) {
        return false;
    }
    s->owed++;
    return true;
}

bool slotsNextChunk(const dataSlots_t *s, uint16_t maxChunk, uint8_t maxInFlight, slotChunk_t *out)
{
    if (maxChunk == 0U || s->inFlight >= maxInFlight) {
        return false;
    }
    for (uint8_t i = 0U; i < s->orderCount; i++) {
        const uint8_t     idx = s->order[i];
        const dataSlot_t *sl  = &s->slot[idx];
        if (sl->failed || sl->queued >= sl->len) {
            continue;
        }
        const uint16_t left = (uint16_t)(sl->len - sl->queued);
        out->slot = idx;
        out->data = &sl->data[sl->queued];
        out->len  = (left < maxChunk) ? left : maxChunk;
        return true;
    }
    return false;
}

void slotsChunkQueued(dataSlots_t *s, const slotChunk_t *c)
{
    if (c->slot >= s->n || !s->slot[c->slot].used) {
        return;
    }
    dataSlot_t *sl = &s->slot[c->slot];
    sl->queued = (uint16_t)(sl->queued + c->len);
    sl->pending++;
    s->inFlight++;
}

void slotsChunkFailed(dataSlots_t *s, const slotChunk_t *c)
{
    if (c->slot >= s->n || !s->slot[c->slot].used) {
        return;
    }
    s->slot[c->slot].failed = true;
    slotMaybeRelease(s, c->slot);
}

bool slotsChunkSent(dataSlots_t *s, uint8_t slot)
{
    if (slot >= s->n || !s->slot[slot].used || s->slot[slot].pending == 0U) {
        s->strayCompletions++;
        return false;
    }
    s->slot[slot].pending--;
    if (s->inFlight > 0U) {
        s->inFlight--;
    }
    slotMaybeRelease(s, slot);
    return true;
}

void slotsDropAll(dataSlots_t *s)
{
    while (s->orderCount > 0U) {
        slotRelease(s, s->order[0]);
    }
    s->inFlight = 0U;
}

uint8_t slotsOwed(const dataSlots_t *s)
{
    return s->owed;
}

void slotsCreditSent(dataSlots_t *s, uint8_t count)
{
    s->owed = (count >= s->owed) ? 0U : (uint8_t)(s->owed - count);
}

uint8_t slotsHelloCredits(const dataSlots_t *s)
{
    return slotsFree(s);
}

void slotsHelloSent(dataSlots_t *s)
{
    s->owed = 0U;
}
