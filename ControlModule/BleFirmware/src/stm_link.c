/*
 * The UART link to the STM32. See stm_link.h.
 */

#include "stm_link.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>

#include "byte_ring.h"
#include "frame_codec.h"

LOG_MODULE_REGISTER(stm_link, LOG_LEVEL_INF);

BUILD_ASSERT((CONFIG_TT_UART_RX_RING & (CONFIG_TT_UART_RX_RING - 1)) == 0, "RX ring: power of two");
BUILD_ASSERT((CONFIG_TT_UART_TX_RING & (CONFIG_TT_UART_TX_RING - 1)) == 0, "TX ring: power of two");
BUILD_ASSERT(CONFIG_TT_UART_TX_RING >= LINK_WIRE_MAX, "TX ring must hold the largest link frame");
BUILD_ASSERT(DT_PROP(DT_NODELABEL(uart0), current_speed) == BLE_LINK_BAUD, "uart0 baud != link baud");

/* RX inactivity before the driver hands over a partly filled buffer. One byte
 * is 21.7 us at 460800; a few byte times keeps latency low without an event
 * per byte. */
#define RX_TIMEOUT_US 100

static const struct device *const uart = DEVICE_DT_GET(DT_NODELABEL(uart0));

static void (*wakeFn)(void);

/* --- RX --------------------------------------------------------------------- */

static uint8_t    rxDma[2][CONFIG_TT_UART_RX_DMA_BUF];
static uint8_t    rxDmaNext;                 /* buffer to hand out on the next request */
static uint8_t    rxRingBuf[CONFIG_TT_UART_RX_RING];
static byteRing_t rxRing;

static uint8_t        decoderBuf[LINK_RX_BUF_BYTES];
static frameDecoder_t decoder;

/* --- TX --------------------------------------------------------------------- */

static uint8_t          txRingBuf[CONFIG_TT_UART_TX_RING];
static byteRing_t       txRing;
static uint8_t          txScratch[LINK_WIRE_MAX];   /* encode buffer, under txLock */
static struct k_spinlock txLock;
static uint16_t         txInFlight;                 /* bytes handed to uart_tx, 0 = idle */

/* --- Counters ----------------------------------------------------------------- */

static atomic_t uartErrors;
static atomic_t rxOverflows;
static atomic_t txDropped;
static uint32_t badMessages;
static uint32_t ctrlIgnored;
static uint32_t lastRxMs;

/* Starts the next contiguous TX run if the UART is idle. Call under txLock. */
static void txKickLocked(void)
{
    if (txInFlight != 0U) {
        return;
    }
    const uint8_t *data;
    const uint16_t len = byteRingPeek(&txRing, &data);
    if (len == 0U) {
        return;
    }
    if (uart_tx(uart, data, len, SYS_FOREVER_US) == 0) {
        txInFlight = len;
    }
}

static volatile bool rxEnableFailed;         /* RX is off: stmLinkPoll() retries */

static void rxEnable(void)
{
    rxDmaNext = 1U;
    if (uart_rx_enable(uart, rxDma[0], sizeof(rxDma[0]), RX_TIMEOUT_US) != 0) {
        atomic_inc(&uartErrors);
        rxEnableFailed = true;
    } else {
        rxEnableFailed = false;
    }
}

static void uartCallback(const struct device *dev, struct uart_event *evt, void *user_data)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(user_data);

    switch (evt->type) {
    case UART_TX_DONE:
    case UART_TX_ABORTED: {
        k_spinlock_key_t key = k_spin_lock(&txLock);
        byteRingConsume(&txRing, (uint16_t)evt->data.tx.len);
        txInFlight = 0U;
        txKickLocked();
        k_spin_unlock(&txLock, key);
        break;
    }

    case UART_RX_RDY: {
        const uint8_t *p = &evt->data.rx.buf[evt->data.rx.offset];
        const uint16_t n = (uint16_t)evt->data.rx.len;
        const uint16_t w = byteRingWrite(&rxRing, p, n);
        if (w < n) {
            atomic_add(&rxOverflows, (atomic_val_t)(n - w));
        }
        if (wakeFn != NULL) {
            wakeFn();
        }
        break;
    }

    case UART_RX_BUF_REQUEST:
        (void)uart_rx_buf_rsp(uart, rxDma[rxDmaNext], sizeof(rxDma[0]));
        rxDmaNext ^= 1U;
        break;

    case UART_RX_BUF_RELEASED:
        break;

    case UART_RX_STOPPED:
        atomic_inc(&uartErrors);           /* RX_DISABLED follows */
        break;

    case UART_RX_DISABLED:
        rxEnable();                        /* allowed from this callback */
        break;

    default:
        break;
    }
}

int stmLinkInit(void (*wake)(void))
{
    wakeFn = wake;

    if (!device_is_ready(uart)) {
        return -ENODEV;
    }
    (void)byteRingInit(&rxRing, rxRingBuf, sizeof(rxRingBuf));
    (void)byteRingInit(&txRing, txRingBuf, sizeof(txRingBuf));
    frameDecoderInit(&decoder, decoderBuf, sizeof(decoderBuf));

    int err = uart_callback_set(uart, uartCallback, NULL);
    if (err != 0) {
        return err;
    }
    rxEnable();
    return 0;
}

/* One decoded frame: the message kind to return, or 0 (counted, skipped). */
static int takeFrame(linkMsg_t *out)
{
    const linkMsgResult_e m = linkParse(decoder.frame, decoder.frameLen, out);

    if (m == LINK_MSG_DATA || m == LINK_MSG_CTRL || m == LINK_MSG_CTRL_UNKNOWN
        || m == LINK_MSG_CTRL_BAD_VALUE) {
        /* A well-formed frame: the STM is alive. */
        const uint32_t now = k_uptime_get_32();
        lastRxMs = (now != 0U) ? now : 1U;
    }
    if (m == LINK_MSG_DATA || m == LINK_MSG_CTRL) {
        return (int)m;
    }
    if (m == LINK_MSG_CTRL_UNKNOWN || m == LINK_MSG_CTRL_BAD_VALUE) {
        ctrlIgnored++;
    } else {
        badMessages++;
    }
    return 0;
}

int stmLinkPoll(linkMsg_t *out)
{
    const uint8_t *run;
    uint16_t n;

    if (rxEnableFailed) {
        rxEnable();                       /* a failed re-enable must not stop RX for good */
    }

    /* Feed the decoder from the ring's contiguous runs; stop right after a
     * complete message so out->data (in the decoder buffer) stays valid. */
    while ((n = byteRingPeek(&rxRing, &run)) != 0U) {
        uint16_t used   = 0U;
        int      result = 0;
        while (used < n && result == 0) {
            if (frameDecoderPush(&decoder, run[used++]) == FRAME_READY) {
                result = takeFrame(out);
            }
            /* Decoder errors are counted in decoder.stats. */
        }
        byteRingConsume(&rxRing, used);
        if (result != 0) {
            return result;
        }
    }
    return 0;
}

static bool txFrame(uint8_t ch, uint8_t ctrlType, const void *msg, uint16_t len)
{
    bool ok = false;
    k_spinlock_key_t key = k_spin_lock(&txLock);

    const uint16_t wire = (ch == BLE_LINK_CH_DATA)
        ? linkEncodeData((const uint8_t *)msg, len, txScratch, sizeof(txScratch))
        : linkEncodeCtrl(ctrlType, msg, len, txScratch, sizeof(txScratch));
    if (wire != 0U && byteRingWriteAll(&txRing, txScratch, wire)) {
        ok = true;
        txKickLocked();
    }

    k_spin_unlock(&txLock, key);

    if (!ok) {
        atomic_inc(&txDropped);
    }
    return ok;
}

bool stmLinkSendData(const uint8_t *data, uint16_t len)
{
    return txFrame(BLE_LINK_CH_DATA, 0U, data, len);
}

bool stmLinkSendCtrl(uint8_t type, const void *msg, uint16_t msgLen)
{
    return txFrame(BLE_LINK_CH_CTRL, type, msg, msgLen);
}

uint32_t stmLinkLastRxMs(void)
{
    return lastRxMs;
}

void stmLinkGetCounters(stmLinkCounters_t *out)
{
    const frameCodecStats_t *s = &decoder.stats;
    out->uartErrors  = (uint32_t)atomic_get(&uartErrors);
    out->rxOverflows = (uint32_t)atomic_get(&rxOverflows);
    out->badFrames   = s->crcErrors + s->cobsErrors + s->overflows + s->shortFrames;
    out->badMessages = badMessages;
    out->ctrlIgnored = ctrlIgnored;
    out->txDropped   = (uint32_t)atomic_get(&txDropped);
    out->goodFrames  = s->frames;
}
