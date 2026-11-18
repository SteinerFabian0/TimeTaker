/*
 * The UART link to the STM32: UARTE0 async (DMA) at 460800 8N1, framing via
 * the control tree's frame_codec. BLE_FIRMWARE_GUIDE.md §5, §8.2.
 *
 *   RX: UARTE DMA, 2 x TT_UART_RX_DMA_BUF bytes, swapped by the driver
 *       -> UART_RX_RDY (ISR) copies into the RX ring -> wake()
 *       -> main loop: stmLinkPoll() runs the link decoder over the ring.
 *   TX: stmLinkSendData() / stmLinkSendCtrl() encode one whole link frame
 *       into the TX ring under a spinlock (any thread, never blocks; a full
 *       ring drops the frame and counts it), then the UARTE DMA sends
 *       contiguous runs of the ring and advances on UART_TX_DONE.
 *
 * Errors (framing, overrun, parity, break, RX ring overflow) are counted and
 * RX is re-enabled at once; the decoder resyncs at the next 0x00.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "link_msg.h"

typedef struct {
    uint32_t uartErrors;     /* UART_RX_STOPPED (framing/overrun/parity/break) */
    uint32_t rxOverflows;    /* bytes lost because the RX ring was full        */
    uint32_t badFrames;      /* decoder CRC/COBS/overflow/short                 */
    uint32_t badMessages;    /* bad channel, DATA length, short CTRL            */
    uint32_t ctrlIgnored;    /* unknown CTRL type or out-of-range value         */
    uint32_t txDropped;      /* frames not sent: TX ring full                   */
    uint32_t goodFrames;
} stmLinkCounters_t;

/* wake() is called from ISR context when bytes arrive. */
int stmLinkInit(void (*wake)(void));

/* Main loop: decodes buffered bytes until one message is ready. Returns
 * LINK_MSG_DATA or LINK_MSG_CTRL with *out filled (DATA points into the
 * decoder buffer: valid until the next call), or 0 when the ring is empty.
 * Bad frames and ignored messages are counted and skipped. */
int stmLinkPoll(linkMsg_t *out);

/* Any thread. false = dropped (TX ring full or bad length), counted. */
bool stmLinkSendData(const uint8_t *data, uint16_t len);
bool stmLinkSendCtrl(uint8_t type, const void *msg, uint16_t msgLen);

/* k_uptime_get_32() of the last good frame from the STM (0 = none yet). */
uint32_t stmLinkLastRxMs(void);

void stmLinkGetCounters(stmLinkCounters_t *out);
