/*
 * Host test for bind_list.c: the BLE bind list (CONTROL_FIRMWARE_GUIDE §5.4).
 * Lookup, add/touch, LRU eviction at 8, remove, clear, the zero-id rule, and
 * the serialised form settings_store keeps, with every refusal of the
 * deserialiser broken one rule at a time.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "bind_list.h"
#include "../test_check.h"

static void id(uint8_t out[16], uint8_t n)
{
    memset(out, 0, 16);
    out[0] = n;
    out[15] = (uint8_t)(0xA0U + n);
}

static void testAddFind(void)
{
    bindList_t l;
    bindListInit(&l);
    uint8_t a[16], b[16];
    id(a, 1);
    id(b, 2);
    check("add: new client -> ADDED, findable at 0",
          bindListAdd(&l, a, "Laptop", 6, NULL) == BIND_LIST_ADDED && bindListFind(&l, a) == 0 && l.count == 1);
    check("add: name stored zero-padded",
          memcmp(l.entries[0].clientName, "Laptop\0\0", 8) == 0);
    check("add: unknown client not found", bindListFind(&l, b) == -1);
    uint32_t stampA = l.entries[0].lastUsed;
    check("add: second client gets a newer stamp",
          bindListAdd(&l, b, "Tab", 3, NULL) == BIND_LIST_ADDED && l.entries[1].lastUsed > stampA);
    check("add: same client again -> TOUCHED, no new entry, newest stamp",
          bindListAdd(&l, a, "Laptop2", 7, NULL) == BIND_LIST_TOUCHED && l.count == 2
          && l.entries[0].lastUsed > l.entries[1].lastUsed && l.entries[0].clientName[6] == '2');
    uint8_t z[16] = { 0 };
    check("add: the all-zero id is refused", bindListAdd(&l, z, "x", 1, NULL) == BIND_LIST_REFUSED && l.count == 2);
    char longName[40];
    memset(longName, 'n', sizeof(longName));
    uint8_t c[16];
    id(c, 3);
    bindListAdd(&l, c, longName, sizeof(longName), NULL);
    check("add: a long name is cut at 24 bytes",
          l.entries[2].clientName[23] == 'n' && l.count == 3);
}

static void testLru(void)
{
    bindList_t l;
    bindListInit(&l);
    uint8_t ids[9][16];
    for (uint8_t i = 0; i < 9; i++) {
        id(ids[i], (uint8_t)(i + 1));
    }
    for (uint8_t i = 0; i < 8; i++) {
        bindListAdd(&l, ids[i], "c", 1, NULL);
    }
    bindListTouch(&l, ids[0]);              /* 0 is now the newest; 1 is the LRU */
    uint8_t ev[16] = { 0 };

    bindListAdd_e r = bindListAdd(&l, ids[8], "new", 3, ev);
    check("lru: 9th client evicts the least recently used",
          r == BIND_LIST_EVICTED && memcmp(ev, ids[1], 16) == 0 && l.count == 8
          && bindListFind(&l, ids[1]) == -1 && bindListFind(&l, ids[8]) >= 0 && bindListFind(&l, ids[0]) >= 0);
    check("lru: touch of an unknown client -> false", !bindListTouch(&l, ids[1]));
}

static void testRemove(void)
{
    bindList_t l;
    bindListInit(&l);
    uint8_t a[16], b[16], c[16];
    id(a, 1); id(b, 2); id(c, 3);
    bindListAdd(&l, a, "a", 1, NULL);
    bindListAdd(&l, b, "b", 1, NULL);
    bindListAdd(&l, c, "c", 1, NULL);
    check("remove: listed client removed, the rest kept in order",
          bindListRemove(&l, b) && l.count == 2 && bindListFind(&l, a) == 0 && bindListFind(&l, c) == 1);
    check("remove: unlisted client -> false", !bindListRemove(&l, b) && l.count == 2);
    bindListClear(&l);
    check("clear: empty", l.count == 0 && bindListFind(&l, a) == -1);
}

static void testSerial(void)
{
    bindList_t l, back;
    bindListInit(&l);
    uint8_t a[16], b[16];
    id(a, 1); id(b, 2);
    bindListAdd(&l, a, "a", 1, NULL);
    bindListAdd(&l, b, "b", 1, NULL);
    uint8_t buf[BIND_LIST_SERIAL_BYTES];
    uint8_t small[10];
    check("serial: refuses a short buffer", bindListSerialize(&l, small, sizeof(small)) == 0);
    check("serial: writes the fixed size", bindListSerialize(&l, buf, sizeof(buf)) == BIND_LIST_SERIAL_BYTES);
    check("serial: round trip", bindListDeserialize(&back, buf, sizeof(buf)) && back.count == 2
          && memcmp(&back.entries[0], &l.entries[0], sizeof(hpBindEntry_t)) == 0
          && memcmp(&back.entries[1], &l.entries[1], sizeof(hpBindEntry_t)) == 0);

    check("deserial: short buffer refused", !bindListDeserialize(&back, buf, sizeof(buf) - 1U) && back.count == 0);
    uint8_t bad[BIND_LIST_SERIAL_BYTES];
    memcpy(bad, buf, sizeof(bad));
    bad[0] = 9;
    check("deserial: count above 8 refused", !bindListDeserialize(&back, bad, sizeof(bad)) && back.count == 0);
    memcpy(bad, buf, sizeof(bad));
    memset(&bad[4 + HP_BIND_ENTRY_BYTES], 0, 16);
    check("deserial: a zero id refused", !bindListDeserialize(&back, bad, sizeof(bad)) && back.count == 0);
    memcpy(bad, buf, sizeof(bad));
    memcpy(&bad[4 + HP_BIND_ENTRY_BYTES], a, 16);
    check("deserial: a duplicate id refused", !bindListDeserialize(&back, bad, sizeof(bad)) && back.count == 0);
}

int main(void)
{
    testAddFind();
    testLru();
    testRemove();
    testSerial();
    return finish();
}
