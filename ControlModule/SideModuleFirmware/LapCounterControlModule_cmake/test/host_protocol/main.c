/*
 * Host test for host_protocol.h: the firmware's packed structs against the
 * golden bytes.
 *
 * gen_protocol.py packs every example in host_protocol.toml with its own
 * reference packer and writes the bytes into golden_cases.h. The same values
 * are also written as the firmware's C structs. Here those structs must lay
 * out to exactly the reference bytes (field order, widths, packing,
 * endianness), and header + struct through frame_codec must produce exactly
 * the reference frame. The web app's tests hold codec.ts / wire.ts to the same
 * bytes (golden.json), so C and TypeScript agree through a third
 * implementation.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "frame_codec.h"
#include "host_protocol.h"
#include "golden_cases.h"

static int failures;

static void check(const char *name, int passed)
{
    printf("%-60s %s\n", name, passed ? "ok" : "FAIL");
    failures += passed ? 0 : 1;
}

static void testStructLayouts(void)
{
    bool ok = true;
    for (size_t i = 0U; i < GOLDEN_MSG_COUNT; i++) {
        const goldenMsgCase_t *c = &goldenMsgCases[i];
        uint8_t payload[HP_MAX_PAYLOAD];
        uint16_t len = (uint16_t)(c->fixedLen + c->tailLen);

        if (len != c->payloadLen || len > sizeof(payload)) {
            printf("  %s: payload %u B, expected %u B\n", c->name, len, c->payloadLen);
            ok = false;
            continue;
        }
        if (c->fixedLen != 0U) {
            memcpy(payload, c->fixed, c->fixedLen);
        }
        if (c->tailLen != 0U) {
            memcpy(payload + c->fixedLen, c->tail, c->tailLen);
        }
        if (len != 0U && memcmp(payload, c->payload, len) != 0) {
            printf("  %s: struct bytes differ from the reference\n", c->name);
            ok = false;
        }
    }
    check("structs: every example lays out to the reference bytes", ok);
}

static void testFrames(void)
{
    static uint8_t rx[FRAME_CODEC_RX_BUF_BYTES(HP_MAX_FRAME_RAW)];
    frameDecoder_t dec;
    bool encOk = true, decOk = true;

    frameDecoderInit(&dec, rx, sizeof(rx));
    for (size_t i = 0U; i < GOLDEN_MSG_COUNT; i++) {
        const goldenMsgCase_t *c = &goldenMsgCases[i];
        uint8_t wire[FRAME_CODEC_WIRE_BOUND(HP_MAX_FRAME_RAW)];

        uint16_t n = frameEncode((const uint8_t *)&c->header, HP_HEADER_BYTES,
                                 c->payload, c->payloadLen, wire, sizeof(wire));
        if (n != c->wireLen || memcmp(wire, c->wire, n) != 0) {
            printf("  %s: frame differs from the reference\n", c->name);
            encOk = false;
        }

        frameResult_e r = FRAME_NONE;
        for (uint16_t k = 0U; k < c->wireLen; k++) {
            r = frameDecoderPush(&dec, c->wire[k]);
        }
        const hpHeader_t *h = (const hpHeader_t *)dec.frame;
        if (r != FRAME_READY || dec.frameLen != HP_HEADER_BYTES + c->payloadLen
            || h->msgId != c->header.msgId || h->flags != c->header.flags || h->tag != c->header.tag
            || (c->payloadLen != 0U && memcmp(dec.frame + HP_HEADER_BYTES, c->payload, c->payloadLen) != 0)) {
            printf("  %s: decoded frame differs\n", c->name);
            decOk = false;
        }
    }
    check("frames: header + struct encode to the reference frame", encOk);
    check("frames: reference frames decode to header + payload", decOk);
}

static void testLimits(void)
{
    /* The nRF link adds a channel byte in front of a full host frame. */
    check("limits: a full host frame + nRF channel byte fits 254 B on the wire",
          FRAME_CODEC_WIRE_BOUND(HP_MAX_FRAME_RAW + 1U) <= 254U);
    check("limits: NODE_LIST answers all 6 lanes + ctrl + BLE in one frame",
          HP_NODE_LIST_RSP_MAX_ITEMS >= 8U);
    check("limits: a raw-sample chunk carries at least 100 samples",
          HP_STR_RAW_SAMPLES_MAX_ITEMS >= 100U);
}

static void testNames(void)
{
    check("names: message and status names resolve",
          strcmp(hpMsgName(HP_MSG_EVT_CROSSING), "EVT_CROSSING") == 0
          && strcmp(ctrlStatusName(CTRL_ERR_NOT_BOUND), "ERR_NOT_BOUND") == 0
          && strcmp(hpMsgName(0x7FU), "?") == 0);
}

int main(void)
{
    testStructLayouts();
    testFrames();
    testLimits();
    testNames();

    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
