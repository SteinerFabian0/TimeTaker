/*
 * ble_link_protocol.h — the UART link between the nRF52805 BLE bridge and the
 * control module STM32.
 *
 * THIS FILE IS BYTE-IDENTICAL IN TWO TREES:
 *   ControlModule/BleFirmware/src/ble_link_protocol.h
 *   ControlModule/SideModuleFirmware/LapCounterControlModule_cmake/app/config/ble_link_protocol.h
 * Both trees' host tests compare the two copies. Change it in one change, in
 * both places, and bump BLE_LINK_MINOR (compatible: new CTRL types, fields at
 * the END of a message) or BLE_LINK_MAJOR.
 *
 * Background: BLE_FIRMWARE_GUIDE.md §5, CONTROL_FIRMWARE_GUIDE.md §5.4,
 * SYSTEM_ARCHITECTURE.md §9.3.
 *
 * ---------------------------------------------------------------------------
 * Wire format
 * ---------------------------------------------------------------------------
 * USART1 (STM PB6/PB7) <-> UARTE0 (nRF P0.16/P0.18), 460800 8N1, no flow
 * control lines.
 *
 *   link frame = COBS( channel u8 | body | crc16 ) 0x00
 *   crc16      = CRC-16/CCITT-FALSE over channel|body, little-endian
 *
 * The codec is the control module's frame_codec.c/h (head = the channel
 * byte). The BLE firmware compiles that file directly; there is no copy.
 *
 * Channels:
 *   DATA  opaque host-protocol bytes, 1..BLE_LINK_DATA_MAX per frame. A byte
 *         stream: host frames may span DATA frames and share them. The nRF
 *         never looks inside.
 *   CTRL  body = type u8 + one of the packed structs below.
 *
 * Flow control, STM -> nRF only: the nRF grants `credits` DATA slots in
 * NRF_HELLO. The STM sends a DATA frame only while it holds a credit; the nRF
 * returns one with CREDIT when the last notification made from that slot has
 * been sent. Every NRF_HELLO resets the STM's credit count to its value.
 * NRF_HELLO.credits means "credits granted now": the slots free at that
 * moment. Slots still busy then return theirs later, each with a CREDIT.
 *
 * Compatibility: an unknown CTRL type is ignored (and counted). A CTRL body
 * shorter than its type needs is dropped; a longer one is accepted and the
 * extra bytes ignored. On a MAJOR mismatch the STM drops DATA and keeps CTRL.
 *
 * All values little-endian, all structs packed.
 */

#pragma once

#include <stdint.h>

#define BLE_LINK_MAJOR                 0U
#define BLE_LINK_MINOR                 2U

#define BLE_LINK_BAUD                  460800U

/* ATT payload at MTU 247; the STM never sends more per DATA frame. */
#define BLE_LINK_DATA_MAX              244U

/* Largest raw link frame: channel + biggest body + crc16. */
#define BLE_LINK_RAW_MAX               (1U + BLE_LINK_DATA_MAX + 2U)

typedef enum {
    BLE_LINK_CH_DATA = 0,
    BLE_LINK_CH_CTRL = 1,
} bleLinkChannel_e;

/* CTRL types: 0x01-0x3F nRF -> STM, 0x41-0x7F STM -> nRF. */
typedef enum {
    BLE_CTRL_NRF_HELLO  = 0x01,
    BLE_CTRL_LINK_UP    = 0x02,
    BLE_CTRL_LINK_DOWN  = 0x03,
    BLE_CTRL_BUTTON     = 0x04,
    BLE_CTRL_CREDIT     = 0x05,
    BLE_CTRL_STATS      = 0x06,

    BLE_CTRL_STM_HELLO  = 0x41,
    BLE_CTRL_UI_STATE   = 0x42,
    BLE_CTRL_CUE        = 0x43,
    BLE_CTRL_KICK       = 0x44,
    BLE_CTRL_ADVERTISE  = 0x45,
} bleCtrlType_e;

/* BUTTON press; same numbers as the host protocol's pressKind. */
typedef enum {
    BLE_PRESS_SHORT     = 1,
    BLE_PRESS_LONG      = 2,   /* held 2..10 s  */
    BLE_PRESS_VERY_LONG = 3,   /* held 10..30 s */
} blePress_e;

/* UI_STATE.session */
typedef enum {
    BLE_UI_SESSION_NONE    = 0,   /* no HELLO on BLE yet                    */
    BLE_UI_SESSION_UNBOUND = 1,   /* HELLO'd, not on the bind list          */
    BLE_UI_SESSION_BOUND   = 2,
} bleUiSession_e;

/* CUE.kind */
typedef enum {
    BLE_CUE_BIND_ACCEPTED = 1,    /* 3 quick flashes */
    BLE_CUE_BINDS_CLEARED = 2,    /* one 1 s flash   */
} bleCue_e;

/* NRF_HELLO.resetCause */
typedef enum {
    BLE_RESET_UNKNOWN  = 0,
    BLE_RESET_POWER_ON = 1,
    BLE_RESET_PIN      = 2,
    BLE_RESET_SOFTWARE = 3,
    BLE_RESET_WATCHDOG = 4,
    BLE_RESET_LOCKUP   = 5,
} bleResetCause_e;

/* ---- nRF -> STM ---------------------------------------------------------- */

typedef struct __attribute__((packed)) {
    uint8_t  linkMajor;
    uint8_t  linkMinor;
    uint8_t  fwMajor;
    uint8_t  fwMinor;
    uint8_t  fwPatch;
    uint8_t  credits;          /* credits granted now: DATA slots free at this
                                * moment (busy ones come back later by CREDIT) */
    uint8_t  resetCause;       /* bleResetCause_e                               */
    uint8_t  reserved;
    uint32_t fwGitRev;
    uint8_t  btAddr[6];        /* little-endian, as the controller reports it   */
} bleCtrlNrfHello_t;

typedef struct __attribute__((packed)) {
    uint16_t attMtu;
    uint8_t  txPhy;            /* 1 = 1M, 2 = 2M                                */
    uint8_t  rxPhy;
    uint16_t intervalUnits;    /* connection interval, 1.25 ms units            */
    uint16_t latency;
    uint16_t timeout10ms;      /* supervision timeout, 10 ms units              */
    uint16_t maxTxOctets;      /* link-layer data length                        */
} bleCtrlLinkUp_t;

typedef struct __attribute__((packed)) {
    uint8_t  hciReason;
} bleCtrlLinkDown_t;

typedef struct __attribute__((packed)) {
    uint8_t  press;            /* blePress_e                                    */
} bleCtrlButton_t;

typedef struct __attribute__((packed)) {
    uint8_t  count;            /* DATA slots freed                              */
} bleCtrlCredit_t;

typedef struct __attribute__((packed)) {
    int8_t   rssi;             /* dBm, 127 = unknown                            */
    uint8_t  slotsFree;        /* DATA slots free when this was built (0.2+). The
                                * STM may resync its credit count to it when it
                                * sent no DATA frame for the last 20 ms, which
                                * recovers credits lost to corrupted frames.   */
    uint16_t dataDropped;      /* writes/frames dropped on the nRF (saturates)  */
    uint16_t linkCrcErrors;    /* bad link frames from the STM (saturates)      */
    uint16_t uartErrors;       /* framing/overrun (saturates)                   */
} bleCtrlStats_t;

/* ---- STM -> nRF ---------------------------------------------------------- */

typedef struct __attribute__((packed)) {
    uint8_t  linkMajor;
    uint8_t  linkMinor;
} bleCtrlStmHello_t;

typedef struct __attribute__((packed)) {
    uint8_t  session;          /* bleUiSession_e                                */
    uint8_t  pairWindow;       /* 1 = pairing window open                       */
} bleCtrlUiState_t;

typedef struct __attribute__((packed)) {
    uint8_t  kind;             /* bleCue_e                                      */
} bleCtrlCue_t;

typedef struct __attribute__((packed)) {
    uint8_t  reason;           /* HCI reason to disconnect with (0x13 default)  */
} bleCtrlKick_t;

typedef struct __attribute__((packed)) {
    uint8_t  on;               /* 0 = stop advertising (and kick), 1 = advertise */
} bleCtrlAdvertise_t;

_Static_assert(sizeof(bleCtrlNrfHello_t)  == 18, "bleCtrlNrfHello_t");
_Static_assert(sizeof(bleCtrlLinkUp_t)    == 12, "bleCtrlLinkUp_t");
_Static_assert(sizeof(bleCtrlLinkDown_t)  ==  1, "bleCtrlLinkDown_t");
_Static_assert(sizeof(bleCtrlButton_t)    ==  1, "bleCtrlButton_t");
_Static_assert(sizeof(bleCtrlCredit_t)    ==  1, "bleCtrlCredit_t");
_Static_assert(sizeof(bleCtrlStats_t)     ==  8, "bleCtrlStats_t");
_Static_assert(sizeof(bleCtrlStmHello_t)  ==  2, "bleCtrlStmHello_t");
_Static_assert(sizeof(bleCtrlUiState_t)   ==  2, "bleCtrlUiState_t");
_Static_assert(sizeof(bleCtrlCue_t)       ==  1, "bleCtrlCue_t");
_Static_assert(sizeof(bleCtrlKick_t)      ==  1, "bleCtrlKick_t");
_Static_assert(sizeof(bleCtrlAdvertise_t) ==  1, "bleCtrlAdvertise_t");
