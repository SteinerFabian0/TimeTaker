/*
 * Host test for host_session.c: the session layer against a fake transport.
 *
 * The fake is a byte queue in each direction with an adjustable amount of
 * free TX space. Every request goes in as a real frame (frame_codec) and
 * every answer is decoded back from the bytes the session wrote. So this
 * covers the session exactly as the web app sees it: HELLO gating, PING,
 * RESUME and its refusals, the event pump with backpressure and the response
 * reserve, the owner hand-off, and a reset.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "host_session.h"

static int failures;

static void check(const char *name, int passed)
{
    printf("%-64s %s\n", name, passed ? "ok" : "FAIL");
    failures += passed ? 0 : 1;
}

/* --- Fake transport ---------------------------------------------------------- */

static uint8_t  rxQ[4096];
static uint16_t rxHead, rxTail;
static uint8_t  txQ[16384];
static uint16_t txLen;
static uint32_t txSpace = 100000U;
static uint32_t flushes;

static uint16_t fakeRead(uint8_t *buf, uint16_t max)
{
    uint16_t n = 0U;
    while (n < max && rxHead != rxTail) {
        buf[n++] = rxQ[rxHead++];
    }
    return n;
}

static uint16_t fakeSpace(void)
{
    uint32_t s = txSpace;
    return (s > 0xFFFFU) ? 0xFFFFU : (uint16_t)s;
}

static uint16_t fakeWrite(const uint8_t *buf, uint16_t len)
{
    if (len > txSpace || txLen + len > sizeof(txQ)) {
        return 0U;
    }
    memcpy(&txQ[txLen], buf, len);
    txLen += len;
    if (txSpace < 100000U) {
        txSpace -= len;
    }
    return len;
}

static void fakeFlush(void)
{
    flushes++;
}

static const hostTransport_t fakeIo = { fakeRead, fakeSpace, fakeWrite, fakeFlush };

static uint64_t fakeNowUs = 123456789ULL;
static uint64_t fakeClock(void)
{
    return fakeNowUs;
}

/* --- Owner hand-off ----------------------------------------------------------- */

static uint8_t      ownerLastMsg;
static uint16_t     ownerLastLen;
static ctrlStatus_e ownerReturn = CTRL_OK;
static int          ownerShortBody;
static int          ownerDefers;
static int          ownerCalls;

static ctrlStatus_e fakeOwner(hostSession_t *s, uint8_t msgId, const uint8_t *req, uint16_t reqLen,
                              uint8_t *rsp, uint16_t rspCap, uint16_t *rspLen)
{
    (void)req; (void)rspCap;
    ownerLastMsg = msgId;
    ownerLastLen = reqLen;
    ownerCalls++;
    if (ownerDefers) {
        hostSessionDefer(s);
        return CTRL_OK;
    }
    if (ownerShortBody) {
        *rspLen = 1U;
        return CTRL_OK;
    }
    hpLanePowerRsp_t *r = (hpLanePowerRsp_t *)rsp;
    r->status  = 0x5555;   /* garbage the session must overwrite with OK */
    r->powered = 1U;
    *rspLen = HP_LANE_POWER_RSP_BYTES;
    return ownerReturn;
}

/* --- Helpers ------------------------------------------------------------------ */

#define BOOT_ID  0xB007B007U
#define CAP      8U

static eventLogSlot_t slots[CAP];
static eventLog_t     elog;
static hostSession_t  sess;

/* Bind policy hook: what the owner's onHello() decides. */
static uint8_t helloDecides = (uint8_t)HP_BIND_TRUSTED;
static uint8_t helloSawKind;
static uint8_t fakeOnHello(hostSession_t *s, const hpHelloReq_t *hello)
{
    (void)s;
    helloSawKind = hello->clientKind;
    return helloDecides;
}

typedef struct {
    hpHeader_t hdr;
    uint8_t    payload[HP_MAX_PAYLOAD];
    uint16_t   len;
} frame_t;

static frame_t  outFrames[64];
static int      outCount;

static uint8_t setupBoardType = (uint8_t)HP_BOARD_CONTROL;

static void setupWith(uint8_t transport, uint8_t bindState, bool withHello)
{
    rxHead = rxTail = 0U;
    txLen = 0U;
    txSpace = 100000U;
    ownerDefers = 0;
    ownerCalls = 0;
    ownerLastMsg = 0U;
    eventLogInit(&elog, slots, CAP);
    const hostSessionConfig_t cfg = {
        .transport = transport,
        .bindState = bindState,
        .boardType = setupBoardType,
        .bootId    = BOOT_ID,
        .log       = &elog,
        .nowUs     = fakeClock,
        .otherSessions = NULL,
        .onRequest = fakeOwner,
        .onHello   = withHello ? fakeOnHello : NULL,
    };
    hostSessionInit(&sess, &fakeIo, &cfg);
}

static void setup(void)
{
    setupWith((uint8_t)HP_TRANSPORT_USB, (uint8_t)HP_BIND_TRUSTED, false);
}

static void sendRaw(const uint8_t *wire, uint16_t n)
{
    memcpy(&rxQ[rxTail], wire, n);
    rxTail += n;
}

static void sendRequest(uint8_t msgId, uint8_t flags, uint16_t tag, const void *payload, uint16_t len)
{
    hpHeader_t hdr = { .msgId = msgId, .flags = flags, .tag = tag };
    uint8_t wire[FRAME_CODEC_WIRE_BOUND(HP_MAX_FRAME_RAW)];
    uint16_t n = frameEncode((const uint8_t *)&hdr, HP_HEADER_BYTES, (const uint8_t *)payload, len,
                             wire, sizeof(wire));
    sendRaw(wire, n);
}

/* Runs the session, decodes everything it wrote into outFrames. */
static void run(void)
{
    static uint8_t rx[FRAME_CODEC_RX_BUF_BYTES(HP_MAX_FRAME_RAW)];
    frameDecoder_t dec;

    txLen = 0U;
    hostSessionRun(&sess);

    frameDecoderInit(&dec, rx, sizeof(rx));
    outCount = 0;
    for (uint16_t i = 0U; i < txLen; i++) {
        if (frameDecoderPush(&dec, txQ[i]) == FRAME_READY && outCount < 64) {
            frame_t *f = &outFrames[outCount++];
            memcpy(&f->hdr, dec.frame, HP_HEADER_BYTES);
            f->len = (uint16_t)(dec.frameLen - HP_HEADER_BYTES);
            memcpy(f->payload, dec.frame + HP_HEADER_BYTES, f->len);
        }
    }
}

static int16_t statusOf(const frame_t *f)
{
    int16_t st;
    memcpy(&st, f->payload, sizeof(st));
    return st;
}

static bool isStatusOnly(const frame_t *f, uint8_t msgId, uint16_t tag, ctrlStatus_e st)
{
    return f->hdr.msgId == msgId && f->hdr.flags == HP_FLAG_RESPONSE && f->hdr.tag == tag
        && f->len == 2U && statusOf(f) == (int16_t)st;
}

static hpHelloReq_t helloReq(uint8_t major)
{
    hpHelloReq_t h;
    memset(&h, 0, sizeof(h));
    h.protoMajor = major;
    h.protoMinor = HP_PROTO_MINOR;
    h.clientKind = (uint8_t)HP_CLIENT_WEBAPP;
    memcpy(h.clientName, "test", 4);
    return h;
}

static void openSession(void)
{
    hpHelloReq_t h = helloReq(HP_PROTO_MAJOR);
    sendRequest(HP_MSG_HELLO, 0U, 1U, &h, sizeof(h));
    run();
}

static void publish(uint8_t id)
{
    hpEvtButton_t e = { .button = id, .press = 1U, .sysUs = id };
    (void)eventLogAppend(&elog, HP_MSG_EVT_BUTTON, &e, sizeof(e), NULL);
}

/* evSeq of an event frame, or -1. */
static int64_t evSeqOf(const frame_t *f)
{
    if (f->hdr.flags != HP_FLAG_EVENT || f->hdr.tag != 0U || f->len < 4U) {
        return -1;
    }
    uint32_t seq;
    memcpy(&seq, f->payload, 4);
    return seq;
}

/* --- Cases -------------------------------------------------------------------- */

static void testGate(void)
{
    setup();
    hpPingReq_t ping = { .hostMs = 7U };
    sendRequest(HP_MSG_PING, 0U, 0x2222U, &ping, sizeof(ping));
    run();
    check("gate: PING before HELLO -> ERR_NO_SESSION, tag echoed",
          outCount == 1 && isStatusOnly(&outFrames[0], HP_MSG_PING, 0x2222U, CTRL_ERR_NO_SESSION));

    sendRequest(HP_MSG_LANE_POWER, 0U, 3U, "\x01", 1U);
    run();
    check("gate: owner request before HELLO never reaches the owner",
          outCount == 1 && isStatusOnly(&outFrames[0], HP_MSG_LANE_POWER, 3U, CTRL_ERR_NO_SESSION)
          && ownerLastMsg == 0U);
}

static void testHello(void)
{
    setup();
    hpHelloReq_t h = helloReq(HP_PROTO_MAJOR);
    sendRequest(HP_MSG_HELLO, 0U, 9U, &h, (uint16_t)(sizeof(h) - 1U));
    run();
    check("hello: one byte short -> ERR_BAD_LENGTH",
          outCount == 1 && isStatusOnly(&outFrames[0], HP_MSG_HELLO, 9U, CTRL_ERR_BAD_LENGTH));

    h = helloReq((uint8_t)(HP_PROTO_MAJOR + 1U));
    sendRequest(HP_MSG_HELLO, 0U, 10U, &h, sizeof(h));
    run();
    check("hello: other protocol MAJOR -> ERR_PROTO_MISMATCH, stays closed",
          outCount == 1 && isStatusOnly(&outFrames[0], HP_MSG_HELLO, 10U, CTRL_ERR_PROTO_MISMATCH)
          && !hostSessionIsOpen(&sess));

    publish(1);
    publish(2);
    openSession();
    hpHelloRsp_t r;
    memcpy(&r, outFrames[0].payload, sizeof(r));
    check("hello: OK opens the session",
          outCount == 1 && outFrames[0].hdr.flags == HP_FLAG_RESPONSE && outFrames[0].len == HP_HELLO_RSP_BYTES
          && r.status == 0 && hostSessionIsOpen(&sess));
    check("hello: reports bootId, transport, bind state, clock, log window",
          r.bootId == BOOT_ID && r.transport == HP_TRANSPORT_USB && r.bindState == HP_BIND_TRUSTED
          && r.sysUs == fakeNowUs && r.logOldestSeq == 0U && r.logNextSeq == 2U
          && r.protoMajor == HP_PROTO_MAJOR && r.protoMinor == HP_PROTO_MINOR);
    check("hello: reports the configured board type (CONTROL), reserved zero",
          r.boardType == HP_BOARD_CONTROL && r.reserved[0] == 0U && r.reserved[1] == 0U);
    check("hello: history is not pushed (cursor starts at the head)", outCount == 1);

    setupBoardType = (uint8_t)HP_BOARD_CONTROL_BOOT;   /* the bootloader's session config */
    setup();
    openSession();
    memcpy(&r, outFrames[0].payload, sizeof(r));
    setupBoardType = (uint8_t)HP_BOARD_CONTROL;
    check("hello: the bootloader's session reports CONTROL_BOOT",
          outCount == 1 && r.status == 0 && r.boardType == HP_BOARD_CONTROL_BOOT);
}

static void testPing(void)
{
    setup();
    openSession();
    fakeNowUs = 987654321ULL;
    hpPingReq_t ping = { .hostMs = 0xCAFEU };
    sendRequest(HP_MSG_PING, 0U, 0xBEEFU, &ping, sizeof(ping));
    run();
    hpPingRsp_t r;
    memcpy(&r, outFrames[0].payload, sizeof(r));
    check("ping: echoes hostMs and adds sysUs",
          outCount == 1 && outFrames[0].hdr.tag == 0xBEEFU && r.status == 0 && r.hostMs == 0xCAFEU
          && r.sysUs == 987654321ULL);
}

static void testEventsAndResume(void)
{
    setup();
    publish(0);                      /* evSeq 0, before the session */
    openSession();
    publish(1);
    publish(2);
    publish(3);
    run();
    check("events: new events pumped in order with evSeq, EVENT flag, tag 0",
          outCount == 3 && evSeqOf(&outFrames[0]) == 1 && evSeqOf(&outFrames[1]) == 2
          && evSeqOf(&outFrames[2]) == 3 && outFrames[0].hdr.msgId == HP_MSG_EVT_BUTTON
          && outFrames[0].len == HP_EVT_BUTTON_BYTES);
    check("events: each pump that wrote something flushed", flushes > 0U);

    hpResumeReq_t res = { .bootId = BOOT_ID, .fromSeq = 0U };
    sendRequest(HP_MSG_RESUME, 0U, 5U, &res, sizeof(res));
    run();
    hpResumeRsp_t rr;
    memcpy(&rr, outFrames[0].payload, sizeof(rr));
    check("resume: OK reports the window, then replays from fromSeq",
          outCount == 5 && rr.status == 0 && rr.logOldestSeq == 0U && rr.logNextSeq == 4U
          && evSeqOf(&outFrames[1]) == 0 && evSeqOf(&outFrames[4]) == 3);

    res.bootId = BOOT_ID ^ 1U;
    sendRequest(HP_MSG_RESUME, 0U, 6U, &res, sizeof(res));
    run();
    check("resume: another bootId -> ERR_BOOT_MISMATCH",
          outCount == 1 && isStatusOnly(&outFrames[0], HP_MSG_RESUME, 6U, CTRL_ERR_BOOT_MISMATCH));

    res.bootId  = BOOT_ID;
    res.fromSeq = 5U;
    sendRequest(HP_MSG_RESUME, 0U, 7U, &res, sizeof(res));
    run();
    check("resume: fromSeq beyond the head -> ERR_INVALID_ARG",
          outCount == 1 && isStatusOnly(&outFrames[0], HP_MSG_RESUME, 7U, CTRL_ERR_INVALID_ARG));

    for (uint8_t i = 4U; i < 12U; i++) {
        publish(i);                  /* 12 events in a log of 8: 0..3 overwritten */
    }
    run();                           /* pump the new ones first */
    res.fromSeq = 3U;
    sendRequest(HP_MSG_RESUME, 0U, 8U, &res, sizeof(res));
    run();
    check("resume: fromSeq already overwritten -> ERR_LOG_GAP",
          outCount == 1 && isStatusOnly(&outFrames[0], HP_MSG_RESUME, 8U, CTRL_ERR_LOG_GAP));
}

static void testBackpressure(void)
{
    setup();
    openSession();
    for (uint8_t i = 0U; i < 6U; i++) {
        publish(i);
    }
    /* Room for the reserve plus two event frames only. */
    uint16_t evWire = FRAME_CODEC_WIRE_BOUND(HP_HEADER_BYTES + HP_EVT_BUTTON_BYTES);
    txSpace = HOST_SESSION_RESPONSE_RESERVE + 2U * evWire;
    run();
    int first = outCount;
    int64_t lastSeq = (first > 0) ? evSeqOf(&outFrames[first - 1]) : -1;
    check("backpressure: only what fits beside the reserve goes out", first == 2 && lastSeq == 1);

    /* The reserve is left: a request is still answered. */
    hpPingReq_t ping = { .hostMs = 1U };
    sendRequest(HP_MSG_PING, 0U, 0x77U, &ping, sizeof(ping));
    run();
    check("backpressure: a response still fits in the reserve",
          outCount == 1 && outFrames[0].hdr.flags == HP_FLAG_RESPONSE && outFrames[0].hdr.tag == 0x77U);

    txSpace = 100000U;
    run();
    bool inOrder = outCount == 4;
    for (int i = 0; i < outCount; i++) {
        inOrder = inOrder && evSeqOf(&outFrames[i]) == 2 + i;
    }
    check("backpressure: the rest follows in order, none lost or repeated", inOrder);
}

static void testLaggingCursor(void)
{
    setup();
    openSession();
    txSpace = HOST_SESSION_RESPONSE_RESERVE;          /* nothing fits */
    for (uint8_t i = 0U; i < 12U; i++) {
        publish(i);
    }
    run();
    txSpace = 100000U;
    run();
    check("lagging: overwritten events skipped, pump resumes at the oldest held",
          outCount == (int)CAP && evSeqOf(&outFrames[0]) == 4 && evSeqOf(&outFrames[CAP - 1U]) == 11);
}

static void testOwner(void)
{
    setup();
    openSession();
    ownerReturn = CTRL_OK;
    ownerShortBody = 0;
    sendRequest(HP_MSG_LANE_POWER, 0U, 0x4242U, "\x01", 1U);
    run();
    hpLanePowerRsp_t r;
    memcpy(&r, outFrames[0].payload, sizeof(r));
    check("owner: request forwarded with its payload",
          ownerLastMsg == HP_MSG_LANE_POWER && ownerLastLen == 1U);
    check("owner: OK body sent, status forced to OK over owner garbage",
          outCount == 1 && outFrames[0].hdr.tag == 0x4242U && outFrames[0].len == HP_LANE_POWER_RSP_BYTES
          && r.status == 0 && r.powered == 1U);

    ownerReturn = CTRL_ERR_LANE_POWER;
    sendRequest(HP_MSG_LANE_POWER, 0U, 2U, "\x01", 1U);
    run();
    check("owner: error -> status only",
          outCount == 1 && isStatusOnly(&outFrames[0], HP_MSG_LANE_POWER, 2U, CTRL_ERR_LANE_POWER));

    ownerReturn = CTRL_OK;
    ownerShortBody = 1;
    sendRequest(HP_MSG_LANE_POWER, 0U, 3U, "\x01", 1U);
    run();
    check("owner: OK with no room for the status -> ERR_INVALID_STATE",
          outCount == 1 && isStatusOnly(&outFrames[0], HP_MSG_LANE_POWER, 3U, CTRL_ERR_INVALID_STATE));
    ownerShortBody = 0;
}

static void testBadInput(void)
{
    setup();
    openSession();
    uint32_t badBefore = sess.stats.badFrames;

    hpPingReq_t ping = { .hostMs = 1U };
    sendRequest(HP_MSG_PING, HP_FLAG_EVENT, 1U, &ping, sizeof(ping));
    run();
    check("bad: a frame with flags set is ignored, not answered",
          outCount == 0 && sess.stats.badFrames == badBefore + 1U);

    hpHeader_t hdr = { .msgId = HP_MSG_PING, .flags = 0U, .tag = 1U };
    uint8_t wire[64];
    uint16_t n = frameEncode((const uint8_t *)&hdr, HP_HEADER_BYTES, (const uint8_t *)&ping,
                             sizeof(ping), wire, sizeof(wire));
    wire[2] ^= 0x01U;
    sendRaw(wire, n);
    sendRequest(HP_MSG_PING, 0U, 0x99U, &ping, sizeof(ping));
    run();
    check("bad: a corrupt frame is dropped, the next one answered",
          outCount == 1 && outFrames[0].hdr.tag == 0x99U && sess.stats.badFrames == badBefore + 2U);

    sendRequest(0x7FU, 0U, 4U, NULL, 0U);
    ownerLastMsg = 0U;
    run();
    check("bad: an unknown msgId goes to the owner (who decides)",
          ownerLastMsg == 0x7FU && outCount == 1);
}

static void testReset(void)
{
    setup();
    openSession();
    hostSessionReset(&sess);
    hpPingReq_t ping = { .hostMs = 1U };
    sendRequest(HP_MSG_PING, 0U, 1U, &ping, sizeof(ping));
    run();
    check("reset: session closed, next request needs HELLO again",
          !hostSessionIsOpen(&sess) && outCount == 1
          && isStatusOnly(&outFrames[0], HP_MSG_PING, 1U, CTRL_ERR_NO_SESSION));
}

/* --- Bind gating (CONTROL_FIRMWARE_GUIDE §5.4) ------------------------------- */

static void testBindGate(void)
{
    /* A BLE session: configured UNBOUND, the owner's hook decides at HELLO. */
    setupWith((uint8_t)HP_TRANSPORT_BLE, (uint8_t)HP_BIND_UNBOUND, true);
    helloDecides = (uint8_t)HP_BIND_UNBOUND;
    openSession();
    hpHelloRsp_t r;
    memcpy(&r, outFrames[0].payload, sizeof(r));
    check("bind: HELLO is answered OK while UNBOUND, reporting UNBOUND",
          outCount == 1 && r.status == 0 && r.bindState == HP_BIND_UNBOUND && r.transport == HP_TRANSPORT_BLE
          && helloSawKind == HP_CLIENT_WEBAPP && hostSessionGetBindState(&sess) == HP_BIND_UNBOUND);

    hpPingReq_t ping = { .hostMs = 5U };
    sendRequest(HP_MSG_PING, 0U, 0x10U, &ping, sizeof(ping));
    run();
    check("bind: PING is served while UNBOUND",
          outCount == 1 && outFrames[0].hdr.tag == 0x10U && statusOf(&outFrames[0]) == 0
          && outFrames[0].len == HP_PING_RSP_BYTES);

    hpResumeReq_t res = { .bootId = BOOT_ID, .fromSeq = 0U };
    sendRequest(HP_MSG_RESUME, 0U, 0x11U, &res, sizeof(res));
    run();
    check("bind: RESUME while UNBOUND -> ERR_NOT_BOUND",
          outCount == 1 && isStatusOnly(&outFrames[0], HP_MSG_RESUME, 0x11U, CTRL_ERR_NOT_BOUND));

    sendRequest(HP_MSG_LANE_POWER, 0U, 0x12U, "\x01", 1U);
    run();
    check("bind: owner request while UNBOUND -> ERR_NOT_BOUND, owner never called",
          outCount == 1 && isStatusOnly(&outFrames[0], HP_MSG_LANE_POWER, 0x12U, CTRL_ERR_NOT_BOUND)
          && ownerCalls == 0);

    publish(1);
    publish(2);
    run();
    check("bind: no events are pumped while UNBOUND", outCount == 0);

    hpStrRawSamples_t raw = { .lane = 1U };
    check("bind: no streams while UNBOUND",
          !hostSessionSendStream(&sess, HP_MSG_STR_RAW_SAMPLES, &raw, sizeof(raw)));

    hostSessionSetBindState(&sess, (uint8_t)HP_BIND_BOUND);
    run();
    check("bind: bound live -> the events since HELLO follow in order",
          outCount == 2 && evSeqOf(&outFrames[0]) == 0 && evSeqOf(&outFrames[1]) == 1);
    sendRequest(HP_MSG_LANE_POWER, 0U, 0x13U, "\x01", 1U);
    run();
    check("bind: bound live -> owner requests are served",
          outCount == 1 && outFrames[0].hdr.tag == 0x13U && statusOf(&outFrames[0]) == 0 && ownerCalls == 1);

    hostSessionReset(&sess);
    check("bind: reset returns to the configured bind state",
          hostSessionGetBindState(&sess) == HP_BIND_UNBOUND);

    /* Without a hook the configured state stands (USB: TRUSTED). */
    setupWith((uint8_t)HP_TRANSPORT_USB, (uint8_t)HP_BIND_TRUSTED, false);
    hostSessionSetBindState(&sess, (uint8_t)HP_BIND_UNBOUND);
    openSession();
    memcpy(&r, outFrames[0].payload, sizeof(r));
    check("bind: HELLO without a hook re-applies the configured state",
          r.bindState == HP_BIND_TRUSTED && hostSessionGetBindState(&sess) == HP_BIND_TRUSTED);
}

/* --- Deferred responses ---------------------------------------------------------- */

static void testDefer(void)
{
    setup();
    openSession();
    ownerDefers = 1;
    sendRequest(HP_MSG_LANE_STATUS, 0U, 0x3131U, "\x02", 1U);
    run();
    check("defer: no response while the owner works on it",
          outCount == 0 && hostSessionHasDeferred(&sess) && ownerLastMsg == HP_MSG_LANE_STATUS);

    sendRequest(HP_MSG_LANE_STATUS, 0U, 0x3232U, "\x03", 1U);
    run();
    check("defer: a second deferral while one is owed -> ERR_BUSY",
          outCount == 1 && isStatusOnly(&outFrames[0], HP_MSG_LANE_STATUS, 0x3232U, CTRL_ERR_BUSY));

    hpLaneStatusRsp_t body;
    memset(&body, 0x5A, sizeof(body));
    body.lane = 2U;
    txLen = 0U;
    bool done = hostSessionCompleteDeferred(&sess, CTRL_OK, (uint8_t *)&body, sizeof(body));
    frameDecoder_t dec;
    static uint8_t rx[FRAME_CODEC_RX_BUF_BYTES(HP_MAX_FRAME_RAW)];
    frameDecoderInit(&dec, rx, sizeof(rx));
    int got = 0;
    hpHeader_t hdr = { 0 };
    hpLaneStatusRsp_t back;
    memset(&back, 0, sizeof(back));
    for (uint16_t i = 0U; i < txLen; i++) {
        if (frameDecoderPush(&dec, txQ[i]) == FRAME_READY) {
            got++;
            memcpy(&hdr, dec.frame, HP_HEADER_BYTES);
            memcpy(&back, dec.frame + HP_HEADER_BYTES, sizeof(back));
        }
    }
    check("defer: completion answers with the original msgId and tag, status forced OK",
          done && got == 1 && hdr.msgId == HP_MSG_LANE_STATUS && hdr.tag == 0x3131U
          && hdr.flags == HP_FLAG_RESPONSE && back.status == 0 && back.lane == 2U);
    check("defer: nothing owed after completion",
          !hostSessionHasDeferred(&sess)
          && !hostSessionCompleteDeferred(&sess, CTRL_OK, (uint8_t *)&body, sizeof(body)));

    sendRequest(HP_MSG_LANE_STATUS, 0U, 0x3333U, "\x04", 1U);
    run();
    hostSessionReset(&sess);
    check("defer: a reset voids the owed answer",
          !hostSessionHasDeferred(&sess)
          && !hostSessionCompleteDeferred(&sess, CTRL_ERR_LANE_ABSENT, NULL, 0U));
    ownerDefers = 0;
}

/* --- Streams ----------------------------------------------------------------------- */

static void testStreams(void)
{
    setup();
    hpDbgLog_t line = { .level = 3U, .sysUs = 9U };
    check("stream: nothing before HELLO",
          !hostSessionSendStream(&sess, HP_MSG_DBG_LOG, &line, sizeof(line)));

    openSession();
    txLen = 0U;
    bool sent = hostSessionSendStream(&sess, HP_MSG_DBG_LOG, &line, sizeof(line));
    frameDecoder_t dec;
    static uint8_t rx[FRAME_CODEC_RX_BUF_BYTES(HP_MAX_FRAME_RAW)];
    frameDecoderInit(&dec, rx, sizeof(rx));
    hpHeader_t hdr = { 0 };
    int got = 0;
    for (uint16_t i = 0U; i < txLen; i++) {
        if (frameDecoderPush(&dec, txQ[i]) == FRAME_READY) {
            got++;
            memcpy(&hdr, dec.frame, HP_HEADER_BYTES);
        }
    }
    check("stream: sent with the STREAM flag and tag 0 when there is room",
          sent && got == 1 && hdr.flags == HP_FLAG_STREAM && hdr.tag == 0U && hdr.msgId == HP_MSG_DBG_LOG);

    txSpace = HOST_SESSION_RESPONSE_RESERVE + 4U;
    check("stream: dropped when only the response reserve is left",
          !hostSessionSendStream(&sess, HP_MSG_DBG_LOG, &line, sizeof(line))
          && sess.stats.streamsDropped == 1U);
    txSpace = 100000U;
}

int main(void)
{
    testGate();
    testHello();
    testPing();
    testEventsAndResume();
    testBackpressure();
    testLaggingCursor();
    testOwner();
    testBadInput();
    testReset();
    testBindGate();
    testDefer();
    testStreams();

    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
