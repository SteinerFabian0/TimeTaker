/*
 * Host test for link_msg.c, through the control tree's frame_codec.c (the
 * real codec, compiled from its tree): BLE_FIRMWARE_GUIDE.md §5.
 *
 *   - DATA frames of every length 1..244 through the codec and back, the
 *     244 B frame at the wire bound, the decoder buffer size
 *   - CTRL messages: hand-written wire bytes of every nRF -> STM message
 *     (little-endian, packed), every STM -> nRF message at its exact size,
 *     one byte short (dropped), longer (accepted, extra ignored)
 *   - unknown and wrong-direction types ignored, out-of-range values ignored
 *   - DATA length and channel rejections
 *   - corrupt / truncated frames: dropped, the next frame decodes
 *   - KICK reason mapping, RESETREAS mapping, saturation
 *
 * Every rejection case breaks exactly one rule.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../check.h"
#include "frame_codec.h"
#include "link_msg.h"

static uint8_t        decBuf[LINK_RX_BUF_BYTES];
static frameDecoder_t dec;

/* Feeds a wire frame; returns the parse result of the frame it completes,
 * or 99 if no frame completed with a good CRC. */
static int decodeWire(const uint8_t *wire, uint16_t len, linkMsg_t *m)
{
    int result = 99;
    for (uint16_t i = 0; i < len; i++) {
        if (frameDecoderPush(&dec, wire[i]) == FRAME_READY) {
            result = linkParse(dec.frame, dec.frameLen, m);
        }
    }
    return result;
}

/* Builds a raw link frame channel|body and returns its wire bytes. */
static uint16_t rawWire(const uint8_t *raw, uint16_t rawLen, uint8_t *wire, uint16_t cap)
{
    return frameEncode(raw, rawLen, NULL, 0, wire, cap);
}

/* Raw frame (after decoding) of an encoded CTRL message. */
static bool ctrlRaw(uint8_t type, const void *msg, uint16_t len, const uint8_t *want, uint16_t wantLen)
{
    uint8_t wire[LINK_WIRE_MAX];
    const uint16_t n = linkEncodeCtrl(type, msg, len, wire, sizeof(wire));
    if (n == 0) {
        return false;
    }
    frameDecoderReset(&dec);
    bool ready = false;
    for (uint16_t i = 0; i < n; i++) {
        ready = (frameDecoderPush(&dec, wire[i]) == FRAME_READY);
    }
    return ready && dec.frameLen == wantLen && memcmp(dec.frame, want, wantLen) == 0;
}

static void dataFrames(void)
{
    uint8_t   payload[BLE_LINK_DATA_MAX];
    uint8_t   wire[LINK_WIRE_MAX];
    linkMsg_t m;
    bool      allOk = true;
    uint16_t  maxWire = 0;

    for (uint16_t len = 1; len <= BLE_LINK_DATA_MAX; len++) {
        for (uint16_t i = 0; i < len; i++) {
            payload[i] = (uint8_t)((i * 7U + len) & 0xFFU);   /* includes zeros */
        }
        const uint16_t n = linkEncodeData(payload, len, wire, sizeof(wire));
        if (n > maxWire) {
            maxWire = n;
        }
        const int r = decodeWire(wire, n, &m);
        if (n == 0 || r != LINK_MSG_DATA || m.len != len || memcmp(m.data, payload, len) != 0) {
            allOk = false;
        }
    }
    check("DATA 1..244 B: through frame_codec and back", allOk);
    check("DATA 244 B: within LINK_WIRE_MAX", maxWire <= LINK_WIRE_MAX);
    check("LINK_WIRE_MAX: one COBS block (<= 254 + code + delimiter)", LINK_WIRE_MAX <= 256U);

    /* 244 B of zeros: worst case for COBS, still one frame. */
    memset(payload, 0, sizeof(payload));
    const uint16_t n = linkEncodeData(payload, BLE_LINK_DATA_MAX, wire, sizeof(wire));
    check("DATA 244 zeros: decodes", n != 0 && decodeWire(wire, n, &m) == LINK_MSG_DATA
                                     && m.len == BLE_LINK_DATA_MAX);
    memset(payload, 0xFF, sizeof(payload));
    const uint16_t n2 = linkEncodeData(payload, BLE_LINK_DATA_MAX, wire, sizeof(wire));
    check("DATA 244 x 0xFF: decodes", n2 != 0 && decodeWire(wire, n2, &m) == LINK_MSG_DATA);

    /* Encoder limits. */
    check("encode: DATA 0 B refused", linkEncodeData(payload, 0, wire, sizeof(wire)) == 0);
    check("encode: DATA 245 B refused", linkEncodeData(payload, BLE_LINK_DATA_MAX + 1U, wire, sizeof(wire)) == 0);
    check("encode: DATA into a too small buffer refused", linkEncodeData(payload, 10, wire, 12) == 0);
    {
        uint8_t big[LINK_CTRL_MSG_MAX + 1];
        memset(big, 1, sizeof(big));
        check("encode: CTRL longer than the largest message refused",
              linkEncodeCtrl(BLE_CTRL_STATS, big, sizeof(big), wire, sizeof(wire)) == 0);
    }
}

static void goldenNrfToStm(void)
{
    /* NRF_HELLO, all fields distinct, little-endian. */
    const bleCtrlNrfHello_t h = {
        .linkMajor = 0, .linkMinor = 1, .fwMajor = 2, .fwMinor = 3, .fwPatch = 4,
        .credits = 4, .resetCause = BLE_RESET_WATCHDOG, .reserved = 0,
        .fwGitRev = 0x07212ce4UL, .btAddr = { 0x11, 0x22, 0x33, 0x44, 0x55, 0xC6 },
    };
    const uint8_t helloRaw[] = { 0x01, 0x01, 0x00, 0x01, 0x02, 0x03, 0x04, 0x04, 0x04, 0x00,
                                 0xE4, 0x2C, 0x21, 0x07, 0x11, 0x22, 0x33, 0x44, 0x55, 0xC6 };
    check("NRF_HELLO wire bytes", ctrlRaw(BLE_CTRL_NRF_HELLO, &h, sizeof(h), helloRaw, sizeof(helloRaw)));

    const bleCtrlLinkUp_t up = { .attMtu = 247, .txPhy = 2, .rxPhy = 1, .intervalUnits = 24,
                                 .latency = 0, .timeout10ms = 400, .maxTxOctets = 251 };
    const uint8_t upRaw[] = { 0x01, 0x02, 0xF7, 0x00, 0x02, 0x01, 0x18, 0x00, 0x00, 0x00,
                              0x90, 0x01, 0xFB, 0x00 };
    check("LINK_UP wire bytes", ctrlRaw(BLE_CTRL_LINK_UP, &up, sizeof(up), upRaw, sizeof(upRaw)));

    const bleCtrlLinkDown_t down = { .hciReason = 0x13 };
    const uint8_t downRaw[] = { 0x01, 0x03, 0x13 };
    check("LINK_DOWN wire bytes", ctrlRaw(BLE_CTRL_LINK_DOWN, &down, sizeof(down), downRaw, sizeof(downRaw)));

    const bleCtrlButton_t btn = { .press = BLE_PRESS_VERY_LONG };
    const uint8_t btnRaw[] = { 0x01, 0x04, 0x03 };
    check("BUTTON wire bytes", ctrlRaw(BLE_CTRL_BUTTON, &btn, sizeof(btn), btnRaw, sizeof(btnRaw)));

    const bleCtrlCredit_t cr = { .count = 2 };
    const uint8_t crRaw[] = { 0x01, 0x05, 0x02 };
    check("CREDIT wire bytes", ctrlRaw(BLE_CTRL_CREDIT, &cr, sizeof(cr), crRaw, sizeof(crRaw)));

    const bleCtrlStats_t st = { .rssi = -60, .reserved = 0, .dataDropped = 0x0102,
                                .linkCrcErrors = 0x0304, .uartErrors = 0xFFFF };
    const uint8_t stRaw[] = { 0x01, 0x06, 0xC4, 0x00, 0x02, 0x01, 0x04, 0x03, 0xFF, 0xFF };
    check("STATS wire bytes", ctrlRaw(BLE_CTRL_STATS, &st, sizeof(st), stRaw, sizeof(stRaw)));
}

/* Parses channel CTRL + body. */
static int parseCtrl(const uint8_t *body, uint16_t len, linkMsg_t *m)
{
    uint8_t raw[64];
    raw[0] = BLE_LINK_CH_CTRL;
    memcpy(&raw[1], body, len);
    return linkParse(raw, (uint16_t)(len + 1U), m);
}

static void stmToNrf(void)
{
    linkMsg_t m;
    char      label[96];

    /* Exact size, through the codec. */
    {
        const bleCtrlUiState_t ui = { .session = BLE_UI_SESSION_BOUND, .pairWindow = 1 };
        uint8_t wire[LINK_WIRE_MAX];
        const uint16_t n = linkEncodeCtrl(BLE_CTRL_UI_STATE, &ui, sizeof(ui), wire, sizeof(wire));
        frameDecoderReset(&dec);
        check("UI_STATE through the codec",
              decodeWire(wire, n, &m) == LINK_MSG_CTRL && m.ctrl.type == BLE_CTRL_UI_STATE
              && m.ctrl.u.uiState.session == BLE_UI_SESSION_BOUND && m.ctrl.u.uiState.pairWindow == 1);
    }

    const struct {
        uint8_t type;
        uint8_t body[3];
        uint8_t size;
    } msgs[] = {
        { BLE_CTRL_STM_HELLO, { 0, 1 }, 2 },
        { BLE_CTRL_UI_STATE,  { 1, 0 }, 2 },
        { BLE_CTRL_CUE,       { 2 },    1 },
        { BLE_CTRL_KICK,      { 0x13 }, 1 },
        { BLE_CTRL_ADVERTISE, { 0 },    1 },
    };
    for (unsigned i = 0; i < sizeof(msgs) / sizeof(msgs[0]); i++) {
        uint8_t body[8] = { msgs[i].type };
        memcpy(&body[1], msgs[i].body, msgs[i].size);

        check("size table matches the struct", linkCtrlSize(msgs[i].type) == msgs[i].size);

        snprintf(label, sizeof(label), "type 0x%02X: exact size accepted", msgs[i].type);
        check(label, parseCtrl(body, (uint16_t)(1 + msgs[i].size), &m) == LINK_MSG_CTRL
                     && m.ctrl.type == msgs[i].type
                     && memcmp(&m.ctrl.u, msgs[i].body, msgs[i].size) == 0);

        snprintf(label, sizeof(label), "type 0x%02X: one byte short dropped", msgs[i].type);
        check(label, parseCtrl(body, msgs[i].size, &m) == LINK_MSG_ERR_CTRL_SHORT);

        body[1 + msgs[i].size]     = 0xEE;     /* a newer MINOR's extra field */
        body[1 + msgs[i].size + 1] = 0xEE;
        snprintf(label, sizeof(label), "type 0x%02X: longer accepted, extra ignored", msgs[i].type);
        check(label, parseCtrl(body, (uint16_t)(3 + msgs[i].size), &m) == LINK_MSG_CTRL
                     && memcmp(&m.ctrl.u, msgs[i].body, msgs[i].size) == 0);
    }

    /* Sizes of the nRF -> STM types too. */
    check("size table: NRF_HELLO 18", linkCtrlSize(BLE_CTRL_NRF_HELLO) == 18);
    check("size table: LINK_UP 12", linkCtrlSize(BLE_CTRL_LINK_UP) == 12);
    check("size table: STATS 8", linkCtrlSize(BLE_CTRL_STATS) == 8);
    check("size table: unknown 0", linkCtrlSize(0x46) == 0);

    /* An empty CTRL body has no type: short. */
    check("CTRL with no type byte: dropped as short", parseCtrl(NULL, 0, &m) == LINK_MSG_ERR_CTRL_SHORT);

    /* Unknown types, and nRF -> STM types arriving at the nRF. */
    const uint8_t unknown[] = { 0x00, 0x07, 0x3F, 0x40, 0x46, 0x7F, 0x80, 0xFF };
    for (unsigned i = 0; i < sizeof(unknown); i++) {
        const uint8_t body[4] = { unknown[i], 1, 1, 1 };
        snprintf(label, sizeof(label), "unknown type 0x%02X ignored", unknown[i]);
        check(label, parseCtrl(body, sizeof(body), &m) == LINK_MSG_CTRL_UNKNOWN);
    }
    for (uint8_t t = BLE_CTRL_NRF_HELLO; t <= BLE_CTRL_STATS; t++) {
        uint8_t body[20] = { t };
        snprintf(label, sizeof(label), "wrong direction 0x%02X ignored", t);
        check(label, parseCtrl(body, sizeof(body), &m) == LINK_MSG_CTRL_UNKNOWN);
    }

    /* Values: the edge of the valid range, and one past it. */
    {
        const uint8_t ok1[]  = { BLE_CTRL_UI_STATE, 2, 1 };
        const uint8_t bad1[] = { BLE_CTRL_UI_STATE, 3, 1 };
        const uint8_t bad2[] = { BLE_CTRL_UI_STATE, 2, 2 };
        check("UI_STATE session BOUND, window 1: accepted", parseCtrl(ok1, 3, &m) == LINK_MSG_CTRL);
        check("UI_STATE session 3: ignored", parseCtrl(bad1, 3, &m) == LINK_MSG_CTRL_BAD_VALUE);
        check("UI_STATE pairWindow 2: ignored", parseCtrl(bad2, 3, &m) == LINK_MSG_CTRL_BAD_VALUE);

        const uint8_t cue1[] = { BLE_CTRL_CUE, 1 }, cue2[] = { BLE_CTRL_CUE, 2 };
        const uint8_t cue0[] = { BLE_CTRL_CUE, 0 }, cue3[] = { BLE_CTRL_CUE, 3 };
        check("CUE 1 and 2 accepted", parseCtrl(cue1, 2, &m) == LINK_MSG_CTRL
                                      && parseCtrl(cue2, 2, &m) == LINK_MSG_CTRL);
        check("CUE 0: ignored", parseCtrl(cue0, 2, &m) == LINK_MSG_CTRL_BAD_VALUE);
        check("CUE 3: ignored", parseCtrl(cue3, 2, &m) == LINK_MSG_CTRL_BAD_VALUE);

        const uint8_t adv0[] = { BLE_CTRL_ADVERTISE, 0 }, adv1[] = { BLE_CTRL_ADVERTISE, 1 };
        const uint8_t adv2[] = { BLE_CTRL_ADVERTISE, 2 };
        check("ADVERTISE 0 and 1 accepted", parseCtrl(adv0, 2, &m) == LINK_MSG_CTRL
                                            && parseCtrl(adv1, 2, &m) == LINK_MSG_CTRL);
        check("ADVERTISE 2: ignored", parseCtrl(adv2, 2, &m) == LINK_MSG_CTRL_BAD_VALUE);

        const uint8_t hello[] = { BLE_CTRL_STM_HELLO, 7, 9 };
        check("STM_HELLO any version accepted (the nRF only reports it)",
              parseCtrl(hello, 3, &m) == LINK_MSG_CTRL && m.ctrl.u.stmHello.linkMajor == 7);
    }
}

static void linkRejections(void)
{
    linkMsg_t m;
    uint8_t   raw[BLE_LINK_RAW_MAX + 4];
    uint8_t   wire[LINK_WIRE_MAX + 8];

    memset(raw, 0x5A, sizeof(raw));

    raw[0] = BLE_LINK_CH_DATA;
    check("DATA with no body: rejected", linkParse(raw, 1, &m) == LINK_MSG_ERR_DATA_LEN);
    check("DATA 1 B: accepted", linkParse(raw, 2, &m) == LINK_MSG_DATA && m.len == 1);
    check("DATA 244 B: accepted", linkParse(raw, 1 + BLE_LINK_DATA_MAX, &m) == LINK_MSG_DATA);

    /* 245 B still fits the decoder buffer (raw max + CRC), so the length
     * rule, not the decoder, must reject it. */
    const uint16_t n = rawWire(raw, 1 + BLE_LINK_DATA_MAX + 1, wire, sizeof(wire));
    frameDecoderReset(&dec);
    check("DATA 245 B: decoder passes it, link rejects it",
          n != 0 && decodeWire(wire, n, &m) == LINK_MSG_ERR_DATA_LEN);

    raw[0] = 2;
    check("channel 2: rejected", linkParse(raw, 5, &m) == LINK_MSG_ERR_CHANNEL);
    raw[0] = 0xFF;
    check("channel 0xFF: rejected", linkParse(raw, 5, &m) == LINK_MSG_ERR_CHANNEL);
    check("empty raw frame: rejected", linkParse(raw, 0, &m) == LINK_MSG_ERR_CHANNEL);
}

static void resync(void)
{
    linkMsg_t m;
    uint8_t   a[LINK_WIRE_MAX], b[LINK_WIRE_MAX];
    const uint8_t pa[] = { 1, 2, 0, 3 }, pb[] = { 9, 8, 7 };
    const uint16_t na = linkEncodeData(pa, sizeof(pa), a, sizeof(a));
    const uint16_t nb = linkEncodeData(pb, sizeof(pb), b, sizeof(b));

    frameDecoderReset(&dec);
    a[2] ^= 0x40;                                          /* bit error */
    check("corrupt frame: dropped", decodeWire(a, na, &m) == 99);
    check("corrupt frame: next frame decodes",
          decodeWire(b, nb, &m) == LINK_MSG_DATA && m.len == 3 && m.data[0] == 9);
    a[2] ^= 0x40;

    check("truncated frame: dropped", decodeWire(&a[2], (uint16_t)(na - 2), &m) == 99);
    check("truncated frame: next frame decodes", decodeWire(b, nb, &m) == LINK_MSG_DATA);

    /* Garbage without delimiters longer than the buffer, then a frame. */
    uint8_t junk[400];
    memset(junk, 0x33, sizeof(junk));
    check("overflow garbage: dropped", decodeWire(junk, sizeof(junk), &m) == 99);
    const uint8_t delim = 0;
    decodeWire(&delim, 1, &m);
    check("overflow garbage: next frame decodes", decodeWire(b, nb, &m) == LINK_MSG_DATA);

    /* Back-to-back CTRL and DATA: each its own frame. */
    const bleCtrlCredit_t c = { .count = 3 };
    uint8_t both[2 * LINK_WIRE_MAX];
    uint16_t nc = linkEncodeCtrl(BLE_CTRL_CREDIT, &c, sizeof(c), both, sizeof(both));
    memcpy(&both[nc], b, nb);
    int first = 99, second = 99;
    for (uint16_t i = 0; i < nc + nb; i++) {
        if (frameDecoderPush(&dec, both[i]) == FRAME_READY) {
            const int r = linkParse(dec.frame, dec.frameLen, &m);
            if (first == 99) {
                first = r;
            } else {
                second = r;
            }
        }
    }
    check("CTRL then DATA back to back: two messages",
          first == LINK_MSG_CTRL_UNKNOWN /* CREDIT is nRF->STM */ && second == LINK_MSG_DATA);
}

static void helpers(void)
{
    const uint8_t valid[] = { 0x05, 0x13, 0x14, 0x15, 0x1A, 0x29, 0x3B };
    bool keep = true;
    for (unsigned i = 0; i < sizeof(valid); i++) {
        keep = keep && (linkKickReason(valid[i]) == valid[i]);
    }
    check("KICK: valid HCI disconnect reasons kept", keep);
    check("KICK: 0 -> 0x13", linkKickReason(0) == 0x13);
    check("KICK: 0x16 (local host) -> 0x13", linkKickReason(0x16) == 0x13);
    check("KICK: 0xFF -> 0x13", linkKickReason(0xFF) == 0x13);

    check("reset: no bit -> POWER_ON", linkResetCause(0) == BLE_RESET_POWER_ON);
    check("reset: RESETPIN -> PIN", linkResetCause(NRF_RESETREAS_RESETPIN) == BLE_RESET_PIN);
    check("reset: DOG -> WATCHDOG", linkResetCause(NRF_RESETREAS_DOG) == BLE_RESET_WATCHDOG);
    check("reset: SREQ -> SOFTWARE", linkResetCause(NRF_RESETREAS_SREQ) == BLE_RESET_SOFTWARE);
    check("reset: LOCKUP -> LOCKUP", linkResetCause(NRF_RESETREAS_LOCKUP) == BLE_RESET_LOCKUP);
    check("reset: OFF -> UNKNOWN", linkResetCause(NRF_RESETREAS_OFF) == BLE_RESET_UNKNOWN);
    check("reset: DIF -> UNKNOWN", linkResetCause(NRF_RESETREAS_DIF) == BLE_RESET_UNKNOWN);
    check("reset: DOG beats PIN", linkResetCause(NRF_RESETREAS_DOG | NRF_RESETREAS_RESETPIN) == BLE_RESET_WATCHDOG);
    check("reset: LOCKUP beats SREQ", linkResetCause(NRF_RESETREAS_LOCKUP | NRF_RESETREAS_SREQ) == BLE_RESET_LOCKUP);
    check("reset: SREQ beats PIN", linkResetCause(NRF_RESETREAS_SREQ | NRF_RESETREAS_RESETPIN) == BLE_RESET_SOFTWARE);

    check("sat16: 0", linkSat16(0) == 0);
    check("sat16: 65535", linkSat16(65535) == 65535);
    check("sat16: 65536 saturates", linkSat16(65536) == 65535);
}

int main(void)
{
    frameDecoderInit(&dec, decBuf, sizeof(decBuf));
    dataFrames();
    goldenNrfToStm();
    stmToNrf();
    linkRejections();
    resync();
    helpers();
    return checkSummary("host_link_msg");
}
