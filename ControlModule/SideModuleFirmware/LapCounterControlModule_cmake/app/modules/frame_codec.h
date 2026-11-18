/*
 * Frame codec: COBS framing with a CRC-16, for every byte stream in the system.
 *
 *     wire = COBS( raw | crc16 ) 0x00
 *
 * raw is whatever the layer above puts in a frame:
 *   - host protocol: header | payload (host_protocol.h)
 *   - nRF link:      channel | payload (SYSTEM_ARCHITECTURE §9.3)
 *
 * The CRC is CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no
 * xorout) over raw, appended little-endian. COBS removes every 0x00 from the
 * frame, so 0x00 only ever means "frame ends here". A receiver that joins
 * mid-stream, or loses bytes, resynchronises at the next 0x00 and loses at most
 * that one frame.
 *
 * Pure logic: no HAL, no globals, no allocation. The same file is meant to
 * build in the control app and the nRF bridge. Host
 * tests: test/host_frame_codec (unit), test/host_protocol (golden bytes shared
 * with gen_protocol.py and the web app's codec.ts). The three encoders must
 * stay byte-identical: a 0xFF COBS block is always followed by a new block,
 * even at the end of the data.
 *
 * Owns: nothing but the decoder state the caller hands it.
 * Never: blocks, allocates, or keeps a pointer to the caller's input.
 * Context: not reentrant per decoder. One decoder per byte stream, fed from
 *          one context (the main loop, or one ISR).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define FRAME_CODEC_CRC_BYTES      2U
#define FRAME_CODEC_DELIMITER      0x00U

/* Worst-case wire length for rawLen bytes of raw: CRC, one COBS code byte per
 * started 254-byte block, the delimiter. Size transmit buffers with it. */
#define FRAME_CODEC_WIRE_BOUND(rawLen) \
    ((rawLen) + FRAME_CODEC_CRC_BYTES + ((rawLen) + FRAME_CODEC_CRC_BYTES) / 254U + 2U)

/* Receive buffer bytes a decoder needs for frames of up to rawMax raw bytes. */
#define FRAME_CODEC_RX_BUF_BYTES(rawMax)  ((rawMax) + FRAME_CODEC_CRC_BYTES)

typedef enum {
    FRAME_NONE         =  0,  /* No complete frame yet (also: an empty frame, ignored) */
    FRAME_READY        =  1,  /* A frame passed its CRC: see frameDecoder_t.frame/frameLen */
    FRAME_ERR_CRC      = -1,  /* Frame complete but the CRC does not match */
    FRAME_ERR_COBS     = -2,  /* Delimiter arrived inside a COBS block: truncated frame */
    FRAME_ERR_OVERFLOW = -3,  /* Frame longer than the receive buffer: dropped */
    FRAME_ERR_SHORT    = -4,  /* Fewer bytes than CRC + 1 raw byte */
} frameResult_e;

/* Diagnostics: free-running, only frameDecoderInit() clears them. A climbing
 * crcErrors with cobsErrors flat points at bit errors on the line; cobsErrors
 * climbing points at lost bytes (overrun, a receiver that cannot keep up). */
typedef struct {
    uint32_t frames;
    uint32_t crcErrors;
    uint32_t cobsErrors;
    uint32_t overflows;
    uint32_t shortFrames;
} frameCodecStats_t;

/* Streaming decoder state. Caller-owned. Treat as opaque except for the
 * fields marked "read". */
typedef struct {
    uint8_t *buf;             /* caller's buffer, FRAME_CODEC_RX_BUF_BYTES(rawMax) */
    uint16_t cap;
    uint16_t len;             /* decoded bytes of the frame in progress */
    uint8_t  blockCode;       /* code byte of the current COBS block; 0 = frame start */
    uint8_t  blockLeft;       /* data bytes still due in the current block */
    bool     discarding;      /* overflowed: skip to the next delimiter */

    const uint8_t *frame;     /* read: raw frame after FRAME_READY (CRC stripped) */
    uint16_t       frameLen;  /* read */
    frameCodecStats_t stats;  /* read */
} frameDecoder_t;

/*
 * CRC-16/CCITT-FALSE over len bytes, continuing from crc (start with 0xFFFF).
 */
uint16_t frameCrc16(uint16_t crc, const uint8_t *data, uint16_t len);

/*
 * Encodes head | body as one frame into out, delimiter included. Either part
 * may be empty (NULL with length 0), but not both. Returns the bytes written,
 * or 0 if the frame does not fit outCap (out is then undefined, nothing past
 * outCap is touched) or the input is empty.
 */
uint16_t frameEncode(const uint8_t *head, uint16_t headLen,
                     const uint8_t *body, uint16_t bodyLen,
                     uint8_t *out, uint16_t outCap);

/*
 * Prepares d to decode into buf (cap bytes: FRAME_CODEC_RX_BUF_BYTES of the
 * largest raw frame expected). Clears the statistics.
 */
void frameDecoderInit(frameDecoder_t *d, uint8_t *buf, uint16_t cap);

/*
 * Feeds one received byte. Returns FRAME_READY when a frame completed with a
 * good CRC; d->frame / d->frameLen then hold its raw bytes, valid until the
 * next frameDecoderPush() on d. Errors are reported once, at the delimiter that
 * ends the bad frame; the decoder is already in sync for the next one.
 */
frameResult_e frameDecoderPush(frameDecoder_t *d, uint8_t byte);

/*
 * Drops any partial frame, keeps the statistics. Use when the link resets
 * (USB re-enumeration, BLE disconnect).
 */
void frameDecoderReset(frameDecoder_t *d);
