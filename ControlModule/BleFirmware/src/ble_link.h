/*
 * The BLE side: advertising, the Nordic UART Service, connection callbacks,
 * notifications with a completion callback. BLE_FIRMWARE_GUIDE.md §6.
 *
 * Bluetooth callbacks (BT RX thread, system work queue) only record what
 * happened and call wake(); the main loop reads it with bleLinkTakeEvents()
 * and acts. The one exception is the NUS write: it goes straight to the
 * write handler given at init (it encodes into the UART TX ring and never
 * blocks).
 *
 * Notifications: bleLinkNotify() tags each one with a slot index and the
 * connection generation. Its completion is counted per slot and read with
 * bleLinkTakeCompletions(). A disconnect bumps the generation, so a late
 * completion from an old connection is never credited to a new one.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ble_link_protocol.h"
#include "data_slots.h"

/* Events for the main loop (bit mask). */
#define BLE_EVT_CONNECTED       (1U << 0)
#define BLE_EVT_DISCONNECTED    (1U << 1)
#define BLE_EVT_PARAMS          (1U << 2)   /* MTU, PHY, data length, conn params */
#define BLE_EVT_NOTIFY_ON       (1U << 3)
#define BLE_EVT_NOTIFY_OFF      (1U << 4)
#define BLE_EVT_RECYCLED        (1U << 5)   /* connection object free: advertise again */
#define BLE_EVT_SENT            (1U << 6)   /* notification completions to collect */

typedef void (*bleWriteHandler_t)(const uint8_t *data, uint16_t len);

int bleLinkInit(void (*wake)(void), bleWriteHandler_t onWrite);

uint32_t bleLinkTakeEvents(void);

/* HCI reason of the last disconnect. */
uint8_t bleLinkLastDisconnectReason(void);

bool bleLinkConnected(void);
bool bleLinkNotifyEnabled(void);

/* Advertising (guide §6.1). bleLinkAdvertiseStart() is a no-op returning 0
 * while connected or already advertising; -ENOMEM etc. means "try again
 * later" (e.g. the connection object is not recycled yet). */
int  bleLinkAdvertiseStart(void);
int  bleLinkAdvertiseStop(void);
bool bleLinkAdvertising(void);

/* Disconnects the central with the given HCI reason. */
int bleLinkKick(uint8_t hciReason);

/* Largest notification payload now: ATT MTU - 3, at most BLE_LINK_DATA_MAX.
 * 0 if not connected. */
uint16_t bleLinkMaxChunk(void);

/* Notifications the main loop may have in flight at once: one ATT buffer is
 * always left for the host's own responses, so a notify call from the main
 * thread never waits for a buffer. */
uint8_t bleLinkMaxInFlight(void);

/* Sends one notification on the NUS TX characteristic. 0 on success; the
 * completion is then counted for `slot`. */
int bleLinkNotify(const uint8_t *data, uint16_t len, uint8_t slot);

/* Moves the completion counts per slot into counts[SLOTS_MAX] (added) and
 * clears them. */
void bleLinkTakeCompletions(uint8_t counts[SLOTS_MAX]);

/* Starts a new completion generation: every notification queued so far is
 * forgotten, and its late completion callback is ignored. Call when the
 * slots are dropped (their credits returned), so a late callback can never
 * hit a slot that was freed and reused. */
void bleLinkNewSession(void);

/* LINK_UP for the current connection. false if not connected. */
bool bleLinkGetLinkUp(bleCtrlLinkUp_t *out);

/* RSSI of the connection in dBm, 127 if unknown (not connected, or not read
 * yet). Never blocks: returns the last value the system workqueue read and
 * queues a new read, so a STATS value is one second old. */
int8_t bleLinkRssi(void);

/* The identity address, little-endian, as the controller reports it. */
void bleLinkGetAddr(uint8_t out[6]);
