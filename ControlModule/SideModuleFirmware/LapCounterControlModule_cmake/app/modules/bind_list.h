/*
 * BLE bind list: the web app clients allowed to use the BLE transport.
 *
 * Binding is done at the application layer (SYSTEM_ARCHITECTURE §9.5): the
 * web app carries a random 128-bit clientId, and this list holds up to
 * BIND_LIST_MAX of them with a name and an LRU stamp. A full list evicts the
 * least recently used entry. The all-zero clientId is never a client (it is
 * the "clear all" wildcard of BIND_REMOVE), so it can never be added.
 *
 * The bind POLICY (window, timers, who may add) is host_manager's. This module
 * only keeps the list consistent and serialises it for settings_store.
 *
 * Pure logic: no HAL, no globals; storage is the caller's. Host-tested
 * (test/host_bind_list). Main loop only.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "host_protocol.h"

#define BIND_LIST_MAX        8U
#define BIND_LIST_ID_BYTES   16U
#define BIND_LIST_NAME_BYTES 24U

/* Serialised form: count u8, 3 reserved bytes, then count x hpBindEntry_t. */
#define BIND_LIST_SERIAL_BYTES  (4U + BIND_LIST_MAX * HP_BIND_ENTRY_BYTES)

typedef struct {
    hpBindEntry_t entries[BIND_LIST_MAX];
    uint8_t       count;
} bindList_t;

typedef enum {
    BIND_LIST_ADDED   = 0,   /* new entry, there was room            */
    BIND_LIST_TOUCHED = 1,   /* already listed: name and LRU updated */
    BIND_LIST_EVICTED = 2,   /* new entry, the LRU entry made room   */
    BIND_LIST_REFUSED = 3,   /* all-zero clientId                    */
} bindListAdd_e;

void bindListInit(bindList_t *list);

/* Index of the entry with this clientId, or -1. */
int bindListFind(const bindList_t *list, const uint8_t clientId[BIND_LIST_ID_BYTES]);

/* Adds (or touches) a client; name is zero-padded to 24 bytes (may be NULL).
 * evictedId (may be NULL) receives the evicted clientId on BIND_LIST_EVICTED. */
bindListAdd_e bindListAdd(bindList_t *list, const uint8_t clientId[BIND_LIST_ID_BYTES],
                          const char *name, uint16_t nameLen, uint8_t evictedId[BIND_LIST_ID_BYTES]);

/* Marks the client as just used. Returns false if it is not listed. */
bool bindListTouch(bindList_t *list, const uint8_t clientId[BIND_LIST_ID_BYTES]);

/* Removes one client. Returns false if it was not listed. */
bool bindListRemove(bindList_t *list, const uint8_t clientId[BIND_LIST_ID_BYTES]);

void bindListClear(bindList_t *list);

/* Whether every byte of id is zero. */
bool bindListIsZeroId(const uint8_t clientId[BIND_LIST_ID_BYTES]);

/* Writes BIND_LIST_SERIAL_BYTES (unused entries zeroed). Returns the bytes
 * written, 0 if cap is too small. */
uint16_t bindListSerialize(const bindList_t *list, uint8_t *out, uint16_t cap);

/* Rebuilds a list from serialised bytes. Refuses (returns false, list left
 * empty) a short buffer, a count above the maximum, a zero id or a duplicate. */
bool bindListDeserialize(bindList_t *list, const uint8_t *in, uint16_t len);
