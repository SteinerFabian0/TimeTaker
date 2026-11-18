/*
 * Host session layer. See host_session.h.
 */

#include "host_session.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "host_protocol.h"
#include "frame_codec.h"
#include "event_log.h"

#define HOST_SESSION_STATUS_BYTES  2U

/* Encodes header | head2 | body into txBuf and writes it if it fits with
 * `reserve` bytes to spare. Returns whether it went out. */
static bool hostSessionSend(hostSession_t *s, const uint8_t *head, uint16_t headLen,
                            const uint8_t *body, uint16_t bodyLen, uint16_t reserve)
{
    uint16_t n = frameEncode(head, headLen, body, bodyLen, s->txBuf, (uint16_t)sizeof(s->txBuf));
    if (n == 0U) {
        return false;
    }
    if ((uint32_t)s->io->writeSpace() < (uint32_t)n + reserve) {
        return false;
    }
    return s->io->write(s->txBuf, n) == n;
}

static void hostSessionRespond(hostSession_t *s, uint8_t msgId, uint16_t tag, ctrlStatus_e status,
                               uint8_t *payload, uint16_t payloadLen)
{
    const hpHeader_t hdr = { .msgId = msgId, .flags = HP_FLAG_RESPONSE, .tag = tag };
    int16_t          st  = (int16_t)status;

    if (status != CTRL_OK || payloadLen < HOST_SESSION_STATUS_BYTES) {
        payload    = (uint8_t *)&st;
        payloadLen = HOST_SESSION_STATUS_BYTES;
    } else {
        memcpy(payload, &st, HOST_SESSION_STATUS_BYTES);   /* the owner cannot get it wrong */
    }

    s->stats.requests++;
    if (!hostSessionSend(s, (const uint8_t *)&hdr, HP_HEADER_BYTES, payload, payloadLen, 0U)) {
        s->stats.responsesDropped++;
    }
}


/* --- Session requests -------------------------------------------------------- */

static ctrlStatus_e hostSessionHello(hostSession_t *s, const uint8_t *req, uint16_t len,
                                     uint8_t *rsp, uint16_t *rspLen)
{
    if (len != HP_HELLO_REQ_BYTES) {
        return CTRL_ERR_BAD_LENGTH;
    }
    hpHelloReq_t hello;
    memcpy(&hello, req, sizeof(hello));
    if (hello.protoMajor != HP_PROTO_MAJOR) {
        s->open = false;
        return CTRL_ERR_PROTO_MISMATCH;
    }

    s->open        = true;
    s->cursor      = eventLogNext(s->cfg.log);
    s->deferActive = false;                 /* an answer owed to the previous host is void */
    memcpy(s->clientId, hello.clientId, sizeof(s->clientId));
    s->stats.opens++;
    s->bindState = (s->cfg.onHello != NULL) ? s->cfg.onHello(s, &hello) : s->cfg.bindState;

    hpHelloRsp_t *r = (hpHelloRsp_t *)rsp;
    memset(r, 0, sizeof(*r));
    r->protoMajor    = HP_PROTO_MAJOR;
    r->protoMinor    = HP_PROTO_MINOR;
    r->transport     = s->cfg.transport;
    r->bindState     = s->bindState;
    r->bootId        = s->cfg.bootId;
    r->sysUs         = s->cfg.nowUs();
    r->logOldestSeq  = eventLogOldest(s->cfg.log);
    r->logNextSeq    = eventLogNext(s->cfg.log);
    r->otherSessions = (s->cfg.otherSessions != NULL) ? s->cfg.otherSessions(s) : 0U;
    r->boardType     = s->cfg.boardType;
    *rspLen = HP_HELLO_RSP_BYTES;
    return CTRL_OK;
}

static ctrlStatus_e hostSessionPing(hostSession_t *s, const uint8_t *req, uint16_t len,
                                    uint8_t *rsp, uint16_t *rspLen)
{
    if (len != HP_PING_REQ_BYTES) {
        return CTRL_ERR_BAD_LENGTH;
    }
    hpPingReq_t ping;
    memcpy(&ping, req, sizeof(ping));

    hpPingRsp_t *r = (hpPingRsp_t *)rsp;
    r->hostMs = ping.hostMs;
    r->sysUs  = s->cfg.nowUs();
    *rspLen = HP_PING_RSP_BYTES;
    return CTRL_OK;
}

static ctrlStatus_e hostSessionResume(hostSession_t *s, const uint8_t *req, uint16_t len,
                                      uint8_t *rsp, uint16_t *rspLen)
{
    if (len != HP_RESUME_REQ_BYTES) {
        return CTRL_ERR_BAD_LENGTH;
    }
    hpResumeReq_t resume;
    memcpy(&resume, req, sizeof(resume));

    uint32_t oldest = eventLogOldest(s->cfg.log);
    uint32_t next   = eventLogNext(s->cfg.log);
    if (resume.bootId != s->cfg.bootId) {
        return CTRL_ERR_BOOT_MISMATCH;
    }
    if (resume.fromSeq < oldest) {
        return CTRL_ERR_LOG_GAP;
    }
    if (resume.fromSeq > next) {
        return CTRL_ERR_INVALID_ARG;
    }

    s->cursor = resume.fromSeq;

    hpResumeRsp_t *r = (hpResumeRsp_t *)rsp;
    r->logOldestSeq = oldest;
    r->logNextSeq   = next;
    *rspLen = HP_RESUME_RSP_BYTES;
    return CTRL_OK;
}


/* --- Frames in ------------------------------------------------------------- */

static void hostSessionHandleFrame(hostSession_t *s, const uint8_t *raw, uint16_t len)
{
    if (len < HP_HEADER_BYTES) {
        s->stats.badFrames++;
        return;
    }
    hpHeader_t hdr;
    memcpy(&hdr, raw, sizeof(hdr));
    if (hdr.flags != 0U) {
        s->stats.badFrames++;            /* the host only ever sends requests */
        return;
    }
    s->stats.framesIn++;

    const uint8_t *req    = raw + HP_HEADER_BYTES;
    uint16_t       reqLen = (uint16_t)(len - HP_HEADER_BYTES);
    uint8_t        rsp[HP_MAX_PAYLOAD];
    uint16_t       rspLen = 0U;
    ctrlStatus_e   status;

    memset(rsp, 0, sizeof(rsp));

    if (hdr.msgId == HP_MSG_HELLO) {
        status = hostSessionHello(s, req, reqLen, rsp, &rspLen);
    } else if (!s->open) {
        status = CTRL_ERR_NO_SESSION;
    } else if (hdr.msgId == HP_MSG_PING) {
        status = hostSessionPing(s, req, reqLen, rsp, &rspLen);
    } else if (s->bindState == (uint8_t)HP_BIND_UNBOUND) {
        s->stats.notBound++;
        status = CTRL_ERR_NOT_BOUND;
    } else if (hdr.msgId == HP_MSG_RESUME) {
        status = hostSessionResume(s, req, reqLen, rsp, &rspLen);
    } else if (s->cfg.onRequest != NULL) {
        const bool owedBefore = s->deferActive;
        s->deferRequested = false;
        status = s->cfg.onRequest(s, hdr.msgId, req, reqLen, rsp, (uint16_t)sizeof(rsp), &rspLen);
        if (s->deferRequested) {
            s->deferRequested = false;
            if (!owedBefore) {
                s->deferActive = true;           /* the owner answers later */
                s->deferMsgId  = hdr.msgId;
                s->deferTag    = hdr.tag;
                return;
            }
            status = CTRL_ERR_BUSY;              /* one deferred answer at a time */
        } else if (status == CTRL_OK && (rspLen < HOST_SESSION_STATUS_BYTES || rspLen > sizeof(rsp))) {
            status = CTRL_ERR_INVALID_STATE;   /* owner bug: never send garbage */
        }
    } else {
        status = CTRL_ERR_UNSUPPORTED;
    }

    hostSessionRespond(s, hdr.msgId, hdr.tag, status, rsp, rspLen);
}

static bool hostSessionReceive(hostSession_t *s)
{
    uint8_t  chunk[64];
    uint16_t budget = HOST_SESSION_RX_BUDGET;
    bool     any    = false;

    while (budget > 0U) {
        uint16_t want = (budget < sizeof(chunk)) ? budget : (uint16_t)sizeof(chunk);
        uint16_t n    = s->io->read(chunk, want);
        if (n == 0U) {
            break;
        }
        budget = (uint16_t)(budget - n);
        for (uint16_t i = 0U; i < n; i++) {
            frameResult_e r = frameDecoderPush(&s->dec, chunk[i]);
            if (r == FRAME_READY) {
                hostSessionHandleFrame(s, s->dec.frame, s->dec.frameLen);
                any = true;
            } else if (r != FRAME_NONE) {
                s->stats.badFrames++;
            }
        }
    }
    return any;
}


/* --- Events out ------------------------------------------------------------ */

static bool hostSessionPump(hostSession_t *s)
{
    bool any = false;

    while (s->open && s->bindState != (uint8_t)HP_BIND_UNBOUND && s->cursor < eventLogNext(s->cfg.log)) {
        const eventLogSlot_t *slot = eventLogGet(s->cfg.log, s->cursor);
        if (slot == NULL) {
            /* Overwritten while we lagged: jump to the oldest held event. The
             * app sees the evSeq gap and asks RESUME, which then says
             * ERR_LOG_GAP. */
            s->cursor = eventLogOldest(s->cfg.log);
            continue;
        }

        uint8_t head[HP_HEADER_BYTES + EVENT_LOG_EVSEQ_BYTES];
        const hpHeader_t hdr = { .msgId = slot->msgId, .flags = HP_FLAG_EVENT, .tag = 0U };
        memcpy(head, &hdr, HP_HEADER_BYTES);
        memcpy(head + HP_HEADER_BYTES, &s->cursor, EVENT_LOG_EVSEQ_BYTES);

        if (!hostSessionSend(s, head, (uint16_t)sizeof(head), slot->body, slot->len,
                             HOST_SESSION_RESPONSE_RESERVE)) {
            break;                       /* no room: the event waits in the log */
        }
        s->cursor++;
        s->stats.eventsSent++;
        any = true;
    }
    return any;
}


/* --- API --------------------------------------------------------------------- */

void hostSessionInit(hostSession_t *s, const hostTransport_t *io, const hostSessionConfig_t *cfg)
{
    memset(s, 0, sizeof(*s));
    s->io        = io;
    s->cfg       = *cfg;
    s->bindState = cfg->bindState;
    frameDecoderInit(&s->dec, s->rxBuf, (uint16_t)sizeof(s->rxBuf));
}

void hostSessionReset(hostSession_t *s)
{
    s->open        = false;
    s->deferActive = false;
    s->bindState   = s->cfg.bindState;
    frameDecoderReset(&s->dec);
}

void hostSessionRun(hostSession_t *s)
{
    bool wrote = hostSessionReceive(s);
    wrote = hostSessionPump(s) || wrote;
    if (wrote) {
        s->io->flush();
    }
}

bool hostSessionIsOpen(const hostSession_t *s)
{
    return s->open;
}

void hostSessionSetBindState(hostSession_t *s, uint8_t bindState)
{
    s->bindState = bindState;
}

uint8_t hostSessionGetBindState(const hostSession_t *s)
{
    return s->bindState;
}

const uint8_t *hostSessionClientId(const hostSession_t *s)
{
    return s->clientId;
}

void hostSessionDefer(hostSession_t *s)
{
    s->deferRequested = true;
}

bool hostSessionHasDeferred(const hostSession_t *s)
{
    return s->deferActive;
}

bool hostSessionCompleteDeferred(hostSession_t *s, ctrlStatus_e status, uint8_t *payload,
                                 uint16_t payloadLen)
{
    if (!s->deferActive) {
        return false;
    }
    s->deferActive = false;
    hostSessionRespond(s, s->deferMsgId, s->deferTag, status, payload, payloadLen);
    s->io->flush();
    return true;
}

bool hostSessionSendStream(hostSession_t *s, uint8_t msgId, const void *payload, uint16_t len)
{
    if (!s->open || s->bindState == (uint8_t)HP_BIND_UNBOUND || len > HP_MAX_PAYLOAD) {
        return false;
    }
    const hpHeader_t hdr = { .msgId = msgId, .flags = HP_FLAG_STREAM, .tag = 0U };
    if (!hostSessionSend(s, (const uint8_t *)&hdr, HP_HEADER_BYTES, (const uint8_t *)payload, len,
                         HOST_SESSION_RESPONSE_RESERVE)) {
        s->stats.streamsDropped++;
        return false;
    }
    s->stats.streamsSent++;
    s->io->flush();
    return true;
}
