/*
 * Host test for image_header.c and update_writer.c: the bootloader's image
 * checks and UPDATE_BEGIN/CHUNK/END/ABORT against a fake flash
 * (CONTROL_FIRMWARE_GUIDE §5.7). The fake flash enforces the real rules: only
 * erased quad-words may be programmed, erase works on whole pages.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "crc32.h"
#include "image_header.h"
#include "update_writer.h"
#include "../test_check.h"

#define SLOT   IMAGE_APP_SLOT_SIZE
#define PAGE   IMAGE_PAGE_SIZE
#define RAM    0x20000000U

static uint8_t  flash[SLOT];
static int      overProgram;    /* programs onto non-erased bytes */
static int      programs;
static int      erases;
static int      failProgram;
static uint32_t lastProgram;    /* offset of the latest program16 */

static ctrlStatus_e fakeErase(uint32_t off, uint32_t len)
{
    uint32_t start = off / PAGE * PAGE;
    uint32_t end = (off + len + PAGE - 1U) / PAGE * PAGE;
    memset(&flash[start], 0xFF, end - start);
    erases++;
    return CTRL_OK;
}

static ctrlStatus_e fakeProgram(uint32_t off, const uint8_t d[16])
{
    if (failProgram) {
        return CTRL_ERR_FLASH;
    }
    for (int i = 0; i < 16; i++) {
        if (flash[off + i] != 0xFF) {
            overProgram++;
        }
    }
    if ((off & 15U) != 0U) {
        overProgram++;
    }
    memcpy(&flash[off], d, 16);
    programs++;
    lastProgram = off;
    return CTRL_OK;
}

static const updateFlashIo_t io = { fakeErase, fakeProgram, flash, SLOT, HP_BOARD_CONTROL, IMAGE_APP_BASE, RAM, 0x10000U };

static uint8_t img[20000];
static uint32_t imgSize;

/* A plausible image: vectors, header at +0x400 (stamped like image_stamp.py). */
static uint32_t buildImage(uint32_t size, uint8_t board)
{
    for (uint32_t i = 0; i < size; i++) {
        img[i] = (uint8_t)(i * 7U + 3U);
    }
    uint32_t sp = RAM + 0x10000U, reset = IMAGE_APP_BASE + 0x801U;
    memcpy(img, &sp, 4);
    memcpy(img + 4, &reset, 4);
    imageHeader_t h = { .magic = IMAGE_HEADER_MAGIC, .boardType = board, .fwMajor = 1, .gitRev = 0x1234567,
                        .size = size, .headerVersion = IMAGE_HEADER_VERSION,
                        .loadAddr = IMAGE_APP_BASE };
    memcpy(img + IMAGE_HEADER_OFFSET, &h, sizeof(h));
    h.imageCrc = imageCrcSkipHeader(img, size);
    memcpy(img + IMAGE_HEADER_OFFSET, &h, sizeof(h));
    imgSize = size;
    return crc32Compute(img, size);
}

static ctrlStatus_e sendAll(updateWriter_t *w, uint16_t chunk)
{
    uint32_t next = 0;
    for (uint32_t off = 0; off < imgSize; off += chunk) {
        uint16_t n = (uint16_t)((imgSize - off < chunk) ? imgSize - off : chunk);
        ctrlStatus_e s = updateWriterChunk(w, off, img + off, n, &next);
        if (s != CTRL_OK) {
            return s;
        }
    }
    return CTRL_OK;
}

static void testHeader(void)
{
    buildImage(5000, HP_BOARD_CONTROL);
    imageHeader_t h;
    memcpy(&h, img + IMAGE_HEADER_OFFSET, sizeof(h));
    check("header: good", imageHeaderCheck(&h, HP_BOARD_CONTROL, SLOT) == CTRL_OK);
    imageHeader_t b = h; b.magic ^= 1U;
    check("header: bad magic -> ERR_IMAGE_CRC", imageHeaderCheck(&b, HP_BOARD_CONTROL, SLOT) == CTRL_ERR_IMAGE_CRC);
    b = h; b.headerVersion = 2;
    check("header: other version -> ERR_IMAGE_CRC", imageHeaderCheck(&b, HP_BOARD_CONTROL, SLOT) == CTRL_ERR_IMAGE_CRC);
    b = h; b.size = IMAGE_HEADER_OFFSET + IMAGE_HEADER_BYTES - 1U;
    check("header: size cannot hold the header -> ERR_IMAGE_CRC", imageHeaderCheck(&b, HP_BOARD_CONTROL, SLOT) == CTRL_ERR_IMAGE_CRC);
    b = h; b.boardType = HP_BOARD_LANE;
    check("header: lane image -> ERR_IMAGE_BOARD", imageHeaderCheck(&b, HP_BOARD_CONTROL, SLOT) == CTRL_ERR_IMAGE_BOARD);
    b = h; b.size = SLOT + 1U;
    check("header: too big for the slot -> ERR_IMAGE_TOO_BIG", imageHeaderCheck(&b, HP_BOARD_CONTROL, SLOT) == CTRL_ERR_IMAGE_TOO_BIG);

    b = h; b.loadAddr = IMAGE_FLASH_BASE;
    memcpy(img + IMAGE_HEADER_OFFSET, &b, sizeof(b));
    check("image: header linked for another address -> ERR_IMAGE_CRC",
          imageCheck(img, SLOT, HP_BOARD_CONTROL, IMAGE_APP_BASE, RAM, 0x10000U) == CTRL_ERR_IMAGE_CRC);
    memcpy(img + IMAGE_HEADER_OFFSET, &h, sizeof(h));
    check("image: full check of a good image", imageCheck(img, SLOT, HP_BOARD_CONTROL, IMAGE_APP_BASE, RAM, 0x10000U) == CTRL_OK);
    img[3000] ^= 0x40U;
    check("image: a flipped bit -> ERR_IMAGE_CRC", imageCheck(img, SLOT, HP_BOARD_CONTROL, IMAGE_APP_BASE, RAM, 0x10000U) == CTRL_ERR_IMAGE_CRC);
    buildImage(5000, HP_BOARD_CONTROL);
    uint32_t badSp = RAM + 0x20000U;
    memcpy(img, &badSp, 4);
    memcpy(&h, img + IMAGE_HEADER_OFFSET, sizeof(h));
    h.imageCrc = imageCrcSkipHeader(img, 5000);
    memcpy(img + IMAGE_HEADER_OFFSET, &h, sizeof(h));
    check("image: initial SP outside RAM -> refused", imageCheck(img, SLOT, HP_BOARD_CONTROL, IMAGE_APP_BASE, RAM, 0x10000U) == CTRL_ERR_IMAGE_CRC);
    buildImage(5000, HP_BOARD_CONTROL);
    uint32_t armReset = IMAGE_APP_BASE + 0x800U;
    memcpy(img + 4, &armReset, 4);
    memcpy(&h, img + IMAGE_HEADER_OFFSET, sizeof(h));
    h.imageCrc = imageCrcSkipHeader(img, 5000);
    memcpy(img + IMAGE_HEADER_OFFSET, &h, sizeof(h));
    check("image: reset vector without the Thumb bit -> refused", imageCheck(img, SLOT, HP_BOARD_CONTROL, IMAGE_APP_BASE, RAM, 0x10000U) == CTRL_ERR_IMAGE_CRC);
}

static void testUpdate(void)
{
    updateWriter_t w;
    memset(flash, 0, sizeof(flash));
    overProgram = programs = erases = 0;
    updateWriterInit(&w, &io);
    uint32_t crc = buildImage(10003, HP_BOARD_CONTROL);
    uint32_t next = 0;

    check("chunk before BEGIN -> ERR_UPDATE_SEQUENCE",
          updateWriterChunk(&w, 0, img, 16, &next) == CTRL_ERR_UPDATE_SEQUENCE);
    check("begin: lane image -> ERR_IMAGE_BOARD", updateWriterBegin(&w, HP_BOARD_LANE, 10003, crc) == CTRL_ERR_IMAGE_BOARD);
    check("begin: too big -> ERR_IMAGE_TOO_BIG", updateWriterBegin(&w, HP_BOARD_CONTROL, SLOT + 1U, crc) == CTRL_ERR_IMAGE_TOO_BIG);
    check("begin: size 0 -> ERR_INVALID_ARG", updateWriterBegin(&w, HP_BOARD_CONTROL, 0, crc) == CTRL_ERR_INVALID_ARG);
    {
        /* Too small for a header: accepted, written, refused at END. */
        uint32_t c8 = crc32Compute(img, 8);
        check("begin/end: 8-byte image refused at END -> ERR_IMAGE_CRC",
              updateWriterBegin(&w, HP_BOARD_CONTROL, 8, c8) == CTRL_OK
              && updateWriterChunk(&w, 0, img, 8, &next) == CTRL_OK && next == 8
              && updateWriterEnd(&w) == CTRL_ERR_IMAGE_CRC && flash[IMAGE_HEADER_OFFSET] == 0xFF);
        erases = 0;
    }
    check("begin: OK erases the pages it needs",
          updateWriterBegin(&w, HP_BOARD_CONTROL, 10003, crc) == CTRL_OK && erases == 1 && flash[0] == 0xFF && flash[2 * PAGE - 1] == 0xFF);

    check("chunk: in order", updateWriterChunk(&w, 0, img, 224, &next) == CTRL_OK && next == 224);
    check("chunk: the same chunk again is acknowledged", updateWriterChunk(&w, 0, img, 224, &next) == CTRL_OK && next == 224);
    uint8_t wrong[224];
    memcpy(wrong, img, 224);
    wrong[100] ^= 1U;
    check("chunk: a repeated chunk with other bytes -> ERR_UPDATE_SEQUENCE",
          updateWriterChunk(&w, 0, wrong, 224, &next) == CTRL_ERR_UPDATE_SEQUENCE);
    check("chunk: a gap -> ERR_UPDATE_SEQUENCE", updateWriterChunk(&w, 448, img + 448, 224, &next) == CTRL_ERR_UPDATE_SEQUENCE);
    check("chunk: past the image end -> ERR_IMAGE_TOO_BIG",
          updateWriterChunk(&w, 224, img + 224, (uint16_t)(10003U - 224U + 1U), &next) == CTRL_ERR_IMAGE_TOO_BIG);
    check("chunk: offset past the image end -> ERR_UPDATE_SEQUENCE",
          updateWriterChunk(&w, 10000, img, 16, &next) == CTRL_ERR_UPDATE_SEQUENCE);
    check("chunk: overlapping chunk: old part compared, new part written",
          updateWriterChunk(&w, 200, img + 200, 100, &next) == CTRL_OK && next == 300);
    check("end: before every byte arrived -> ERR_UPDATE_SEQUENCE", updateWriterEnd(&w) == CTRL_ERR_UPDATE_SEQUENCE);

    /* The rest, in odd-sized chunks (3 bytes like the TOML example, then 224). */
    updateWriterChunk(&w, 300, img + 300, 3, &next);
    uint32_t off = 303;
    while (off < imgSize) {
        uint16_t n = (uint16_t)((imgSize - off < 224U) ? imgSize - off : 224U);
        updateWriterChunk(&w, off, img + off, n, &next);
        off += n;
    }
    check("chunk: all received", next == 10003);
    check("header not programmed before END", flash[IMAGE_HEADER_OFFSET] == 0xFF && flash[IMAGE_HEADER_OFFSET + 31] == 0xFF);
    check("end: OK", updateWriterEnd(&w) == CTRL_OK && !updateWriterIsActive(&w));
    check("end: the header's magic quad-word is programmed last", lastProgram == IMAGE_HEADER_OFFSET);
    check("end: a repeated END (first answer lost) answers OK", updateWriterEnd(&w) == CTRL_OK);
    check("end: flash holds the image, last quad-word padded erased",
          memcmp(flash, img, 10003) == 0 && flash[10003] == 0xFF && flash[10015] == 0xFF);
    check("end: only erased quad-words were ever programmed", overProgram == 0);
    check("end: the image now boots", imageCheck(flash, SLOT, HP_BOARD_CONTROL, IMAGE_APP_BASE, RAM, 0x10000U) == CTRL_OK);
    check("chunk after END -> ERR_UPDATE_SEQUENCE", updateWriterChunk(&w, 0, img, 16, &next) == CTRL_ERR_UPDATE_SEQUENCE);
}

static void testFailures(void)
{
    updateWriter_t w;
    updateWriterInit(&w, &io);
    uint32_t crc = buildImage(8000, HP_BOARD_CONTROL);

    updateWriterBegin(&w, HP_BOARD_CONTROL, 8000, crc ^ 1U);
    sendAll(&w, 224);
    check("end: CRC differs from BEGIN -> ERR_IMAGE_CRC, header stays erased",
          updateWriterEnd(&w) == CTRL_ERR_IMAGE_CRC && flash[IMAGE_HEADER_OFFSET] == 0xFF && !updateWriterIsActive(&w));

    /* A lane image, BEGIN tricked with board CONTROL: the header catches it. */
    crc = buildImage(8000, HP_BOARD_LANE);
    updateWriterBegin(&w, HP_BOARD_CONTROL, 8000, crc);
    sendAll(&w, 224);
    check("end: header for another board -> ERR_IMAGE_BOARD, not booted",
          updateWriterEnd(&w) == CTRL_ERR_IMAGE_BOARD && flash[IMAGE_HEADER_OFFSET] == 0xFF);

    /* Header imageCrc wrong although the whole-file CRC matches. */
    buildImage(8000, HP_BOARD_CONTROL);
    img[IMAGE_HEADER_OFFSET + 16] ^= 1U;
    crc = crc32Compute(img, 8000);
    updateWriterBegin(&w, HP_BOARD_CONTROL, 8000, crc);
    sendAll(&w, 224);
    check("end: header imageCrc wrong -> ERR_IMAGE_CRC", updateWriterEnd(&w) == CTRL_ERR_IMAGE_CRC);

    /* Resume: BEGIN again with the same image keeps the position. */
    crc = buildImage(8000, HP_BOARD_CONTROL);
    erases = 0;
    updateWriterBegin(&w, HP_BOARD_CONTROL, 8000, crc);
    uint32_t next = 0;
    updateWriterChunk(&w, 0, img, 224, &next);
    updateWriterChunk(&w, 224, img + 224, 224, &next);
    check("resume: same BEGIN keeps what is written, no erase",
          updateWriterBegin(&w, HP_BOARD_CONTROL, 8000, crc) == CTRL_OK && erases == 1
          && updateWriterChunk(&w, 0, img, 224, &next) == CTRL_OK && next == 448);
    check("resume: a host restarting from 0 is fast-forwarded and finishes",
          sendAll(&w, 224) == CTRL_OK && updateWriterEnd(&w) == CTRL_OK);

    updateWriterBegin(&w, HP_BOARD_CONTROL, 8000, crc ^ 2U);
    check("begin: another image restarts with an erase", erases == 2 && flash[0] == 0xFF);
    updateWriterAbort(&w);
    check("abort: update forgotten", !updateWriterIsActive(&w) && updateWriterEnd(&w) == CTRL_ERR_UPDATE_SEQUENCE);

    updateWriterBegin(&w, HP_BOARD_CONTROL, 8000, crc);
    failProgram = 1;
    check("flash failure surfaces", sendAll(&w, 224) == CTRL_ERR_FLASH);
    failProgram = 0;
    uint32_t next2 = 0;
    check("flash failure drops the update (no chunk acknowledged past lost bytes)",
          !updateWriterIsActive(&w) && updateWriterChunk(&w, 0, img, 224, &next2) == CTRL_ERR_UPDATE_SEQUENCE);

    /* An image linked for 0x0800_0000 (the standalone .bin): header and CRC
     * fine, but it would not run at 0x0800_8000. */
    buildImage(8000, HP_BOARD_CONTROL);
    uint32_t standaloneReset = IMAGE_FLASH_BASE + 0x801U;
    memcpy(img + 4, &standaloneReset, 4);
    imageHeader_t h;
    memcpy(&h, img + IMAGE_HEADER_OFFSET, sizeof(h));
    h.imageCrc = imageCrcSkipHeader(img, 8000);
    memcpy(img + IMAGE_HEADER_OFFSET, &h, sizeof(h));
    crc = crc32Compute(img, 8000);
    updateWriterBegin(&w, HP_BOARD_CONTROL, 8000, crc);
    sendAll(&w, 224);
    check("end: image linked for another address -> ERR_IMAGE_CRC, header stays erased",
          updateWriterEnd(&w) == CTRL_ERR_IMAGE_CRC && flash[IMAGE_HEADER_OFFSET] == 0xFF);
}

/* The real build output: image_stamp.py and the bootloader agree byte for
 * byte. Skipped when the preset was not built yet (check_all.sh builds it). */
static uint8_t realBin[IMAGE_APP_SLOT_SIZE + 1U];

static long loadBin(const char *rel)
{
    char path[512];
    const char *file = __FILE__;
    const char *cut = strstr(file, "test/host_update/main.c");
    if (cut == NULL) {
        return -1;
    }
    snprintf(path, sizeof(path), "%.*s%s", (int)(cut - file), file, rel);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }
    long n = (long)fread(realBin, 1, sizeof(realBin), f);
    fclose(f);
    return n;
}

static void testRealImages(void)
{
    long n = loadBin("build/debug_GCC_boot/LapCounterControlModule.bin");
    if (n < 0) {
        printf("  (skipped: build/debug_GCC_boot not built)\n");
    } else {
        const uint32_t size = (uint32_t)n;
        check("real boot .bin: fits the slot", size <= IMAGE_APP_SLOT_SIZE);
        check("real boot .bin: imageCheck passes (stamp == bootloader check)",
              imageCheck(realBin, SLOT, HP_BOARD_CONTROL, IMAGE_APP_BASE, RAM, 0x10000U) == CTRL_OK);
        imageHeader_t h;
        memcpy(&h, realBin + IMAGE_HEADER_OFFSET, sizeof(h));
        check("real boot .bin: header size is the file size", h.size == size);

        /* The whole update over it, as the web app sends it. */
        updateWriter_t w;
        updateWriterInit(&w, &io);
        uint32_t next = 0;
        ctrlStatus_e s = updateWriterBegin(&w, HP_BOARD_CONTROL, size, crc32Compute(realBin, size));
        for (uint32_t off = 0; s == CTRL_OK && off < size; off += 224U) {
            s = updateWriterChunk(&w, off, realBin + off, (uint16_t)((size - off < 224U) ? size - off : 224U), &next);
        }
        check("real boot .bin: BEGIN/CHUNK/END accepted", s == CTRL_OK && updateWriterEnd(&w) == CTRL_OK);
        check("real boot .bin: flashed slot boots",
              imageCheck(flash, SLOT, HP_BOARD_CONTROL, IMAGE_APP_BASE, RAM, 0x10000U) == CTRL_OK);
    }

    n = loadBin("build/debug_GCC_main/LapCounterControlModule.bin");
    if (n >= 0) {
        check("real standalone .bin: refused at 0x0800_8000 (loadAddr)",
              imageCheck(realBin, SLOT, HP_BOARD_CONTROL, IMAGE_APP_BASE, RAM, 0x10000U) == CTRL_ERR_IMAGE_CRC);
        check("real standalone .bin: header and CRC themselves are fine",
              imageCheck(realBin, SLOT, HP_BOARD_CONTROL, IMAGE_FLASH_BASE, RAM, 0x10000U) == CTRL_OK);
    }
}

int main(void)
{
    testHeader();
    testUpdate();
    testFailures();
    testRealImages();
    return finish();
}
