/*
 * Settings record. See settings_record.h.
 */

#include "settings_record.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "crc32.h"

uint32_t settingsRecordEncode(const uint8_t *payload, uint16_t payloadLen, uint32_t sequence,
                              uint8_t *out, uint32_t cap)
{
    const uint32_t total = SETTINGS_RECORD_BYTES(payloadLen);
    if (total > cap || (payload == NULL && payloadLen != 0U)) {
        return 0U;
    }

    const settingsRecordHeader_t hdr = {
        .magic    = SETTINGS_RECORD_MAGIC,
        .version  = SETTINGS_RECORD_VERSION,
        .length   = payloadLen,
        .sequence = sequence,
        .reserved = 0U,
    };
    memset(out, 0xFF, total);
    memcpy(out, &hdr, sizeof(hdr));
    if (payloadLen != 0U) {
        memcpy(out + sizeof(hdr), payload, payloadLen);
    }
    const uint32_t crc = crc32Compute(out, (uint32_t)sizeof(hdr) + payloadLen);
    memcpy(out + sizeof(hdr) + payloadLen, &crc, sizeof(crc));
    return total;
}

static bool settingsRecordIsErased(const uint8_t *p, uint32_t len)
{
    for (uint32_t i = 0U; i < len; i++) {
        if (p[i] != 0xFFU) {
            return false;
        }
    }
    return true;
}

settingsPageState_e settingsRecordCheck(const uint8_t *page, uint32_t pageBytes,
                                        settingsRecordHeader_t *header, const uint8_t **payload)
{
    if (pageBytes < SETTINGS_RECORD_BYTES(0U)) {
        return SETTINGS_PAGE_CORRUPT;
    }
    if (settingsRecordIsErased(page, SETTINGS_RECORD_HEADER_BYTES)) {
        return SETTINGS_PAGE_EMPTY;
    }

    settingsRecordHeader_t hdr;
    memcpy(&hdr, page, sizeof(hdr));
    if (hdr.magic != SETTINGS_RECORD_MAGIC || hdr.version < SETTINGS_RECORD_VERSION_MIN || hdr.version > SETTINGS_RECORD_VERSION
        || SETTINGS_RECORD_BYTES(hdr.length) > pageBytes) {
        return SETTINGS_PAGE_CORRUPT;
    }

    uint32_t stored;
    memcpy(&stored, page + sizeof(hdr) + hdr.length, sizeof(stored));
    if (crc32Compute(page, (uint32_t)sizeof(hdr) + hdr.length) != stored) {
        return SETTINGS_PAGE_CORRUPT;
    }

    if (header != NULL) {
        *header = hdr;
    }
    if (payload != NULL) {
        *payload = page + sizeof(hdr);
    }
    return SETTINGS_PAGE_VALID;
}

uint16_t settingsRecordLoad(const settingsRecordHeader_t *header, const uint8_t *payload,
                            uint8_t *out, uint16_t outLen)
{
    const uint16_t n = (header->length < outLen) ? header->length : outLen;
    memcpy(out, payload, n);
    return n;
}

settingsSave_e settingsRecordSaveState(uint32_t savedGen, uint32_t failedGen, uint32_t ticket)
{
    if ((int32_t)(savedGen - ticket) >= 0) {
        return SETTINGS_SAVE_WRITTEN;
    }
    if ((int32_t)(failedGen - ticket) >= 0) {
        return SETTINGS_SAVE_FAILED;
    }
    return SETTINGS_SAVE_PENDING;
}

void settingsRecordSelect(const uint8_t *page0, const uint8_t *page1, uint32_t pageBytes,
                          settingsSelect_t *sel)
{
    settingsRecordHeader_t h[2];
    memset(h, 0, sizeof(h));
    memset(sel, 0, sizeof(*sel));

    sel->state[0] = settingsRecordCheck(page0, pageBytes, &h[0], NULL);
    sel->state[1] = settingsRecordCheck(page1, pageBytes, &h[1], NULL);

    const bool v0 = sel->state[0] == SETTINGS_PAGE_VALID;
    const bool v1 = sel->state[1] == SETTINGS_PAGE_VALID;

    if (v0 && v1) {
        /* Both valid: an erase after a write was cut off. Newer wins (wrap-safe). */
        sel->active = ((int32_t)(h[1].sequence - h[0].sequence) > 0) ? 1 : 0;
    } else if (v0) {
        sel->active = 0;
    } else if (v1) {
        sel->active = 1;
    } else {
        sel->active  = -1;
        sel->corrupt = sel->state[0] == SETTINGS_PAGE_CORRUPT || sel->state[1] == SETTINGS_PAGE_CORRUPT;
    }

    if (sel->active >= 0) {
        sel->sequence    = h[sel->active].sequence;
        sel->writeTarget = (uint8_t)(1 - sel->active);
    } else {
        sel->sequence    = 0U;
        sel->writeTarget = 0U;
    }
}
