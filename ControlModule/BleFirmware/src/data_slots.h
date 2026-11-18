/*
 * DATA slots and credits, STM -> BLE direction. BLE_FIRMWARE_GUIDE.md §5.2,
 * §5.4.
 *
 * The nRF owns `n` slots of BLE_LINK_DATA_MAX bytes and grants one credit per
 * slot. Each DATA frame from the STM fills one slot. The slot is sent as one
 * or more notifications of at most the current MTU payload (the split), in
 * arrival order. Its credit is owed back to the STM when the completion of
 * its LAST notification arrived, not when the notify call returned. That
 * bounds what sits in the BLE stack to n frames.
 *
 *   slotsPut           a DATA frame arrived (connected, notifications on)
 *   slotsDropIncoming  a DATA frame arrived while it cannot be sent: dropped,
 *                      its credit is owed back at once
 *   slotsNextChunk     the next notification to make (does not change state)
 *   slotsChunkQueued   the notify call accepted it
 *   slotsChunkFailed   the notify call refused it: the rest of that slot is
 *                      dropped, the credit comes back once nothing of it is
 *                      in flight
 *   slotsChunkSent     a notification's completion callback
 *   slotsDropAll       disconnect: every slot freed, every credit owed
 *   slotsOwed          credits to send now in CREDIT{n}; slotsCreditSent
 *                      once the CREDIT frame went into the UART queue
 *   slotsHelloCredits  credits to grant in NRF_HELLO: the free slots now;
 *                      slotsHelloSent once the NRF_HELLO went out (it
 *                      covers every owed credit). Busy slots still return
 *                      theirs with CREDIT later, so the STM's count always
 *                      adds up to n.
 *
 * Invariant, with the STM following the rules: credits the STM holds +
 * owed + used slots = n.
 *
 * Pure logic: no Zephyr, no globals. One context only (the main loop).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ble_link_protocol.h"

/* Storage is reserved for SLOTS_MAX slots: the firmware build sets it to
 * CONFIG_TT_DATA_SLOTS (every slot is 250 B of RAM), the host tests to 8. */
#ifndef SLOTS_MAX
#define SLOTS_MAX 8U
#endif

typedef struct {
    uint8_t  data[BLE_LINK_DATA_MAX];
    uint16_t len;
    uint16_t queued;        /* bytes handed to notify so far                 */
    uint8_t  pending;       /* notifications made from it, not completed yet */
    bool     used;
    bool     failed;        /* a notify was refused: send nothing more of it */
} dataSlot_t;

typedef struct {
    dataSlot_t slot[SLOTS_MAX];
    uint8_t    n;
    uint8_t    order[SLOTS_MAX];   /* used slots, oldest first */
    uint8_t    orderCount;
    uint8_t    owed;               /* credits freed, not yet sent */
    uint8_t    inFlight;           /* notifications not yet completed, all slots */
    uint32_t   strayCompletions;   /* completions for a slot with none pending */
} dataSlots_t;

typedef struct {
    uint8_t        slot;
    const uint8_t *data;
    uint16_t       len;
} slotChunk_t;

/* n is clamped to 1..SLOTS_MAX. All slots free, nothing owed. */
void slotsInit(dataSlots_t *s, uint8_t n);

uint8_t slotsCount(const dataSlots_t *s);
uint8_t slotsFree(const dataSlots_t *s);
uint8_t slotsInFlight(const dataSlots_t *s);

/* Stores a DATA frame. false: len is 0 or > BLE_LINK_DATA_MAX, or the STM
 * held no credit for it (every free slot is covered by an owed credit, or
 * none is free); nothing changes then. */
bool slotsPut(dataSlots_t *s, const uint8_t *data, uint16_t len);

/* A DATA frame that cannot be sent: its credit is owed back at once. false
 * (nothing owed) if the STM held no credit for it. */
bool slotsDropIncoming(dataSlots_t *s);

/* The next chunk to notify: the oldest slot with unsent bytes, at most
 * maxChunk bytes. false if there is none, maxChunk is 0, or inFlight has
 * reached maxInFlight. */
bool slotsNextChunk(const dataSlots_t *s, uint16_t maxChunk, uint8_t maxInFlight, slotChunk_t *out);

void slotsChunkQueued(dataSlots_t *s, const slotChunk_t *c);
void slotsChunkFailed(dataSlots_t *s, const slotChunk_t *c);

/* false (and counted in strayCompletions) if the slot had nothing pending. */
bool slotsChunkSent(dataSlots_t *s, uint8_t slot);

void slotsDropAll(dataSlots_t *s);

uint8_t slotsOwed(const dataSlots_t *s);
void    slotsCreditSent(dataSlots_t *s, uint8_t count);
uint8_t slotsHelloCredits(const dataSlots_t *s);
void    slotsHelloSent(dataSlots_t *s);
