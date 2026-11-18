/*
 * Update writer. See update_writer.h.
 */

#include "update_writer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "crc32.h"

/* ctrl_status.h needs the HAL; this module is pure, so it has its own. */
#define UPDATE_TRY(expr) \
    do { \
        ctrlStatus_e _st = (expr); \
        if (_st != CTRL_OK) { \
            return _st; \
        } \
    } while (0)

static bool updateIsHeaderByte(uint32_t pos)
{
    return pos >= IMAGE_HEADER_OFFSET && pos < IMAGE_HEADER_OFFSET + IMAGE_HEADER_BYTES;
}

/* Byte pos as written so far: header bytes from RAM, the open quad-word from
 * the stage, the rest from flash. pos < next. */
static uint8_t updateWrittenByte(const updateWriter_t *w, uint32_t pos)
{
    if (updateIsHeaderByte(pos)) {
        return w->header[pos - IMAGE_HEADER_OFFSET];
    }
    if ((pos & ~(uint32_t)(UPDATE_WRITER_QW - 1U)) == (w->next & ~(uint32_t)(UPDATE_WRITER_QW - 1U))) {
        return w->qw[pos & (UPDATE_WRITER_QW - 1U)];
    }
    return w->io->slot[pos];
}

/* Programs the staged quad-word (unless it is one of the header's). */
static ctrlStatus_e updateFlushQw(updateWriter_t *w, uint32_t qwStart)
{
    ctrlStatus_e status = CTRL_OK;
    if (!updateIsHeaderByte(qwStart)) {
        status = w->io->program16(qwStart, w->qw);
    }
    memset(w->qw, 0xFF, sizeof(w->qw));
    return status;
}

static ctrlStatus_e updateAppendByte(updateWriter_t *w, uint8_t b)
{
    const uint32_t pos = w->next;
    if (updateIsHeaderByte(pos)) {
        w->header[pos - IMAGE_HEADER_OFFSET] = b;
    }
    w->qw[pos & (UPDATE_WRITER_QW - 1U)] = b;
    w->next++;
    if ((w->next & (UPDATE_WRITER_QW - 1U)) == 0U) {
        return updateFlushQw(w, pos & ~(uint32_t)(UPDATE_WRITER_QW - 1U));
    }
    return CTRL_OK;
}

void updateWriterInit(updateWriter_t *w, const updateFlashIo_t *io)
{
    memset(w, 0, sizeof(*w));
    w->io = io;
    memset(w->qw, 0xFF, sizeof(w->qw));
    memset(w->header, 0xFF, sizeof(w->header));
}

ctrlStatus_e updateWriterBegin(updateWriter_t *w, uint8_t boardType, uint32_t size, uint32_t crc32)
{
    if (boardType != w->io->boardType) {
        return CTRL_ERR_IMAGE_BOARD;
    }
    if (size > w->io->slotSize) {
        return CTRL_ERR_IMAGE_TOO_BIG;
    }
    if (size < IMAGE_HEADER_OFFSET + IMAGE_HEADER_BYTES) {
        return CTRL_ERR_INVALID_ARG;
    }
    if (w->active && w->size == size && w->crcExpect == crc32) {
        return CTRL_OK;                      /* resume: keep what is written */
    }

    const updateFlashIo_t *io = w->io;
    updateWriterInit(w, io);
    ctrlStatus_e status = io->erase(0U, size);
    if (status != CTRL_OK) {
        return status;
    }
    w->active    = true;
    w->size      = size;
    w->crcExpect = crc32;
    return CTRL_OK;
}

ctrlStatus_e updateWriterChunk(updateWriter_t *w, uint32_t offset, const uint8_t *data, uint16_t len,
                               uint32_t *nextOffset)
{
    if (!w->active) {
        return CTRL_ERR_UPDATE_SEQUENCE;
    }
    if (offset > w->size || len > w->size - offset) {
        return CTRL_ERR_IMAGE_TOO_BIG;
    }
    if (offset > w->next) {
        return CTRL_ERR_UPDATE_SEQUENCE;
    }

    /* The part below next must match what is already there. */
    uint16_t i = 0U;
    bool     repeat = false;
    for (; i < len && offset + i < w->next; i++) {
        if (updateWrittenByte(w, offset + i) != data[i]) {
            return CTRL_ERR_UPDATE_SEQUENCE;
        }
        repeat = true;
    }
    for (; i < len; i++) {
        UPDATE_TRY(updateAppendByte(w, data[i]));
    }

    w->chunks++;
    if (repeat) {
        w->repeats++;
    }
    *nextOffset = w->next;
    return CTRL_OK;
}

ctrlStatus_e updateWriterEnd(updateWriter_t *w)
{
    if (!w->active || w->next != w->size) {
        return CTRL_ERR_UPDATE_SEQUENCE;
    }

    /* The last partial quad-word, padded with erased bytes. */
    if ((w->next & (UPDATE_WRITER_QW - 1U)) != 0U) {
        for (uint32_t p = w->next & (UPDATE_WRITER_QW - 1U); p < UPDATE_WRITER_QW; p++) {
            w->qw[p] = 0xFFU;
        }
        UPDATE_TRY(updateFlushQw(w, w->next & ~(uint32_t)(UPDATE_WRITER_QW - 1U)));
    }

    /* The whole image as the host sent it: flash with the header from RAM. */
    const uint8_t *img = w->io->slot;
    uint32_t crc = crc32Update(CRC32_INIT, img, IMAGE_HEADER_OFFSET);
    crc = crc32Update(crc, w->header, IMAGE_HEADER_BYTES);
    const uint32_t after = IMAGE_HEADER_OFFSET + IMAGE_HEADER_BYTES;
    crc = crc32Final(crc32Update(crc, img + after, w->size - after));

    imageHeader_t h;
    memcpy(&h, w->header, sizeof(h));
    ctrlStatus_e status = CTRL_OK;
    if (crc != w->crcExpect) {
        status = CTRL_ERR_IMAGE_CRC;
    } else {
        status = imageHeaderCheck(&h, w->io->boardType, w->io->slotSize);
        if (status == CTRL_OK && (h.size != w->size || imageCrcSkipHeader(img, w->size) != h.imageCrc)) {
            status = CTRL_ERR_IMAGE_CRC;
        }
    }
    if (status != CTRL_OK) {
        w->active = false;                   /* the host starts over with BEGIN */
        return status;
    }

    /* Header last: from here on the bootloader boots this image. */
    UPDATE_TRY(w->io->program16(IMAGE_HEADER_OFFSET, &w->header[0]));
    UPDATE_TRY(w->io->program16(IMAGE_HEADER_OFFSET + UPDATE_WRITER_QW, &w->header[UPDATE_WRITER_QW]));
    if (memcmp(img + IMAGE_HEADER_OFFSET, w->header, IMAGE_HEADER_BYTES) != 0) {
        w->active = false;
        return CTRL_ERR_FLASH;
    }
    w->active = false;
    return CTRL_OK;
}

void updateWriterAbort(updateWriter_t *w)
{
    w->active = false;
}

bool updateWriterIsActive(const updateWriter_t *w)
{
    return w->active;
}
