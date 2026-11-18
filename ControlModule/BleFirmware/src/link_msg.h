/*
 * nRF <-> STM link messages: framing of DATA and CTRL through frame_codec,
 * and the CTRL pack/parse rules. BLE_FIRMWARE_GUIDE.md §5, the contract is
 * ble_link_protocol.h.
 *
 *   link frame = COBS( channel u8 | body | crc16 ) 0x00
 *
 * Receive rules (each one has its own rejection case in test/host_link_msg):
 *   - channel DATA: body 1..BLE_LINK_DATA_MAX bytes, else LINK_MSG_ERR_DATA_LEN
 *   - channel CTRL: body = type u8 + message. No type byte, or fewer message
 *     bytes than the type needs: LINK_MSG_ERR_CTRL_SHORT (dropped). More
 *     bytes: accepted, the extra ignored (a newer MINOR appended fields).
 *   - CTRL type unknown, or an nRF -> STM type: LINK_MSG_CTRL_UNKNOWN
 *     (ignored, counted)
 *   - a known type with a value out of range (UI_STATE.session > BOUND,
 *     pairWindow > 1, CUE.kind unknown, ADVERTISE.on > 1):
 *     LINK_MSG_CTRL_BAD_VALUE (ignored, counted)
 *   - any other channel: LINK_MSG_ERR_CHANNEL
 *
 * Pure logic: no Zephyr, no globals.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ble_link_protocol.h"
#include "frame_codec.h"

/* Decoder buffer for the largest link frame. */
#define LINK_RX_BUF_BYTES   FRAME_CODEC_RX_BUF_BYTES(BLE_LINK_RAW_MAX)

/* Worst-case wire bytes of one link frame. */
#define LINK_WIRE_MAX       FRAME_CODEC_WIRE_BOUND(BLE_LINK_RAW_MAX)

/* Largest CTRL message body (type byte excluded). */
#define LINK_CTRL_MSG_MAX   sizeof(bleCtrlNrfHello_t)

/* HCI reason used for KICK when the STM's reason is not one the controller
 * accepts for a disconnect: "remote user terminated connection". */
#define LINK_KICK_DEFAULT_REASON  0x13U

typedef enum {
    LINK_MSG_DATA            =  1,
    LINK_MSG_CTRL            =  2,
    LINK_MSG_CTRL_UNKNOWN    = -1,
    LINK_MSG_CTRL_BAD_VALUE  = -2,
    LINK_MSG_ERR_CTRL_SHORT  = -3,
    LINK_MSG_ERR_DATA_LEN    = -4,
    LINK_MSG_ERR_CHANNEL     = -5,
} linkMsgResult_e;

/* A parsed STM -> nRF CTRL message. */
typedef struct {
    uint8_t type;                         /* bleCtrlType_e */
    union {
        bleCtrlStmHello_t  stmHello;
        bleCtrlUiState_t   uiState;
        bleCtrlCue_t       cue;
        bleCtrlKick_t      kick;
        bleCtrlAdvertise_t advertise;
    } u;
} linkCtrlMsg_t;

typedef struct {
    const uint8_t *data;                  /* DATA: points into the raw frame */
    uint16_t       len;
    linkCtrlMsg_t  ctrl;                  /* CTRL */
} linkMsg_t;

/* Classifies and parses one raw link frame (as frame_codec delivered it). */
linkMsgResult_e linkParse(const uint8_t *raw, uint16_t rawLen, linkMsg_t *out);

/* The message size a CTRL type needs (type byte excluded), 0 if unknown.
 * Covers both directions. */
uint16_t linkCtrlSize(uint8_t type);

/* Encodes a DATA frame. 0 if len is 0 or > BLE_LINK_DATA_MAX, or out is too
 * small. */
uint16_t linkEncodeData(const uint8_t *data, uint16_t len, uint8_t *out, uint16_t outCap);

/* Encodes a CTRL frame: type + msgLen bytes of msg (a packed struct of
 * ble_link_protocol.h). 0 if msgLen > LINK_CTRL_MSG_MAX or out is too
 * small. */
uint16_t linkEncodeCtrl(uint8_t type, const void *msg, uint16_t msgLen, uint8_t *out, uint16_t outCap);

/* KICK reason to hand to the controller: the STM's when it is a valid HCI
 * disconnect reason, else LINK_KICK_DEFAULT_REASON. */
uint8_t linkKickReason(uint8_t stmReason);

/* nRF52 POWER->RESETREAS bits to NRF_HELLO.resetCause. */
#define NRF_RESETREAS_RESETPIN  (1UL << 0)
#define NRF_RESETREAS_DOG       (1UL << 1)
#define NRF_RESETREAS_SREQ      (1UL << 2)
#define NRF_RESETREAS_LOCKUP    (1UL << 3)
#define NRF_RESETREAS_OFF       (1UL << 16)
#define NRF_RESETREAS_DIF       (1UL << 18)

bleResetCause_e linkResetCause(uint32_t resetreas);

/* Saturating counter conversion for STATS. */
uint16_t linkSat16(uint32_t v);
