/*
 * Host manager: the control module's side of the host protocol.
 *
 * Owns the event log and one host session per transport. Today that is USB
 * CDC; the BLE session joins it when the nRF bridge exists (§9 of
 * SYSTEM_ARCHITECTURE). It owns the protocol, not the behaviour:
 *
 *   - session requests (HELLO/PING/RESUME) are answered by host_session;
 *   - every other request is decoded here and handed to the coordinator that
 *     owns it (LANE_POWER → lane_manager, …). Replies come straight back;
 *   - facts go the other way: any coordinator calls hostManagerPublishEvent(),
 *     and the sessions pump the log out at their own pace. Coordinators never
 *     send frames.
 *
 * At init it logs EVT_BOOT, so evSeq 0 of every boot says why it booted and
 * names the bootId.
 *
 * Context: main loop only.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ctrl_status.h"

/* About 24 KB of RAM: roughly 1000 crossings, many minutes of racing. The log
 * only bridges dropouts and transport switches; the app keeps the history. */
#define HOST_MANAGER_EVENT_LOG_CAPACITY  1024U

ctrlStatus_e hostManagerInit(void);

ctrlStatus_e hostManagerRun(void);

/*
 * Logs an event for every session. event is the full protocol struct (for
 * example an hpEvtButton_t); its evSeq field is ignored and assigned here.
 */
ctrlStatus_e hostManagerPublishEvent(uint8_t msgId, const void *event, uint16_t size);

/* Whether any host has an open session (for the status LED). */
bool hostManagerAnySessionOpen(void);

/* Whether a host is physically there (USB enumerated), session or not. */
bool hostManagerAnyLinkUp(void);

/* Frames in + events out across all sessions: changes on every bit of traffic
 * (the activity LED watches it). */
uint32_t hostManagerTrafficCount(void);
