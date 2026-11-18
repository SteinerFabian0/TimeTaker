/*
 * nRF link protocol, pure part: link frame parsing, CTRL length rules, credit
 * accounting and frame building (BLE_FIRMWARE_GUIDE §5, ble_link_protocol.h).
 *
 *   link frame = COBS( channel u8 | body | crc16 ) 0x00     (frame_codec)
 *
 * The rules this module holds:
 *   - DATA carries 1..BLE_LINK_DATA_MAX opaque bytes. An empty or longer one
 *     is dropped and counted.
 *   - CTRL body = type u8 + fixed struct. Unknown type: ignored, counted. Body
 *     shorter than the type needs: dropped, counted. Longer: accepted, the
 *     extra bytes (fields of a newer MINOR) ignored.
 *   - Credits (STM -> nRF DATA only): NRF_HELLO sets the count to its value,
 *     each DATA frame takes one, CREDIT returns some. The grant is only the
 *     slots free at that moment (busy ones return later), so the count may
 *     rise above it; only a uint8_t overflow is clamped and counted.
 *   - STATS.slotsFree resyncs the count (see nrfLinkCreditsResync).
 *   - Link MAJOR must match BLE_LINK_MAJOR, or DATA is refused (CTRL keeps
 *     working).
 *
 * Pure logic: no HAL, no globals. nrf_link.c does the UART and the rings.
 * Host-tested (test/host_nrf_link).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ble_link_protocol.h"
#include "frame_codec.h"

/* Largest encoded link frame (wire bytes, delimiter included). */
#define NRF_LINK_WIRE_MAX  FRAME_CODEC_WIRE_BOUND(BLE_LINK_RAW_MAX - FRAME_CODEC_CRC_BYTES)

typedef enum {
    NRF_LINK_PARSE_DATA = 0,       /* msg->data / dataLen valid             */
    NRF_LINK_PARSE_CTRL,           /* msg->ctrlType + the matching union    */
    NRF_LINK_PARSE_UNKNOWN_CTRL,   /* ignore, count                          */
    NRF_LINK_PARSE_SHORT_CTRL,     /* drop, count                            */
    NRF_LINK_PARSE_BAD_DATA,       /* empty or longer than BLE_LINK_DATA_MAX */
    NRF_LINK_PARSE_BAD_CHANNEL,    /* neither DATA nor CTRL, or no channel   */
} nrfLinkParse_e;

typedef struct {
    uint8_t        ctrlType;       /* bleCtrlType_e (CTRL only) */
    const uint8_t *data;           /* DATA only: points into the raw frame */
    uint16_t       dataLen;
    union {
        bleCtrlNrfHello_t hello;
        bleCtrlLinkUp_t   linkUp;
        bleCtrlLinkDown_t linkDown;
        bleCtrlButton_t   button;
        bleCtrlCredit_t   credit;
        bleCtrlStats_t    stats;
    } ctrl;
} nrfLinkMsg_t;

typedef struct {
    uint8_t  credits;       /* DATA frames the STM may send now */
    uint8_t  granted;       /* credits granted by the last NRF_HELLO (slots free then) */
    uint32_t overReturned;  /* CREDITs that overflowed the uint8_t count (a desync) */
    uint32_t lastDataMs;    /* when the last DATA frame went out (valid if dataSent) */
    bool     dataSent;
    uint32_t resyncs;       /* STATS resyncs that changed the count */
} nrfLinkCredits_t;

/* STATS carries the nRF's free DATA slots. A DATA frame the nRF dropped
 * (corrupt on the wire) loses its credit for good, so STATS resyncs the count,
 * but only if no DATA frame left within the last NRF_LINK_RESYNC_QUIET_MS
 * (STATS was built before that frame arrived, or its credit is still on the
 * way back). */
#define NRF_LINK_RESYNC_QUIET_MS  20U

/* Classifies one decoded link frame (the raw bytes frame_codec handed over). */
nrfLinkParse_e nrfLinkProtoParse(const uint8_t *raw, uint16_t len, nrfLinkMsg_t *msg);

/* Whether the nRF speaks our link MAJOR. */
bool nrfLinkProtoVersionOk(const bleCtrlNrfHello_t *hello);

void nrfLinkCreditsReset(nrfLinkCredits_t *c, uint8_t granted);
void nrfLinkCreditsReturn(nrfLinkCredits_t *c, uint8_t count);
bool nrfLinkCreditsTake(nrfLinkCredits_t *c);

/* A DATA frame was handed to the UART at nowMs. */
void nrfLinkCreditsNoteData(nrfLinkCredits_t *c, uint32_t nowMs);

/* STATS.slotsFree arrived. Sets the count to it (up or down) when connected
 * and no DATA frame went out in the last NRF_LINK_RESYNC_QUIET_MS. Returns
 * true if applied. */
bool nrfLinkCreditsResync(nrfLinkCredits_t *c, uint8_t slotsFree, uint32_t nowMs, bool connected);

/* Builds a DATA frame of 1..BLE_LINK_DATA_MAX bytes. Returns the wire bytes,
 * 0 on a bad length or a short out buffer. */
uint16_t nrfLinkProtoEncodeData(const uint8_t *data, uint16_t len, uint8_t *out, uint16_t cap);

/* Builds a CTRL frame: type + body (bodyLen may be 0). Same return rule. */
uint16_t nrfLinkProtoEncodeCtrl(uint8_t type, const void *body, uint16_t bodyLen,
                                uint8_t *out, uint16_t cap);
