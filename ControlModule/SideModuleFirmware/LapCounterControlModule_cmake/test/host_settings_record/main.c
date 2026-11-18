/*
 * Host test for settings_record.c and crc32.c: the record format and the
 * two-page selection of settings_store (CONTROL_FIRMWARE_GUIDE §5.5).
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "crc32.h"
#include "settings_record.h"
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

static void testCrc(void)
{
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

    erase(p0);
    settingsRecordSelect(p0, p1, PAGE, &s);
    check("select: corrupt + empty -> none, corrupt reported", s.active == -1 && s.corrupt && s.writeTarget == 0);
}

int main(void)
{
    testCrc();
    testEncode();
    testCheck();
    testSelect();
    return finish();
}
