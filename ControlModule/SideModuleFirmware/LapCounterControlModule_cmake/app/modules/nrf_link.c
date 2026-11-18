/*
 * nRF link. See nrf_link.h.
 */

#include "nrf_link.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "stm32_hal.h"
#include "mx_usart1.h"
#include "mx_gpio_default.h"
#include "ctrl_status.h"
#include "ctrl_behavior.h"
#include "byte_ring.h"
#include "frame_codec.h"
#include "nrf_link_proto.h"

#define NRF_LINK_RX_CHUNK      64U
#define NRF_LINK_CTRL_BYTES    128U
#define NRF_LINK_RX_BUDGET     512U   /* bytes decoded per pass */

static hal_uart_handle_t *nrfLinkUart;

/* ISR -> loop */
static uint8_t           nrfLinkRxChunk[2][NRF_LINK_RX_CHUNK];
static volatile uint8_t  nrfLinkRxIdx;
static uint8_t           nrfLinkRxStore[NRF_LINK_RING_BYTES];
static byteRing_t        nrfLinkRxRing;
static volatile bool     nrfLinkTxBusy;
static volatile bool     nrfLinkRxNeedsRestart;

/* Loop only */
static uint8_t           nrfLinkDataStore[NRF_LINK_RING_BYTES];
static byteRing_t        nrfLinkDataRing;         /* DATA payloads -> BLE session */
static uint8_t           nrfLinkStageStore[NRF_LINK_RING_BYTES];
static byteRing_t        nrfLinkStageRing;        /* BLE session -> DATA frames */
static uint8_t           nrfLinkCtrlStore[NRF_LINK_CTRL_BYTES];
static byteRing_t        nrfLinkCtrlRing;         /* encoded CTRL frames */

static frameDecoder_t    nrfLinkDec;
static uint8_t           nrfLinkDecBuf[FRAME_CODEC_RX_BUF_BYTES(BLE_LINK_RAW_MAX)];
static uint8_t           nrfLinkTxFrame[NRF_LINK_WIRE_MAX > NRF_LINK_CTRL_BYTES ? NRF_LINK_WIRE_MAX : NRF_LINK_CTRL_BYTES];

static nrfLinkCredits_t  nrfLinkCredits;
static bleCtrlNrfHello_t nrfLinkHello;
static bool              nrfLinkHelloSeen;
static bool              nrfLinkConnected;
static bool              nrfLinkReset;

static nrfLinkEvent_t    nrfLinkEvents[NRF_LINK_EVENT_DEPTH];
static uint8_t           nrfLinkEvHead;
static uint8_t           nrfLinkEvCount;

static nrfLinkStats_t    nrfLinkStats;


/* --- UART callbacks (ISR prio 5): bytes only --------------------------------- */

static void nrfLinkStartRx(void)
{
    if (HAL_UART_ReceiveToIdle_IT(nrfLinkUart, nrfLinkRxChunk[nrfLinkRxIdx], NRF_LINK_RX_CHUNK) != HAL_OK) {
        nrfLinkRxNeedsRestart = true;    /* the loop tries again */
    } else {
        nrfLinkRxNeedsRestart = false;
    }
}

void HAL_UART_RxCpltCallback(hal_uart_handle_t *huart, uint32_t size_byte, hal_uart_rx_event_types_t rx_event)
{
    (void)rx_event;
    if (huart != nrfLinkUart) {
        return;
    }
    const uint8_t done = nrfLinkRxIdx;
    nrfLinkRxIdx = (uint8_t)(done ^ 1U);
    nrfLinkStartRx();                    /* the 8-byte RX FIFO covers the gap */
    if (size_byte > 0U && !byteRingPut(&nrfLinkRxRing, nrfLinkRxChunk[done], (uint16_t)size_byte)) {
        nrfLinkStats.rxOverflow += size_byte;
    }
}

void HAL_UART_ErrorCallback(hal_uart_handle_t *huart)
{
    if (huart != nrfLinkUart) {
        return;
    }
    nrfLinkStats.uartErrors++;
    if (huart->rx_state == HAL_UART_RX_STATE_IDLE) {
        nrfLinkStartRx();                /* overrun stops the reception: restart */
    } else {
        nrfLinkRxNeedsRestart = true;    /* still winding down: the loop retries */
    }
}

void HAL_UART_TxCpltCallback(hal_uart_handle_t *huart)
{
    if (huart == nrfLinkUart) {
        nrfLinkTxBusy = false;
    }
}


/* --- Events ------------------------------------------------------------------ */

static void nrfLinkPushEvent(const nrfLinkEvent_t *ev)
{
    if (nrfLinkEvCount >= NRF_LINK_EVENT_DEPTH) {
        nrfLinkStats.eventsDropped++;
        return;
    }
    nrfLinkEvents[(nrfLinkEvHead + nrfLinkEvCount) % NRF_LINK_EVENT_DEPTH] = *ev;
    nrfLinkEvCount++;
}

bool nrfLinkTakeEvent(nrfLinkEvent_t *ev)
{
    if (nrfLinkEvCount == 0U) {
        return false;
    }
    *ev = nrfLinkEvents[nrfLinkEvHead];
    nrfLinkEvHead = (uint8_t)((nrfLinkEvHead + 1U) % NRF_LINK_EVENT_DEPTH);
    nrfLinkEvCount--;
    return true;
}


/* --- RX ---------------------------------------------------------------------- */

static void nrfLinkDropSession(void)
{
    byteRingClear(&nrfLinkDataRing);
    byteRingClear(&nrfLinkStageRing);
    nrfLinkReset = true;
}

static void nrfLinkHandleFrame(const uint8_t *raw, uint16_t len)
{
    nrfLinkMsg_t   msg;
    nrfLinkEvent_t ev;
    memset(&ev, 0, sizeof(ev));

    switch (nrfLinkProtoParse(raw, len, &msg)) {
    case NRF_LINK_PARSE_DATA:
        if (!nrfLinkHelloSeen || !nrfLinkProtoVersionOk(&nrfLinkHello)) {
            nrfLinkStats.dataDroppedVersion++;
#if CTRL_ENABLE_NRF_LINK_ECHO
        } else if (!byteRingPut(&nrfLinkStageRing, msg.data, msg.dataLen)) {
            nrfLinkStats.dataDroppedFull++;
#else
        } else if (!byteRingPut(&nrfLinkDataRing, msg.data, msg.dataLen)) {
            nrfLinkStats.dataDroppedFull++;   /* the host's retry covers it */
#endif
        }
        break;

    case NRF_LINK_PARSE_CTRL:
        switch (msg.ctrlType) {
        case BLE_CTRL_NRF_HELLO:
            nrfLinkHello     = msg.ctrl.hello;
            nrfLinkHelloSeen = true;
            nrfLinkCreditsReset(&nrfLinkCredits, msg.ctrl.hello.credits);   /* an nRF reset frees all */
            ev.kind = NRF_LINK_EV_HELLO;
            ev.body.hello = msg.ctrl.hello;
            nrfLinkPushEvent(&ev);
            break;
        case BLE_CTRL_LINK_UP:
            ev.kind = NRF_LINK_EV_LINK_UP;
            ev.body.linkUp = msg.ctrl.linkUp;
            nrfLinkPushEvent(&ev);
            break;
        case BLE_CTRL_LINK_DOWN:
            nrfLinkDropSession();
            ev.kind = NRF_LINK_EV_LINK_DOWN;
            ev.body.linkDown = msg.ctrl.linkDown;
            nrfLinkPushEvent(&ev);
            break;
        case BLE_CTRL_BUTTON:
            ev.kind = NRF_LINK_EV_BUTTON;
            ev.body.button = msg.ctrl.button;
            nrfLinkPushEvent(&ev);
            break;
        case BLE_CTRL_CREDIT:
            nrfLinkCreditsReturn(&nrfLinkCredits, msg.ctrl.credit.count);
            nrfLinkStats.overCredit = nrfLinkCredits.overReturned;
            break;
        case BLE_CTRL_STATS:
            ev.kind = NRF_LINK_EV_STATS;
            ev.body.stats = msg.ctrl.stats;
            nrfLinkPushEvent(&ev);
            break;
        default:
            break;
        }
        break;

    case NRF_LINK_PARSE_UNKNOWN_CTRL: nrfLinkStats.unknownCtrl++; break;
    case NRF_LINK_PARSE_SHORT_CTRL:   nrfLinkStats.shortCtrl++;   break;
    case NRF_LINK_PARSE_BAD_DATA:     nrfLinkStats.badData++;     break;
    default:                          nrfLinkStats.badChannel++;  break;
    }
}

static void nrfLinkReceive(void)
{
    uint8_t  chunk[64];
    uint16_t budget = NRF_LINK_RX_BUDGET;
    while (budget > 0U) {
        uint16_t n = byteRingGet(&nrfLinkRxRing, chunk, (budget < sizeof(chunk)) ? budget : (uint16_t)sizeof(chunk));
        if (n == 0U) {
            break;
        }
        budget = (uint16_t)(budget - n);
        for (uint16_t i = 0U; i < n; i++) {
            frameResult_e r = frameDecoderPush(&nrfLinkDec, chunk[i]);
            if (r == FRAME_READY) {
                nrfLinkStats.framesIn++;
                nrfLinkHandleFrame(nrfLinkDec.frame, nrfLinkDec.frameLen);
            } else if (r != FRAME_NONE) {
                nrfLinkStats.badFrames++;
            }
        }
    }
}


/* --- TX ---------------------------------------------------------------------- */

static void nrfLinkTransmit(void)
{
    if (nrfLinkTxBusy) {
        return;
    }

    /* CTRL first: whole frames already encoded, no credit needed. */
    uint16_t n = byteRingPeek(&nrfLinkCtrlRing, nrfLinkTxFrame, (uint16_t)sizeof(nrfLinkTxFrame));
    if (n > 0U) {
        nrfLinkTxBusy = true;
        if (HAL_UART_Transmit_IT(nrfLinkUart, nrfLinkTxFrame, n) != HAL_OK) {
            nrfLinkTxBusy = false;       /* the frames stay queued: retry next pass */
        } else {
            byteRingDrop(&nrfLinkCtrlRing, n);
        }
        return;
    }

    /* DATA: connected, right version, holding a credit. */
    if (!nrfLinkConnected || !nrfLinkVersionOk() || byteRingCount(&nrfLinkStageRing) == 0U) {
        return;
    }
    if (!nrfLinkCreditsTake(&nrfLinkCredits)) {
        return;
    }
    uint8_t  data[BLE_LINK_DATA_MAX];
    uint16_t len  = byteRingPeek(&nrfLinkStageRing, data, BLE_LINK_DATA_MAX);
    uint16_t wire = nrfLinkProtoEncodeData(data, len, nrfLinkTxFrame, (uint16_t)sizeof(nrfLinkTxFrame));
    if (wire == 0U) {
        nrfLinkCreditsReturn(&nrfLinkCredits, 1U);
        return;
    }
    nrfLinkTxBusy = true;
    if (HAL_UART_Transmit_IT(nrfLinkUart, nrfLinkTxFrame, wire) != HAL_OK) {
        nrfLinkTxBusy = false;           /* keep the bytes, give the credit back */
        nrfLinkCreditsReturn(&nrfLinkCredits, 1U);
        return;
    }
    byteRingDrop(&nrfLinkStageRing, len);
    nrfLinkStats.dataFramesOut++;
}


/* --- API ----------------------------------------------------------------------- */

ctrlStatus_e nrfLinkInit(void)
{
    (void)byteRingInit(&nrfLinkRxRing, nrfLinkRxStore, sizeof(nrfLinkRxStore));
    (void)byteRingInit(&nrfLinkDataRing, nrfLinkDataStore, sizeof(nrfLinkDataStore));
    (void)byteRingInit(&nrfLinkStageRing, nrfLinkStageStore, sizeof(nrfLinkStageStore));
    (void)byteRingInit(&nrfLinkCtrlRing, nrfLinkCtrlStore, sizeof(nrfLinkCtrlStore));
    frameDecoderInit(&nrfLinkDec, nrfLinkDecBuf, sizeof(nrfLinkDecBuf));
    nrfLinkCreditsReset(&nrfLinkCredits, 0U);
    nrfLinkHelloSeen = false;
    nrfLinkConnected = false;
    nrfLinkTxBusy    = false;
    nrfLinkRxIdx     = 0U;

    nrfLinkUart = mx_usart1_uart_gethandle();   /* initialised by mx_system_init() */
    nrfLinkStartRx();
    if (nrfLinkRxNeedsRestart) {
        return CTRL_ERR_HAL;
    }

    const bleCtrlStmHello_t hello = { .linkMajor = BLE_LINK_MAJOR, .linkMinor = BLE_LINK_MINOR };
    return nrfLinkSendCtrl(BLE_CTRL_STM_HELLO, &hello, sizeof(hello));
}

void nrfLinkRun(void)
{
    if (nrfLinkUart == NULL) {
        return;
    }
    if (nrfLinkRxNeedsRestart) {
        nrfLinkStartRx();
    }

    /* BLE_IRQ is the authority on "connected". */
    const bool up = HAL_GPIO_ReadPin(BLE_IRQ_PORT, BLE_IRQ_PIN) == HAL_GPIO_PIN_SET;
    if (nrfLinkConnected && !up) {
        nrfLinkDropSession();
        nrfLinkEvent_t ev = { .kind = NRF_LINK_EV_LINK_DOWN };
        nrfLinkPushEvent(&ev);
    }
    nrfLinkConnected = up;

    nrfLinkReceive();
    nrfLinkTransmit();
}

uint16_t nrfLinkRead(uint8_t *buf, uint16_t max)
{
    return byteRingGet(&nrfLinkDataRing, buf, max);
}

uint16_t nrfLinkWriteSpace(void)
{
    return (nrfLinkConnected && nrfLinkVersionOk()) ? byteRingFree(&nrfLinkStageRing) : 0U;
}

uint16_t nrfLinkWrite(const uint8_t *buf, uint16_t len)
{
    if (len == 0U || len > nrfLinkWriteSpace()) {
        return 0U;
    }
    return byteRingPut(&nrfLinkStageRing, buf, len) ? len : 0U;
}

void nrfLinkFlush(void)
{
    nrfLinkTransmit();   /* start the first DATA frame now, not next pass */
}

bool nrfLinkTakeLinkReset(void)
{
    const bool r = nrfLinkReset;
    nrfLinkReset = false;
    return r;
}

bool nrfLinkIsConnected(void)
{
    return nrfLinkConnected;
}

bool nrfLinkGetHello(bleCtrlNrfHello_t *hello)
{
    if (nrfLinkHelloSeen) {
        *hello = nrfLinkHello;
    }
    return nrfLinkHelloSeen;
}

bool nrfLinkVersionOk(void)
{
    return nrfLinkHelloSeen && nrfLinkProtoVersionOk(&nrfLinkHello);
}

ctrlStatus_e nrfLinkSendCtrl(uint8_t type, const void *body, uint16_t bodyLen)
{
    uint8_t  frame[32];
    uint16_t n = nrfLinkProtoEncodeCtrl(type, body, bodyLen, frame, sizeof(frame));
    if (n == 0U) {
        return CTRL_ERR_INVALID_ARG;
    }
    if (!byteRingPut(&nrfLinkCtrlRing, frame, n)) {
        nrfLinkStats.ctrlDropped++;
        return CTRL_ERR_BUSY;
    }
    nrfLinkStats.ctrlFramesOut++;
    return CTRL_OK;
}

const nrfLinkStats_t *nrfLinkGetStats(void)
{
    return &nrfLinkStats;
}
