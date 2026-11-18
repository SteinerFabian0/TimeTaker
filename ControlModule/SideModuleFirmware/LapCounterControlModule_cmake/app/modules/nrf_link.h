/*
 * nRF link: the UART to the nRF52805 BLE bridge (CONTROL_FIRMWARE_GUIDE §5.4,
 * BLE_FIRMWARE_GUIDE §5, ble_link_protocol.h).
 *
 *   RX   USART1 receive-to-idle interrupts (HAL, two 64-byte buffers in turn)
 *        copy into a 1 KB ring. nrfLinkRun() feeds it through a frame_codec
 *        decoder for link frames. DATA payloads go into a second 1 KB ring:
 *        the BLE session's byte stream (nrfLinkRead). CTRL messages become
 *        nrfLinkEvent_t for host_manager (HELLO, LINK_UP/DOWN, BUTTON, STATS);
 *        CREDIT is handled here.
 *   TX   nrfLinkWrite() takes whole host frames into a 1 KB staging ring (all
 *        or nothing, like usbCdcWrite). nrfLinkRun() cuts it into DATA frames
 *        of up to 244 bytes and sends one only with a credit from the nRF and
 *        while a central is connected. CTRL frames (nrfLinkSendCtrl) go first
 *        and need no credit. One UART transmit in flight at a time.
 *   BLE_IRQ  (PB5) high = a central is connected. Polled every pass (the EXTI
 *        is rising-only in CubeMX) and authoritative: its fall resets the BLE
 *        session (nrfLinkTakeLinkReset), LINK_DOWN is the backup.
 *   Versions  NRF_HELLO with another link MAJOR: DATA is refused both ways
 *        (dropped, counted), CTRL keeps working.
 *   Boot  STM_HELLO is sent at init, so an nRF that booted first re-announces.
 *
 * With CTRL_ENABLE_NRF_LINK_ECHO every DATA payload goes straight back to the
 * nRF instead of into the session (BLE bench test).
 *
 * Owns: USART1 (PB6/PB7) through the generated handle and the HAL UART weak
 * callbacks, and the BLE_IRQ pin (read only). PB4 BOOT_REQ is unused
 * in v1 (no bootloader).
 * Context: API main loop only; the UART callbacks run at ISR prio 5 and only
 * move bytes.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ctrl_status.h"
#include "ble_link_protocol.h"

#define NRF_LINK_RING_BYTES     1024U
#define NRF_LINK_EVENT_DEPTH    8U

typedef enum {
    NRF_LINK_EV_HELLO = 0,     /* body.hello */
    NRF_LINK_EV_LINK_UP,       /* body.linkUp */
    NRF_LINK_EV_LINK_DOWN,     /* body.linkDown (or a BLE_IRQ fall: hciReason 0) */
    NRF_LINK_EV_BUTTON,        /* body.button */
    NRF_LINK_EV_STATS,         /* body.stats */
} nrfLinkEventKind_e;

typedef struct {
    uint8_t kind;              /* nrfLinkEventKind_e */
    union {
        bleCtrlNrfHello_t hello;
        bleCtrlLinkUp_t   linkUp;
        bleCtrlLinkDown_t linkDown;
        bleCtrlButton_t   button;
        bleCtrlStats_t    stats;
    } body;
} nrfLinkEvent_t;

typedef struct {
    uint32_t framesIn;
    uint32_t badFrames;        /* CRC / COBS / overflow / short */
    uint32_t badChannel;
    uint32_t badData;
    uint32_t unknownCtrl;
    uint32_t shortCtrl;
    uint32_t dataDroppedVersion;   /* DATA refused on a link MAJOR mismatch */
    uint32_t dataDroppedFull;      /* session ring full */
    uint32_t rxOverflow;           /* UART bytes lost: RX ring full */
    uint32_t uartErrors;
    uint32_t dataFramesOut;
    uint32_t ctrlFramesOut;
    uint32_t ctrlDropped;          /* CTRL queue full */
    uint32_t overCredit;           /* credit count overflowed (desync) */
    uint32_t eventsDropped;
} nrfLinkStats_t;

ctrlStatus_e nrfLinkInit(void);

void nrfLinkRun(void);

/* The BLE session's transport (hostTransport_t). */
uint16_t nrfLinkRead(uint8_t *buf, uint16_t max);
uint16_t nrfLinkWriteSpace(void);
uint16_t nrfLinkWrite(const uint8_t *buf, uint16_t len);
void     nrfLinkFlush(void);

/* True once after a central disconnected (BLE_IRQ fell, or LINK_DOWN). */
bool nrfLinkTakeLinkReset(void);

/* BLE_IRQ: a central is connected. */
bool nrfLinkIsConnected(void);

/* An NRF_HELLO was seen; *hello is the last one. */
bool nrfLinkGetHello(bleCtrlNrfHello_t *hello);

/* NRF_HELLO seen with our link MAJOR. */
bool nrfLinkVersionOk(void);

bool nrfLinkTakeEvent(nrfLinkEvent_t *ev);

/* Queues one STM -> nRF CTRL message. CTRL_ERR_BUSY if the CTRL queue is full. */
ctrlStatus_e nrfLinkSendCtrl(uint8_t type, const void *body, uint16_t bodyLen);

const nrfLinkStats_t *nrfLinkGetStats(void);
