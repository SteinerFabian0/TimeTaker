/*
 * BLE bind list. See bind_list.h.
 */

#include "bind_list.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static uint32_t bindListNextStamp(const bindList_t *list)
{
    uint32_t max = 0U;
    for (uint8_t i = 0U; i < list->count; i++) {
        if (list->entries[i].lastUsed > max) {
            max = list->entries[i].lastUsed;
        }
    }
    return max + 1U;
}

void bindListInit(bindList_t *list)
{
    memset(list, 0, sizeof(*list));
}

bool bindListIsZeroId(const uint8_t clientId[BIND_LIST_ID_BYTES])
{
    for (uint8_t i = 0U; i < BIND_LIST_ID_BYTES; i++) {
        if (clientId[i] != 0U) {
            return false;
        }
    }
    return true;
}

int bindListFind(const bindList_t *list, const uint8_t clientId[BIND_LIST_ID_BYTES])
{
    for (uint8_t i = 0U; i < list->count; i++) {
        if (memcmp(list->entries[i].clientId, clientId, BIND_LIST_ID_BYTES) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void bindListSetName(hpBindEntry_t *e, const char *name, uint16_t nameLen)
{
    memset(e->clientName, 0, sizeof(e->clientName));
    if (name != NULL) {
        uint16_t n = (nameLen < sizeof(e->clientName)) ? nameLen : (uint16_t)sizeof(e->clientName);
        memcpy(e->clientName, name, n);
    }
}

bindListAdd_e bindListAdd(bindList_t *list, const uint8_t clientId[BIND_LIST_ID_BYTES],
                          const char *name, uint16_t nameLen, uint8_t evictedId[BIND_LIST_ID_BYTES])
{
    if (bindListIsZeroId(clientId)) {
        return BIND_LIST_REFUSED;
    }

    const uint32_t stamp = bindListNextStamp(list);
    int idx = bindListFind(list, clientId);
    if (idx >= 0) {
        hpBindEntry_t *e = &list->entries[idx];
        bindListSetName(e, name, nameLen);
        e->lastUsed = stamp;
        return BIND_LIST_TOUCHED;
    }

    bindListAdd_e result = BIND_LIST_ADDED;
    uint8_t       slot   = list->count;
    if (list->count >= BIND_LIST_MAX) {
        slot = 0U;
        for (uint8_t i = 1U; i < list->count; i++) {
            if (list->entries[i].lastUsed < list->entries[slot].lastUsed) {
                slot = i;
            }
        }
        if (evictedId != NULL) {
            memcpy(evictedId, list->entries[slot].clientId, BIND_LIST_ID_BYTES);
        }
        result = BIND_LIST_EVICTED;
    } else {
        list->count++;
    }

    hpBindEntry_t *e = &list->entries[slot];
    memcpy(e->clientId, clientId, BIND_LIST_ID_BYTES);
    bindListSetName(e, name, nameLen);
    e->lastUsed = stamp;
    return result;
}

bool bindListTouch(bindList_t *list, const uint8_t clientId[BIND_LIST_ID_BYTES])
{
    int idx = bindListFind(list, clientId);
    if (idx < 0) {
        return false;
    }
    list->entries[idx].lastUsed = bindListNextStamp(list);
    return true;
}

bool bindListRemove(bindList_t *list, const uint8_t clientId[BIND_LIST_ID_BYTES])
{
    int idx = bindListFind(list, clientId);
    if (idx < 0) {
        return false;
    }
    for (uint8_t i = (uint8_t)idx; i + 1U < list->count; i++) {
        list->entries[i] = list->entries[i + 1U];
    }
    list->count--;
    memset(&list->entries[list->count], 0, sizeof(list->entries[0]));
    return true;
}

void bindListClear(bindList_t *list)
{
    bindListInit(list);
}

uint16_t bindListSerialize(const bindList_t *list, uint8_t *out, uint16_t cap)
{
    if (cap < BIND_LIST_SERIAL_BYTES) {
        return 0U;
    }
    memset(out, 0, BIND_LIST_SERIAL_BYTES);
    out[0] = list->count;
    memcpy(&out[4], list->entries, (size_t)list->count * HP_BIND_ENTRY_BYTES);
    return (uint16_t)BIND_LIST_SERIAL_BYTES;
}

bool bindListDeserialize(bindList_t *list, const uint8_t *in, uint16_t len)
{
    bindListInit(list);
    if (len < BIND_LIST_SERIAL_BYTES || in[0] > BIND_LIST_MAX) {
        return false;
    }

    bindList_t tmp;
    bindListInit(&tmp);
    for (uint8_t i = 0U; i < in[0]; i++) {
        hpBindEntry_t e;
        memcpy(&e, &in[4U + (uint16_t)i * HP_BIND_ENTRY_BYTES], sizeof(e));
        if (bindListIsZeroId(e.clientId) || bindListFind(&tmp, e.clientId) >= 0) {
            return false;
        }
        tmp.entries[tmp.count++] = e;
    }
    *list = tmp;
    return true;
}
