/*
 * Firmware image header. See image_header.h.
 */

#include "image_header.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "crc32.h"

ctrlStatus_e imageHeaderCheck(const imageHeader_t *h, uint8_t boardType, uint32_t slotSize)
{
    if (h->magic != IMAGE_HEADER_MAGIC || h->headerVersion != IMAGE_HEADER_VERSION
        || h->size < IMAGE_HEADER_OFFSET + IMAGE_HEADER_BYTES) {
        return CTRL_ERR_IMAGE_CRC;
    }
    if (h->boardType != boardType) {
        return CTRL_ERR_IMAGE_BOARD;
    }
    if (h->size > slotSize) {
        return CTRL_ERR_IMAGE_TOO_BIG;
    }
    return CTRL_OK;
}

uint32_t imageCrcSkipHeader(const uint8_t *image, uint32_t size)
{
    uint32_t crc = CRC32_INIT;
    if (size <= IMAGE_HEADER_OFFSET) {
        return crc32Final(crc32Update(crc, image, size));
    }
    crc = crc32Update(crc, image, IMAGE_HEADER_OFFSET);
    const uint32_t after = IMAGE_HEADER_OFFSET + IMAGE_HEADER_BYTES;
    if (size > after) {
        crc = crc32Update(crc, image + after, size - after);
    }
    return crc32Final(crc);
}

ctrlStatus_e imageCheck(const uint8_t *image, uint32_t slotSize, uint8_t boardType,
                        uint32_t imageAddr, uint32_t ramBase, uint32_t ramSize)
{
    imageHeader_t h;
    memcpy(&h, image + IMAGE_HEADER_OFFSET, sizeof(h));
    ctrlStatus_e status = imageHeaderCheck(&h, boardType, slotSize);
    if (status != CTRL_OK) {
        return status;
    }
    if (h.loadAddr != imageAddr || imageCrcSkipHeader(image, h.size) != h.imageCrc) {
        return CTRL_ERR_IMAGE_CRC;
    }

    return imageVectorsCheck(image, h.size, imageAddr, ramBase, ramSize) ? CTRL_OK : CTRL_ERR_IMAGE_CRC;
}

bool imageVectorsCheck(const uint8_t *image, uint32_t size, uint32_t imageAddr, uint32_t ramBase, uint32_t ramSize)
{
    uint32_t sp;
    uint32_t reset;
    memcpy(&sp, image, sizeof(sp));
    memcpy(&reset, image + 4, sizeof(reset));
    const bool spOk    = (sp & 3U) == 0U && sp > ramBase && sp <= ramBase + ramSize;
    const bool resetOk = (reset & 1U) != 0U && reset > imageAddr && reset < imageAddr + size;
    return spOk && resetOk;
}
