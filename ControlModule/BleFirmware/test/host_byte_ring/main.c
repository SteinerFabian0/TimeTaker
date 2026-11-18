/*
 * Host test for byte_ring.c: the UART RX/TX rings.
 *
 * Init rules, partial and all-or-nothing writes, wrap-around with the
 * contiguous peek (what the UARTE DMA sends from), index wrap past 65535,
 * a long randomised producer/consumer run against a reference queue.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../check.h"
#include "byte_ring.h"

static uint32_t rngState = 0x9E3779B9U;
static uint32_t rng(void)
{
    rngState ^= rngState << 13;
    rngState ^= rngState >> 17;
    rngState ^= rngState << 5;
    return rngState;
}

int main(void)
{
    byteRing_t r;
    uint8_t    buf[16];
    uint8_t    out[64];
    const uint8_t *p;

    /* Init rules. */
    check("init: size 12 (not a power of two) refused", !byteRingInit(&r, buf, 12));
    check("init: size 0 refused", !byteRingInit(&r, buf, 0));
    check("init: NULL buffer refused", !byteRingInit(&r, NULL, 16));
    check("init: size 16 accepted", byteRingInit(&r, buf, 16));
    check("init: empty", byteRingUsed(&r) == 0 && byteRingFree(&r) == 16);
    check("init: peek empty", byteRingPeek(&r, &p) == 0);

    /* Partial write: as much as fits. */
    uint8_t src[40];
    for (unsigned i = 0; i < sizeof(src); i++) {
        src[i] = (uint8_t)(i + 1);
    }
    check("write 20 into 16: 16 written", byteRingWrite(&r, src, 20) == 16);
    check("full: holds size bytes, not size-1", byteRingUsed(&r) == 16 && byteRingFree(&r) == 0);
    check("full: write 1 writes 0", byteRingWrite(&r, src, 1) == 0);
    check("read back in order", byteRingRead(&r, out, 64) == 16 && memcmp(out, src, 16) == 0);

    /* All-or-nothing write. */
    byteRingWrite(&r, src, 10);
    check("writeAll 7 into 6 free: refused, nothing written",
          !byteRingWriteAll(&r, src, 7) && byteRingUsed(&r) == 10);
    check("writeAll 6 into 6 free: accepted", byteRingWriteAll(&r, src, 6) && byteRingFree(&r) == 0);
    byteRingClear(&r);
    check("clear: empty", byteRingUsed(&r) == 0);

    /* Wrap: tail at 12, write 8 -> 4 at the end, 4 at the start. */
    byteRingInit(&r, buf, 16);
    byteRingWrite(&r, src, 12);
    byteRingRead(&r, out, 12);
    byteRingWrite(&r, &src[20], 8);
    uint16_t n = byteRingPeek(&r, &p);
    check("peek at the wrap: only the contiguous 4", n == 4 && p == &buf[12] && memcmp(p, &src[20], 4) == 0);
    byteRingConsume(&r, n);
    n = byteRingPeek(&r, &p);
    check("peek after consume: the other 4 from the start", n == 4 && p == &buf[0] && memcmp(p, &src[24], 4) == 0);
    byteRingConsume(&r, 100);
    check("consume more than used: clamps to empty", byteRingUsed(&r) == 0);

    /* Index wrap past 65535. */
    byteRingInit(&r, buf, 16);
    r.head = r.tail = 65530;
    byteRingWrite(&r, src, 12);
    check("index wrap: used 12", byteRingUsed(&r) == 12);
    check("index wrap: data intact", byteRingRead(&r, out, 12) == 12 && memcmp(out, src, 12) == 0);

    /* Randomised run against a reference FIFO. */
    {
        static uint8_t big[512];
        static uint8_t ref[1 << 20];
        uint32_t refHead = 0, refTail = 0;
        uint8_t  next = 0;
        bool     ok = true;
        byteRingInit(&r, big, sizeof(big));

        for (int step = 0; step < 200000 && ok; step++) {
            uint8_t chunk[80];
            const uint16_t want = (uint16_t)(rng() % sizeof(chunk));
            if (rng() & 1U) {
                for (uint16_t i = 0; i < want; i++) {
                    chunk[i] = next++;
                }
                const uint16_t w = byteRingWrite(&r, chunk, want);
                for (uint16_t i = 0; i < w; i++) {
                    ref[refHead++ % sizeof(ref)] = chunk[i];
                }
                next = (uint8_t)(next - (want - w));
            } else if (rng() & 1U) {
                const uint16_t got = byteRingRead(&r, chunk, want);
                for (uint16_t i = 0; i < got && ok; i++) {
                    ok = (chunk[i] == ref[refTail++ % sizeof(ref)]);
                }
            } else {
                const uint16_t got = byteRingPeek(&r, &p);
                const uint16_t take = (got < want) ? got : want;
                for (uint16_t i = 0; i < take && ok; i++) {
                    ok = (p[i] == ref[refTail++ % sizeof(ref)]);
                }
                byteRingConsume(&r, take);
            }
            ok = ok && (byteRingUsed(&r) == (uint16_t)(refHead - refTail));
        }
        check("200000 random steps match a reference FIFO", ok);
    }

    return checkSummary("host_byte_ring");
}
