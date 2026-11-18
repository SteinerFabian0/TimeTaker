/*
 * Update writer: UPDATE_BEGIN / CHUNK / END / ABORT as a state machine over
 * a flash slot (CONTROL_FIRMWARE_GUIDE §5.7, SYSTEM_ARCHITECTURE §10.1).
 *
 *   BEGIN   board type and size checked, slot pages erased. A BEGIN with the
 *           same size and crc as the update in progress resumes it (nothing
 *           erased, the write position kept).
 *   CHUNK   offset-addressed. offset == next: written. A chunk wholly or
 *           partly below next is compared with what was written; equal bytes
 *           are acknowledged again (retries and restarts are safe: the
 *           response's nextOffset tells the host where to go on), different
 *           bytes are ERR_UPDATE_SEQUENCE. offset > next: ERR_UPDATE_SEQUENCE.
 *   END     every byte received; the CRC over the whole image (header
 *           included, as UPDATE_BEGIN named it) and the header's own checks
 *           pass; only then the two header quad-words are programmed. Until
 *           that moment the slot holds no valid image, so an interrupted
 *           update leaves the bootloader in charge: it cannot brick.
 *   ABORT   forgets the update. The slot stays without a valid header.
 *
 * Flash is programmed in 16-byte quad-words (the C5's unit). Bytes are staged
 * until a quad-word is full; END pads the last one with 0xFF. The header's
 * quad-words are never programmed before END: their bytes wait in RAM.
 *
 * Pure logic: the flash comes in as function pointers plus a memory-mapped
 * read view, so the host test runs it against an array. Main loop only.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "host_protocol.h"
#include "image_header.h"

#define UPDATE_WRITER_QW  16U

typedef struct {
    /* Erases whole pages covering [offset, offset + len) of the slot. */
    ctrlStatus_e (*erase)(uint32_t offset, uint32_t len);
    /* Programs one quad-word at a 16-aligned slot offset. */
    ctrlStatus_e (*program16)(uint32_t offset, const uint8_t data[UPDATE_WRITER_QW]);
    const uint8_t *slot;      /* read view of the slot */
    uint32_t       slotSize;
    uint8_t        boardType; /* the board this writer accepts images for */
} updateFlashIo_t;

typedef struct {
    const updateFlashIo_t *io;
    bool     active;
    uint32_t size;
    uint32_t crcExpect;
    uint32_t next;                          /* bytes received in order */
    uint8_t  qw[UPDATE_WRITER_QW];          /* the quad-word at next & ~15 */
    uint8_t  header[IMAGE_HEADER_BYTES];    /* header bytes, held back until END */
    uint32_t chunks;
    uint32_t repeats;                       /* chunks acknowledged again */
} updateWriter_t;

void updateWriterInit(updateWriter_t *w, const updateFlashIo_t *io);

ctrlStatus_e updateWriterBegin(updateWriter_t *w, uint8_t boardType, uint32_t size, uint32_t crc32);

ctrlStatus_e updateWriterChunk(updateWriter_t *w, uint32_t offset, const uint8_t *data, uint16_t len,
                               uint32_t *nextOffset);

ctrlStatus_e updateWriterEnd(updateWriter_t *w);

void updateWriterAbort(updateWriter_t *w);

bool updateWriterIsActive(const updateWriter_t *w);
