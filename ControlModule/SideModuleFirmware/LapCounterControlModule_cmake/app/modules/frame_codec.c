/*
 * Frame codec. See frame_codec.h for the format and the rules.
 */

#include "frame_codec.h"

#include <stddef.h>
#include <stdint.h>

/* One COBS block carries at most 254 data bytes; its code byte is then 0xFF and
 * it implies no zero after it. */
#define COBS_MAX_CODE  0xFFU

uint16_t frameCrc16(uint16_t crc, const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0U; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (uint8_t bit = 0U; bit < 8U; bit++) {
            crc = ((crc & 0x8000U) != 0U) ? (uint16_t)((crc << 1) ^ 0x1021U) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}


/* --- Encoder ---------------------------------------------------------------
 *
 * COBS over the virtual concatenation head | body | crc, so the caller never
 * has to assemble the frame in a scratch buffer. codeIdx is where the current
 * block's code byte goes once the block length is known.
 */

typedef struct {
    uint8_t *out;
    uint16_t cap;
    uint16_t pos;
    uint16_t codeIdx;
    uint8_t  code;
    bool     overflow;
} cobsWriter_t;

static void cobsWriterPut(cobsWriter_t *w, uint8_t b)
{
    if (w->pos >= w->cap) {
        w->overflow = true;
        return;
    }
    w->out[w->pos++] = b;
}

static void cobsWriterOpenBlock(cobsWriter_t *w)
{
    w->codeIdx = w->pos;
    w->code    = 1U;
    cobsWriterPut(w, 0U);           /* placeholder for the code byte */
}

static void cobsWriterCloseBlock(cobsWriter_t *w)
{
    if (!w->overflow) {
        w->out[w->codeIdx] = w->code;
    }
}

static void cobsWriterByte(cobsWriter_t *w, uint8_t b)
{
    if (b == 0U) {
        cobsWriterCloseBlock(w);
        cobsWriterOpenBlock(w);
        return;
    }
    cobsWriterPut(w, b);
    w->code++;
    if (w->code == COBS_MAX_CODE) {
        cobsWriterCloseBlock(w);
        cobsWriterOpenBlock(w);
    }
}

static void cobsWriterBytes(cobsWriter_t *w, const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0U; i < len && !w->overflow; i++) {
        cobsWriterByte(w, data[i]);
    }
}

uint16_t frameEncode(const uint8_t *head, uint16_t headLen,
                     const uint8_t *body, uint16_t bodyLen,
                     uint8_t *out, uint16_t outCap)
{
    if ((headLen + bodyLen) == 0U || out == NULL
        || (head == NULL && headLen != 0U) || (body == NULL && bodyLen != 0U)) {
        return 0U;
    }

    uint16_t crc = 0xFFFFU;
    crc = frameCrc16(crc, head, headLen);
    crc = frameCrc16(crc, body, bodyLen);
    const uint8_t crcLe[FRAME_CODEC_CRC_BYTES] = { (uint8_t)(crc & 0xFFU), (uint8_t)(crc >> 8) };

    cobsWriter_t w = { .out = out, .cap = outCap };
    cobsWriterOpenBlock(&w);
    cobsWriterBytes(&w, head, headLen);
    cobsWriterBytes(&w, body, bodyLen);
    cobsWriterBytes(&w, crcLe, FRAME_CODEC_CRC_BYTES);
    cobsWriterCloseBlock(&w);
    cobsWriterPut(&w, FRAME_CODEC_DELIMITER);

    return w.overflow ? 0U : w.pos;
}


/* --- Decoder ---------------------------------------------------------------
 *
 * Streaming COBS: each block's code byte says how many data bytes follow
 * (code - 1). A block shorter than 254 bytes stands for "data, then a zero";
 * that zero is written only when another block follows, because the last
 * block's zero is the delimiter itself.
 */

static void frameDecoderStartFrame(frameDecoder_t *d)
{
    d->len        = 0U;
    d->blockCode  = 0U;
    d->blockLeft  = 0U;
    d->discarding = false;
}

void frameDecoderInit(frameDecoder_t *d, uint8_t *buf, uint16_t cap)
{
    *d = (frameDecoder_t){ .buf = buf, .cap = cap };
    frameDecoderStartFrame(d);
}

void frameDecoderReset(frameDecoder_t *d)
{
    frameDecoderStartFrame(d);
}

static void frameDecoderAppend(frameDecoder_t *d, uint8_t b)
{
    if (d->len >= d->cap) {
        d->discarding = true;
        return;
    }
    d->buf[d->len++] = b;
}

static frameResult_e frameDecoderEnd(frameDecoder_t *d)
{
    frameResult_e result;

    if (d->discarding) {
        d->stats.overflows++;
        result = FRAME_ERR_OVERFLOW;
    } else if (d->blockCode == 0U) {
        result = FRAME_NONE;                    /* empty frame: two delimiters in a row */
    } else if (d->blockLeft != 0U) {
        d->stats.cobsErrors++;
        result = FRAME_ERR_COBS;
    } else if (d->len < FRAME_CODEC_CRC_BYTES + 1U) {
        d->stats.shortFrames++;
        result = FRAME_ERR_SHORT;
    } else {
        const uint16_t rawLen = (uint16_t)(d->len - FRAME_CODEC_CRC_BYTES);
        const uint16_t want   = (uint16_t)(d->buf[rawLen] | ((uint16_t)d->buf[rawLen + 1U] << 8));
        if (frameCrc16(0xFFFFU, d->buf, rawLen) == want) {
            d->frame    = d->buf;
            d->frameLen = rawLen;
            d->stats.frames++;
            result = FRAME_READY;
        } else {
            d->stats.crcErrors++;
            result = FRAME_ERR_CRC;
        }
    }

    frameDecoderStartFrame(d);
    return result;
}

frameResult_e frameDecoderPush(frameDecoder_t *d, uint8_t byte)
{
    if (byte == FRAME_CODEC_DELIMITER) {
        return frameDecoderEnd(d);
    }
    if (d->discarding) {
        return FRAME_NONE;
    }

    if (d->blockLeft == 0U) {
        /* A code byte. The block before it, unless full, implied a zero. */
        if (d->blockCode != 0U && d->blockCode != COBS_MAX_CODE) {
            frameDecoderAppend(d, 0U);
        }
        d->blockCode = byte;
        d->blockLeft = (uint8_t)(byte - 1U);
        return FRAME_NONE;
    }

    frameDecoderAppend(d, byte);
    d->blockLeft--;
    return FRAME_NONE;
}
