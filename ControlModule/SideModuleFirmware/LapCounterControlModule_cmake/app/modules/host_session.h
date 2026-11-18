/*
 * One host session on one transport: the session layer of the host protocol.
 *
 * The bytes come from a transport (USB CDC now, the nRF link later; see
 * hostTransport_t). Frames go through frame_codec. This layer answers the
 * session requests itself and hands every other request to its owner:
 *
 *   HELLO   opens the session (protocol MAJOR must match). The event cursor
 *           starts at the log head: only new events, unless the app sends
 *           RESUME. A second HELLO re-opens it.
 *   PING    echoes hostMs, adds sysUs: heartbeat and clock estimate.
 *   RESUME  moves the cursor back to fromSeq of this bootId, so the log
 *           replays what the app missed (ERR_BOOT_MISMATCH / ERR_LOG_GAP when
 *           it cannot).
 *   other   before HELLO: ERR_NO_SESSION. After: the owner's hostRequestFn_t,
 *           or ERR_UNSUPPORTED.
 *
 * Bind state (CONTROL_FIRMWARE_GUIDE §5.4): every session has its own, mutable
 * bind state. It starts at the configured one (TRUSTED on USB, UNBOUND on
 * BLE). At HELLO the owner's onHello() decides it (the bind policy lives in
 * host_manager, not here), and hostSessionSetBindState() changes it later
 * (pairing window opened, list cleared). While UNBOUND the session serves
 * HELLO and PING only: every other request gets ERR_NOT_BOUND without
 * reaching the owner, and no events or streams are sent, so an unbound phone
 * sees nothing of the race.
 *
 * Deferred responses: a request whose answer needs a bus round trip (for
 * example LANE_STATUS) may call hostSessionDefer() inside onRequest. No
 * response goes out then; the owner sends it later with
 * hostSessionCompleteDeferred(), same msgId and tag. One at a time per
 * session; a second defer is answered ERR_BUSY at once.
 *
 * Streams (STR_*, DBG_LOG): best effort. hostSessionSendStream() sends one
 * only if the transport has room for it AND the response reserve, and drops
 * it otherwise. Never logged, never retried.
 *
 * Responses: one per request, same msgId and tag, RESPONSE flag, payload =
 * status i16 (+ body on OK). On error the payload is the status alone. A frame
 * is only taken from the transport while it has HOST_SESSION_RESPONSE_RESERVE
 * free, so a response is never dropped for lack of room: under backpressure the
 * request waits in the transport's receive queue instead.
 *
 * Events: while open, each run sends log[cursor..next) as EVENT frames. Each
 * goes only if the transport has room for it AND still has
 * HOST_SESSION_RESPONSE_RESERVE free, so a response always fits behind the
 * event pump. An event that does not fit waits in the log. It is never lost
 * or skipped.
 *
 * Pure logic: no HAL. The transport and the clock are function pointers, so
 * the host test drives it with fakes (test/host_session). Main loop only.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "host_protocol.h"
#include "frame_codec.h"
#include "event_log.h"

/* Largest frame on the wire. The pump keeps this much transport space free
 * for responses. */
#define HOST_SESSION_WIRE_MAX           FRAME_CODEC_WIRE_BOUND(HP_MAX_FRAME_RAW)
#define HOST_SESSION_RESPONSE_RESERVE   HOST_SESSION_WIRE_MAX

/* Bytes read from the transport per run, so one busy link cannot starve the
 * super-loop. */
#define HOST_SESSION_RX_BUDGET          512U

typedef struct {
    uint16_t (*read)(uint8_t *buf, uint16_t max);
    uint16_t (*writeSpace)(void);
    uint16_t (*write)(const uint8_t *buf, uint16_t len);  /* all or nothing */
    void     (*flush)(void);
} hostTransport_t;

typedef struct hostSession_s hostSession_t;

/*
 * The owner's handler for every request past the session layer. On success it
 * fills rsp with the complete response payload struct (status field
 * included, which the session forces to CTRL_OK), sets *rspLen and returns
 * CTRL_OK. On failure it returns the error and the session sends the status
 * alone.
 */
typedef ctrlStatus_e (*hostRequestFn_t)(hostSession_t *session, uint8_t msgId,
                                        const uint8_t *req, uint16_t reqLen,
                                        uint8_t *rsp, uint16_t rspCap, uint16_t *rspLen);

typedef struct {
    uint8_t         transport;      /* hpTransport_e of this session */
    uint8_t         bindState;      /* hpBindState_e reported in HELLO */
    uint8_t         boardType;      /* hpBoardType_e reported in HELLO: CONTROL or CONTROL_BOOT */
    uint32_t        bootId;
    eventLog_t     *log;
    uint64_t      (*nowUs)(void);
    uint8_t       (*otherSessions)(const hostSession_t *self);   /* may be NULL */
    hostRequestFn_t onRequest;      /* may be NULL: everything else unsupported */
    /* Bind policy hook, called on every accepted HELLO before the response is
     * built. Returns the hpBindState_e the session gets. May be NULL: the
     * session keeps the configured bindState. */
    uint8_t       (*onHello)(hostSession_t *self, const hpHelloReq_t *hello);
} hostSessionConfig_t;

/* Diagnostics. Free-running; only hostSessionInit() clears them. */
typedef struct {
    uint32_t framesIn;              /* well-formed frames received */
    uint32_t badFrames;             /* CRC/COBS/overflow/short, or a malformed header */
    uint32_t requests;              /* requests answered (any status) */
    uint32_t responsesDropped;      /* response did not fit the transport */
    uint32_t eventsSent;
    uint32_t opens;                 /* successful HELLOs */
    uint32_t notBound;              /* requests refused with ERR_NOT_BOUND */
    uint32_t streamsSent;
    uint32_t streamsDropped;        /* no room beside the response reserve */
} hostSessionStats_t;

struct hostSession_s {
    const hostTransport_t *io;
    hostSessionConfig_t    cfg;
    frameDecoder_t         dec;
    uint8_t                rxBuf[FRAME_CODEC_RX_BUF_BYTES(HP_MAX_FRAME_RAW)];
    uint8_t                txBuf[HOST_SESSION_WIRE_MAX];
    bool                   open;
    uint32_t               cursor;  /* next evSeq to send */
    uint8_t                clientId[16];
    uint8_t                bindState;      /* hpBindState_e, mutable */
    bool                   deferRequested; /* set by hostSessionDefer() inside onRequest */
    bool                   deferActive;    /* a deferred response is owed */
    uint8_t                deferMsgId;
    uint16_t               deferTag;
    uint8_t                rxPend[64];     /* read from the transport, not yet decoded */
    uint16_t               rxPos, rxLen;
    hostSessionStats_t     stats;
};

void hostSessionInit(hostSession_t *s, const hostTransport_t *io, const hostSessionConfig_t *cfg);

/* The link went away (unplug, port closed): close the session and drop any
 * partial frame. The next HELLO opens it again. */
void hostSessionReset(hostSession_t *s);

/* Receive and answer, then pump events. Every super-loop pass. */
void hostSessionRun(hostSession_t *s);

bool hostSessionIsOpen(const hostSession_t *s);

/* Bind state of this session (hpBindState_e). Setting it on a closed session
 * is allowed and lasts until the next HELLO or reset. */
void    hostSessionSetBindState(hostSession_t *s, uint8_t bindState);
uint8_t hostSessionGetBindState(const hostSession_t *s);

/* clientId of the last accepted HELLO (all zero before). */
const uint8_t *hostSessionClientId(const hostSession_t *s);

/* Inside onRequest only: send no response now, the owner completes it. */
void hostSessionDefer(hostSession_t *s);

/* Whether a deferred response is owed (the owner refuses a second one). */
bool hostSessionHasDeferred(const hostSession_t *s);

/*
 * Sends the owed response: status, and on CTRL_OK the full payload struct
 * (status field included, forced to OK). Returns false if none was owed (the
 * session was reset or re-opened meanwhile: the answer is dropped).
 */
bool hostSessionCompleteDeferred(hostSession_t *s, ctrlStatus_e status, uint8_t *payload,
                                 uint16_t payloadLen);

/*
 * Sends one stream frame (STREAM flag, tag 0) if the session is open, not
 * UNBOUND, and the transport has room for it plus the response reserve.
 * Returns whether it went out.
 */
bool hostSessionSendStream(hostSession_t *s, uint8_t msgId, const void *payload, uint16_t len);
