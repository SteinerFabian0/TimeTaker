/*
 * Host test for the pure half of the nRF link: nrf_link_proto.c (framing,
 * CTRL length rules, credits, version check), byte_ring.c (the ISR/loop
 * rings) and stream_budget.c (the BLE stream cap). BLE_FIRMWARE_GUIDE §5.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "byte_ring.h"
#include "frame_codec.h"
#include "nrf_link_proto.h"
#include "stream_budget.h"
#include "../test_check.h"

/* Decodes one wire frame back to its raw bytes. */
static uint16_t decode(const uint8_t *wire, uint16_t n, uint8_t *raw)
{
    static uint8_t buf[FRAME_CODEC_RX_BUF_BYTES(BLE_LINK_RAW_MAX)];
    frameDecoder_t d;
    frameDecoderInit(&d, buf, sizeof(buf));
    for (uint16_t i = 0; i < n; i++) {
        if (frameDecoderPush(&d, wire[i]) == FRAME_READY) {
            memcpy(raw, d.frame, d.frameLen);
            return d.frameLen;
        }
    }
    return 0;
}

static void testParse(void)
{
    nrfLinkMsg_t m;
    uint8_t raw[300];

    raw[0] = BLE_LINK_CH_DATA; raw[1] = 0xAB; raw[2] = 0x00;
    check("parse: DATA with bytes", nrfLinkProtoParse(raw, 3, &m) == NRF_LINK_PARSE_DATA
          && m.dataLen == 2 && m.data[0] == 0xAB);
    check("parse: empty DATA dropped", nrfLinkProtoParse(raw, 1, &m) == NRF_LINK_PARSE_BAD_DATA);
    memset(raw + 1, 1, 245);
    check("parse: DATA of 244 accepted", nrfLinkProtoParse(raw, 245, &m) == NRF_LINK_PARSE_DATA);
    check("parse: DATA of 245 dropped", nrfLinkProtoParse(raw, 246, &m) == NRF_LINK_PARSE_BAD_DATA);

    raw[0] = 7;
    check("parse: unknown channel", nrfLinkProtoParse(raw, 3, &m) == NRF_LINK_PARSE_BAD_CHANNEL);
    check("parse: no channel byte", nrfLinkProtoParse(raw, 0, &m) == NRF_LINK_PARSE_BAD_CHANNEL);

    bleCtrlNrfHello_t h = { .linkMajor = BLE_LINK_MAJOR, .linkMinor = 9, .credits = 4, .fwGitRev = 0x1234567 };
    raw[0] = BLE_LINK_CH_CTRL; raw[1] = BLE_CTRL_NRF_HELLO;
    memcpy(raw + 2, &h, sizeof(h));
    check("parse: NRF_HELLO", nrfLinkProtoParse(raw, (uint16_t)(2 + sizeof(h)), &m) == NRF_LINK_PARSE_CTRL
          && m.ctrlType == BLE_CTRL_NRF_HELLO && m.ctrl.hello.credits == 4 && m.ctrl.hello.fwGitRev == 0x1234567U);
    check("parse: CTRL one byte short -> dropped",
          nrfLinkProtoParse(raw, (uint16_t)(1 + sizeof(h)), &m) == NRF_LINK_PARSE_SHORT_CTRL);
    raw[2 + sizeof(h)] = 0x77;
    check("parse: CTRL longer (newer MINOR) -> accepted",
          nrfLinkProtoParse(raw, (uint16_t)(3 + sizeof(h)), &m) == NRF_LINK_PARSE_CTRL && m.ctrl.hello.credits == 4);
    check("parse: CTRL without a type -> dropped", nrfLinkProtoParse(raw, 1, &m) == NRF_LINK_PARSE_SHORT_CTRL);
    raw[1] = 0x3F;
    check("parse: unknown CTRL type -> ignored", nrfLinkProtoParse(raw, 4, &m) == NRF_LINK_PARSE_UNKNOWN_CTRL);
    raw[1] = BLE_CTRL_UI_STATE;
    check("parse: an STM->nRF type from the nRF -> ignored", nrfLinkProtoParse(raw, 4, &m) == NRF_LINK_PARSE_UNKNOWN_CTRL);
    raw[1] = BLE_CTRL_CREDIT; raw[2] = 3;
    check("parse: CREDIT", nrfLinkProtoParse(raw, 3, &m) == NRF_LINK_PARSE_CTRL && m.ctrl.credit.count == 3);

    check("version: same MAJOR ok", nrfLinkProtoVersionOk(&h));
    h.linkMajor = BLE_LINK_MAJOR + 1U;
    check("version: other MAJOR refused", !nrfLinkProtoVersionOk(&h));
}

static void testEncode(void)
{
    uint8_t wire[NRF_LINK_WIRE_MAX];
    uint8_t raw[300];
    uint8_t data[244];
    for (int i = 0; i < 244; i++) {
        data[i] = (uint8_t)i;   /* includes zeros: COBS must cope */
    }
    uint16_t n = nrfLinkProtoEncodeData(data, 244, wire, sizeof(wire));
    uint16_t r = decode(wire, n, raw);
    check("encode: DATA 244 fits NRF_LINK_WIRE_MAX and round-trips",
          n > 0 && n <= NRF_LINK_WIRE_MAX && r == 245 && raw[0] == BLE_LINK_CH_DATA && memcmp(raw + 1, data, 244) == 0);
    check("encode: DATA 0 refused", nrfLinkProtoEncodeData(data, 0, wire, sizeof(wire)) == 0);
    check("encode: DATA 245 refused", nrfLinkProtoEncodeData(data, 245, wire, sizeof(wire)) == 0);
    bleCtrlUiState_t ui = { .session = BLE_UI_SESSION_BOUND, .pairWindow = 1 };
    n = nrfLinkProtoEncodeCtrl(BLE_CTRL_UI_STATE, &ui, sizeof(ui), wire, sizeof(wire));
    r = decode(wire, n, raw);
    check("encode: CTRL = channel, type, body", r == 4 && raw[0] == BLE_LINK_CH_CTRL && raw[1] == BLE_CTRL_UI_STATE
          && raw[2] == BLE_UI_SESSION_BOUND && raw[3] == 1);
    n = nrfLinkProtoEncodeCtrl(BLE_CTRL_KICK, NULL, 0, wire, sizeof(wire));
    check("encode: CTRL with an empty body", decode(wire, n, raw) == 2);
}

static void testCredits(void)
{
    nrfLinkCredits_t c = { 0 };
    check("credits: none before NRF_HELLO", !nrfLinkCreditsTake(&c));
    nrfLinkCreditsReset(&c, 2);
    check("credits: HELLO grants its count", nrfLinkCreditsTake(&c) && nrfLinkCreditsTake(&c) && !nrfLinkCreditsTake(&c));
    nrfLinkCreditsReturn(&c, 1);
    check("credits: CREDIT returns one", nrfLinkCreditsTake(&c) && !nrfLinkCreditsTake(&c));
    nrfLinkCreditsReturn(&c, 5);
    check("credits: returns above the HELLO grant are kept (busy slots come back later)",
          c.credits == 5 && c.overReturned == 0);
    nrfLinkCreditsReset(&c, 1);
    nrfLinkCreditsReturn(&c, 3);
    check("credits: grant 1 then CREDIT 3 -> 4 (nRF had 4 slots, 3 busy)", c.credits == 4 && c.overReturned == 0);
    nrfLinkCreditsReturn(&c, 255);
    check("credits: uint8 overflow is clamped and counted", c.credits == 255 && c.overReturned == 1);
    nrfLinkCreditsTake(&c);
    nrfLinkCreditsReset(&c, 4);
    check("credits: a new HELLO resets to its value", c.credits == 4 && c.granted == 4);
}

static void testRing(void)
{
    byteRing_t r;
    uint8_t buf[16];
    uint8_t out[32];
    check("ring: size not a power of two refused", !byteRingInit(&r, buf, 12));
    byteRingInit(&r, buf, 16);
    check("ring: capacity is size - 1", byteRingFree(&r) == 15);
    uint8_t in[20];
    for (int i = 0; i < 20; i++) in[i] = (uint8_t)(i + 1);
    check("ring: put of more than free refused, nothing taken", !byteRingPut(&r, in, 16) && byteRingCount(&r) == 0);
    byteRingPut(&r, in, 10);
    check("ring: get in order", byteRingGet(&r, out, 4) == 4 && out[0] == 1 && out[3] == 4 && byteRingCount(&r) == 6);
    byteRingPut(&r, in + 10, 9);   /* wraps */
    check("ring: wraps around", byteRingCount(&r) == 15 && byteRingFree(&r) == 0);
    check("ring: peek leaves bytes", byteRingPeek(&r, out, 3) == 3 && out[0] == 5 && byteRingCount(&r) == 15);
    byteRingDrop(&r, 2);
    uint16_t n = byteRingGet(&r, out, 32);
    check("ring: the rest in order across the wrap", n == 13 && out[0] == 7 && out[12] == 19);
}

static void testBudget(void)
{
    streamBudget_t b;
    streamBudgetInit(&b, 0, 0, 0);
    check("budget: rate 0 = unlimited", streamBudgetTake(&b, 0, 100000));
    streamBudgetInit(&b, 1000, 500, 0);
    check("budget: starts with a full burst", streamBudgetTake(&b, 0, 500) && !streamBudgetTake(&b, 0, 1));
    check("budget: refusal counted", b.dropped == 1);
    check("budget: refills at the rate", streamBudgetTake(&b, 100, 100) && !streamBudgetTake(&b, 100, 1));
    check("budget: refill capped at the burst", streamBudgetTake(&b, 60000, 500) && !streamBudgetTake(&b, 60000, 1));
    check("budget: BLE rate MTU 247 / 30 ms -> clamped to the max", streamBudgetBleRate(247, 24) == STREAM_BUDGET_BLE_MAX);
    check("budget: BLE rate MTU 23 / 50 ms -> the floor", streamBudgetBleRate(23, 40) == STREAM_BUDGET_BLE_MIN);
    check("budget: BLE rate in between", streamBudgetBleRate(103, 40) == 2000U);
}

int main(void)
{
    testParse();
    testEncode();
    testCredits();
    testRing();
    testBudget();
    return finish();
}
