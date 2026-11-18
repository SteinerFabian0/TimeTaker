/*
 * Settings record: how one settings snapshot sits in a flash page, and which
 * of the two pages holds the current one (CONTROL_FIRMWARE_GUIDE §5.5).
 *
 *   record = header (16 B) | payload (length B) | crc32 (4 B) | 0xFF pad to 16
 *   header = magic u32 | version u16 | length u16 | sequence u32 | reserved u32
 *   crc32  = CRC-32 (crc32.h) over header | payload
 *
 * Two pages are used alternately, one record per page: a new record goes into
 * the erased page with sequence + 1, then the old page is erased. At boot the
 * valid record with the higher sequence wins (compared with wrap, so the
 * counter never runs out). A page whose first quad-word is all 0xFF is EMPTY;
 * one that holds anything else that fails a check is CORRUPT. "Both empty" is
 * a new board (defaults, no error). "Corrupt and nothing valid" is reported
 * (ERR_SETTINGS_CORRUPT) and falls back to the defaults.
 *
 * Every record is padded to whole 16-byte quad-words, the C5's flash program
 * unit, so a record is written with nothing but full quad-word programs.
 *
 * Pure logic: no HAL, no globals. The pages are read as plain memory (the
 * flash is memory-mapped; the host test uses arrays). Host-tested
 * (test/host_settings_record).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "host_protocol.h"

#define SETTINGS_RECORD_MAGIC        0x54545354U   /* "TSTT" little-endian: TimeTaker SeTTings */
/* Version 2 appended the per-lane saved references. Older versions still
 * load: fields are only ever appended to the payload, so the loader copies
 * what the record holds and keeps the defaults for the rest
 * (settingsRecordLoad). A newer version than this firmware knows is CORRUPT. */
#define SETTINGS_RECORD_VERSION      2U
#define SETTINGS_RECORD_VERSION_MIN  1U
#define SETTINGS_RECORD_HEADER_BYTES 16U
#define SETTINGS_RECORD_CRC_BYTES    4U
#define SETTINGS_RECORD_ALIGN        16U

/* Bytes a record with payloadLen bytes of payload takes, padding included. */
#define SETTINGS_RECORD_BYTES(payloadLen) \
    ((((uint32_t)SETTINGS_RECORD_HEADER_BYTES + (payloadLen) + SETTINGS_RECORD_CRC_BYTES) \
      + SETTINGS_RECORD_ALIGN - 1U) / SETTINGS_RECORD_ALIGN * SETTINGS_RECORD_ALIGN)

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t length;      /* payload bytes */
    uint32_t sequence;
    uint32_t reserved;    /* 0 */
} settingsRecordHeader_t;

_Static_assert(sizeof(settingsRecordHeader_t) == SETTINGS_RECORD_HEADER_BYTES, "settingsRecordHeader_t");

typedef enum {
    SETTINGS_PAGE_EMPTY   = 0,
    SETTINGS_PAGE_VALID   = 1,
    SETTINGS_PAGE_CORRUPT = 2,
} settingsPageState_e;

typedef struct {
    settingsPageState_e state[2];
    int8_t   active;          /* page index of the current record, -1 = none */
    uint8_t  writeTarget;     /* page the next record goes into            */
    uint32_t sequence;        /* sequence of the active record (0 if none)  */
    bool     corrupt;         /* nothing valid, and at least one page held garbage */
} settingsSelect_t;

/*
 * Encodes a record into out (cap bytes). Returns the record length (a multiple
 * of 16, pad bytes 0xFF), or 0 if payloadLen exceeds 0xFFFF or cap is too
 * small.
 */
uint32_t settingsRecordEncode(const uint8_t *payload, uint16_t payloadLen, uint32_t sequence,
                              uint8_t *out, uint32_t cap);

/*
 * Checks the record at the start of a page of pageBytes. VALID fills *header;
 * *payload then points into the page.
 */
settingsPageState_e settingsRecordCheck(const uint8_t *page, uint32_t pageBytes,
                                        settingsRecordHeader_t *header, const uint8_t **payload);

/*
 * Copies a VALID record's payload over out (outLen bytes, prefilled with the
 * defaults by the caller): min(length, outLen) bytes. A shorter, older record
 * keeps the defaults for the fields appended since; a longer one is cut.
 * Returns the bytes copied.
 */
uint16_t settingsRecordLoad(const settingsRecordHeader_t *header, const uint8_t *payload,
                            uint8_t *out, uint16_t outLen);

/* Whether the data of change generation `ticket` reached the flash. savedGen:
 * generation of the last record written and verified; failedGen: of the last
 * write that failed (both wrap-compared, a later success wins). */
typedef enum {
    SETTINGS_SAVE_PENDING = 0,
    SETTINGS_SAVE_WRITTEN,
    SETTINGS_SAVE_FAILED,
} settingsSave_e;

settingsSave_e settingsRecordSaveState(uint32_t savedGen, uint32_t failedGen, uint32_t ticket);

/* Classifies both pages and picks the current record and the next target. */
void settingsRecordSelect(const uint8_t *page0, const uint8_t *page1, uint32_t pageBytes,
                          settingsSelect_t *sel);
