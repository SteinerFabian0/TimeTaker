/*
 * Host test for settings_record.c and crc32.c: the record format and the
 * two-page selection of settings_store (CONTROL_FIRMWARE_GUIDE §5.5).
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "crc32.h"
#include "settings_record.h"
#include "settings_store.h"   /* settingsData_t layout only */
#include "../test_check.h"

#define PAGE 1024U

static uint8_t p0[PAGE], p1[PAGE];

static void erase(uint8_t *p)
{
    memset(p, 0xFF, PAGE);
}

static void write(uint8_t *p, uint32_t seq, uint8_t fill)
{
    uint8_t payload[100];
    memset(payload, fill, sizeof(payload));
    erase(p);
    settingsRecordEncode(payload, sizeof(payload), seq, p, PAGE);
}

/* Bit-exact model of the C5 CRC unit as crc32.c configures it (POLYSIZE 32,
 * POL 0x04C11DB7, REV_IN byte, REV_OUT bit, INIT = rbit32(crc)), written from
 * the reference manual semantics: shift left, MSB first. Must equal the
 * software path, which is what the target's hardware path is built to match. */
static uint32_t rbit32(uint32_t v)
{
    uint32_t r = 0U;
    for (int i = 0; i < 32; i++) {
        r = (r << 1) | ((v >> i) & 1U);
    }
    return r;
}

static uint32_t hwModelUpdate(uint32_t crc, const uint8_t *d, uint32_t len)
{
    uint32_t dr = rbit32(crc);                        /* INIT, then reset */
    for (uint32_t i = 0U; i < len; i++) {
        uint32_t in = 0U;
        for (int b = 0; b < 8; b++) {
            in = (in << 1) | (((uint32_t)d[i] >> b) & 1U);   /* REV_IN byte */
        }
        dr ^= in << 24;
        for (int b = 0; b < 8; b++) {
            dr = ((dr & 0x80000000U) != 0U) ? ((dr << 1) ^ 0x04C11DB7U) : (dr << 1);
        }
    }
    return rbit32(dr);                                /* REV_OUT bit */
}

static void testCrc(void)
{
    uint8_t buf[300];
    for (uint32_t i = 0U; i < sizeof(buf); i++) {
        buf[i] = (uint8_t)(i * 37U + 11U);
    }
    check("crc model: \"123456789\" = 0xCBF43926",
          (hwModelUpdate(CRC32_INIT, (const uint8_t *)"123456789", 9) ^ 0xFFFFFFFFU) == 0xCBF43926U);
    check("crc model: equals software, whole buffer",
          hwModelUpdate(CRC32_INIT, buf, sizeof(buf)) == crc32Update(CRC32_INIT, buf, sizeof(buf)));
    check("crc model: equals software, chunked streaming",
          hwModelUpdate(hwModelUpdate(CRC32_INIT, buf, 123), buf + 123, 177U) ==
          crc32Update(crc32Update(CRC32_INIT, buf, 123), buf + 123, 177U));
    check("crc32: empty input = 0", crc32Compute(buf, 0U) == 0U);

    check("crc32: check value of \"123456789\"", crc32Compute((const uint8_t *)"123456789", 9) == 0xCBF43926U);
    uint32_t c = crc32Update(CRC32_INIT, (const uint8_t *)"1234", 4);
    c = crc32Update(c, (const uint8_t *)"56789", 5);
    check("crc32: streaming equals one-shot", crc32Final(c) == 0xCBF43926U);
}

static void testEncode(void)
{
    uint8_t out[64];
    uint8_t payload[5] = { 1, 2, 3, 4, 5 };
    uint32_t n = settingsRecordEncode(payload, 5, 7, out, sizeof(out));
    check("encode: length padded to quad-words", n == 32U && SETTINGS_RECORD_BYTES(5) == 32U);
    check("encode: pad bytes erased", out[16 + 5 + 4] == 0xFF && out[31] == 0xFF);
    settingsRecordHeader_t h;
    const uint8_t *pl = NULL;
    check("encode: decodes back VALID with the payload",
          settingsRecordCheck(out, sizeof(out), &h, &pl) == SETTINGS_PAGE_VALID && h.sequence == 7U
          && h.length == 5U && pl != NULL && memcmp(pl, payload, 5) == 0);
    check("encode: refuses a short buffer", settingsRecordEncode(payload, 5, 7, out, 31) == 0U);
}

static void testCheck(void)
{
    settingsRecordHeader_t h;
    erase(p0);
    check("check: erased page -> EMPTY", settingsRecordCheck(p0, PAGE, &h, NULL) == SETTINGS_PAGE_EMPTY);
    write(p0, 1, 0x11);
    check("check: written page -> VALID", settingsRecordCheck(p0, PAGE, &h, NULL) == SETTINGS_PAGE_VALID);
    p0[40] ^= 1U;
    check("check: a flipped payload bit -> CORRUPT", settingsRecordCheck(p0, PAGE, &h, NULL) == SETTINGS_PAGE_CORRUPT);
    write(p0, 1, 0x11);
    p0[0] ^= 1U;
    check("check: bad magic -> CORRUPT", settingsRecordCheck(p0, PAGE, &h, NULL) == SETTINGS_PAGE_CORRUPT);
    write(p0, 1, 0x11);
    settingsRecordHeader_t hh;
    memcpy(&hh, p0, sizeof(hh));
    hh.version = SETTINGS_RECORD_VERSION + 1U;
    memcpy(p0, &hh, sizeof(hh));
    uint32_t crc = crc32Compute(p0, sizeof(hh) + hh.length);
    memcpy(p0 + sizeof(hh) + hh.length, &crc, 4);
    check("check: another version (CRC fine) -> CORRUPT", settingsRecordCheck(p0, PAGE, &h, NULL) == SETTINGS_PAGE_CORRUPT);
    write(p0, 1, 0x11);
    memcpy(&hh, p0, sizeof(hh));
    hh.length = (uint16_t)PAGE;
    memcpy(p0, &hh, sizeof(hh));
    check("check: length beyond the page -> CORRUPT", settingsRecordCheck(p0, PAGE, &h, NULL) == SETTINGS_PAGE_CORRUPT);
    erase(p0);
    p0[0] = 0x00;           /* a half-programmed first quad-word */
    check("check: not erased but not a record -> CORRUPT",
          settingsRecordCheck(p0, PAGE, &h, NULL) == SETTINGS_PAGE_CORRUPT);
}

static void testSelect(void)
{
    settingsSelect_t s;
    erase(p0); erase(p1);
    settingsRecordSelect(p0, p1, PAGE, &s);
    check("select: new board -> none active, target 0, not corrupt", s.active == -1 && s.writeTarget == 0 && !s.corrupt);

    write(p0, 5, 0x55);
    settingsRecordSelect(p0, p1, PAGE, &s);
    check("select: one valid page -> it, target the other", s.active == 0 && s.writeTarget == 1 && s.sequence == 5U);

    write(p1, 6, 0x66);
    settingsRecordSelect(p0, p1, PAGE, &s);
    check("select: both valid -> the higher sequence", s.active == 1 && s.sequence == 6U && s.writeTarget == 0);

    write(p0, 0x00000001U, 0x11);
    write(p1, 0xFFFFFFFFU, 0x22);
    settingsRecordSelect(p0, p1, PAGE, &s);
    check("select: sequence wrap -> 1 is newer than 0xFFFFFFFF", s.active == 0);

    write(p0, 9, 0x99);
    p1[0] = 0x00;
    settingsRecordSelect(p0, p1, PAGE, &s);
    check("select: valid + corrupt -> the valid one, not reported corrupt", s.active == 0 && !s.corrupt);

    /* Power cut after the first quad-word of the new record: the old one stays. */
    write(p0, 9, 0x99);
    erase(p1);
    { uint8_t pl[100]; uint8_t rec[160];
      memset(pl, 0x77, sizeof(pl));
      uint32_t n = settingsRecordEncode(pl, sizeof(pl), 10, rec, sizeof(rec));
      memcpy(p1, rec, 16U);
      settingsRecordSelect(p0, p1, PAGE, &s);
      check("select: half-written newer record -> old stays active", n > 16U && s.active == 0 && s.sequence == 9U && s.writeTarget == 1);
      memcpy(p1, rec, n - 16U);   /* all but the last quad-word (the CRC) */
      settingsRecordSelect(p0, p1, PAGE, &s);
      check("select: record missing its CRC quad-word -> old stays", s.active == 0 && !s.corrupt); }

    erase(p0);
    settingsRecordSelect(p0, p1, PAGE, &s);
    check("select: corrupt + empty -> none, corrupt reported", s.active == -1 && s.corrupt && s.writeTarget == 0);
}

/* A record of any version, CRC fine (as an older/newer firmware wrote it). */
static uint32_t writeVersion(uint8_t *page, uint16_t version, const uint8_t *payload, uint16_t len, uint32_t seq)
{
    erase(page);
    uint32_t n = settingsRecordEncode(payload, len, seq, page, PAGE);
    settingsRecordHeader_t h;
    memcpy(&h, page, sizeof(h));
    h.version = version;
    memcpy(page, &h, sizeof(h));
    uint32_t crc = crc32Compute(page, sizeof(h) + len);
    memcpy(page + sizeof(h) + len, &crc, 4);
    return n;
}

static void testMigration(void)
{
    _Static_assert(SETTINGS_RECORD_BYTES(sizeof(settingsData_t)) <= PAGE, "test page holds a record");
    settingsData_t old;
    memset(&old, 0, sizeof(old));
    uint8_t *raw = (uint8_t *)&old;
    for (uint32_t i = 0U; i < SETTINGS_DATA_V1_BYTES; i++) {
        raw[i] = (uint8_t)(i * 7U + 3U);   /* every v1 byte distinct-ish, none of it 0 by design */
    }

    /* A version-1 record: exactly the bytes before laneReference. */
    writeVersion(p0, 1U, raw, (uint16_t)SETTINGS_DATA_V1_BYTES, 4);
    settingsRecordHeader_t h;
    const uint8_t *pl = NULL;
    check("migrate: version 1 record -> VALID",
          settingsRecordCheck(p0, PAGE, &h, &pl) == SETTINGS_PAGE_VALID && h.version == 1U);

    settingsData_t d;
    memset(&d, 0, sizeof(d));                 /* defaults: references 0 */
    d.laneReference[2] = 0xBEEF;              /* a default the old record must not touch */
    uint16_t n = settingsRecordLoad(&h, pl, (uint8_t *)&d, (uint16_t)sizeof(d));
    check("migrate: v1 copies its whole payload", n == SETTINGS_DATA_V1_BYTES);
    check("migrate: every v1 field kept (device, lane table, modes, binds)",
          memcmp(&d, raw, SETTINGS_DATA_V1_BYTES) == 0);
    check("migrate: references keep the defaults", d.laneReference[0] == 0 && d.laneReference[2] == 0xBEEF
          && d.laneReference[5] == 0);

    /* The current version round-trips with the references. */
    settingsData_t cur = old;
    for (uint8_t i = 0U; i < LANE_MAX_LANES; i++) {
        cur.laneReference[i] = (uint16_t)(1000U + i);
    }
    erase(p1);
    settingsRecordEncode((const uint8_t *)&cur, (uint16_t)sizeof(cur), 5, p1, PAGE);
    check("migrate: version 2 record -> VALID", settingsRecordCheck(p1, PAGE, &h, &pl) == SETTINGS_PAGE_VALID
          && h.version == SETTINGS_RECORD_VERSION && SETTINGS_RECORD_VERSION == 2U);
    memset(&d, 0, sizeof(d));
    settingsRecordLoad(&h, pl, (uint8_t *)&d, (uint16_t)sizeof(d));
    check("migrate: v2 loads the references", memcmp(&d, &cur, sizeof(d)) == 0);

    settingsSelect_t sel;
    settingsRecordSelect(p0, p1, PAGE, &sel);
    check("migrate: v1 + newer v2 -> the v2 one", sel.active == 1 && sel.sequence == 5U);

    writeVersion(p0, 0U, raw, (uint16_t)SETTINGS_DATA_V1_BYTES, 4);
    check("migrate: version 0 -> CORRUPT", settingsRecordCheck(p0, PAGE, &h, NULL) == SETTINGS_PAGE_CORRUPT);
    writeVersion(p0, SETTINGS_RECORD_VERSION + 1U, raw, (uint16_t)SETTINGS_DATA_V1_BYTES, 4);
    check("migrate: a newer version than known -> CORRUPT",
          settingsRecordCheck(p0, PAGE, &h, NULL) == SETTINGS_PAGE_CORRUPT);

    /* A longer (future) payload is cut at what this firmware knows. */
    uint8_t big[sizeof(settingsData_t) + 8];
    memset(big, 0x5A, sizeof(big));
    settingsRecordHeader_t hb = { .length = (uint16_t)sizeof(big) };
    memset(&d, 0, sizeof(d));
    check("load: longer payload cut to the struct",
          settingsRecordLoad(&hb, big, (uint8_t *)&d, (uint16_t)sizeof(d)) == sizeof(d));
}

static void testSaveState(void)
{
    check("save: nothing written yet -> pending", settingsRecordSaveState(0, 0, 1) == SETTINGS_SAVE_PENDING);
    check("save: written at the ticket -> WRITTEN", settingsRecordSaveState(3, 0, 3) == SETTINGS_SAVE_WRITTEN);
    check("save: written past the ticket -> WRITTEN", settingsRecordSaveState(5, 0, 3) == SETTINGS_SAVE_WRITTEN);
    check("save: an older write only -> pending", settingsRecordSaveState(2, 0, 3) == SETTINGS_SAVE_PENDING);
    check("save: write covering it failed -> FAILED", settingsRecordSaveState(2, 3, 3) == SETTINGS_SAVE_FAILED);
    check("save: an older write failed -> pending", settingsRecordSaveState(2, 2, 3) == SETTINGS_SAVE_PENDING);
    check("save: failed, then a later success -> WRITTEN", settingsRecordSaveState(4, 3, 3) == SETTINGS_SAVE_WRITTEN);
    check("save: generation wrap", settingsRecordSaveState(1U, 0U, 0xFFFFFFFFU) == SETTINGS_SAVE_WRITTEN);
    check("save: no change since boot (ticket 0) -> WRITTEN", settingsRecordSaveState(0, 0, 0) == SETTINGS_SAVE_WRITTEN);
}

int main(void)
{
    testMigration();
    testSaveState();
    testCrc();
    testEncode();
    testCheck();
    testSelect();
    return finish();
}
