/*
 * Host manager: the control module's side of the host protocol.
 *
 * Owns the event log and one host session per transport: USB CDC (TRUSTED:
 * a cable is physical access) and BLE through the nRF bridge (bound or not,
 * per the bind list). It owns the protocol, not the behaviour:
 *
 *   - session requests (HELLO/PING/RESUME) are answered by host_session;
 *   - every other request is decoded and length-checked here and handed to
 *     the coordinator that owns it (LANE_* -> lane_manager, START_SEQUENCE ->
 *     sequence_manager, SETTINGS_* -> settings_store). LANE_STATUS needs a
 *     bus round trip: its answer is deferred until lane_manager has it;
 *   - facts go the other way: any coordinator calls hostManagerPublishEvent(),
 *     and the sessions pump the log out at their own pace. Coordinators never
 *     send frames. Streams (STR_RAW_SAMPLES, DBG_LOG) go to every open
 *     session that has room and stream budget (USB: all, BLE: a cap from the
 *     link parameters), and are dropped otherwise.
 *
 * The BLE bind policy lives here (CONTROL_FIRMWARE_GUIDE §5.4,
 * SYSTEM_ARCHITECTURE §9.5): the list (bind_list, persisted by
 * settings_store), the pairing window and its timer, the 60 s grace of an
 * unbound session, and what the nRF shows (UI_STATE, CUE, KICK). The nRF
 * only reports buttons and renders.
 *
 *   BLE HELLO, listed              BOUND, LRU touched, UI_STATE{bound}
 *   BLE HELLO, window open         added, BOUND, CUE{BIND_ACCEPTED}
 *   BLE HELLO, window closed       UNBOUND, 60 s grace, UI_STATE{unbound}
 *   window opens, UNBOUND open     bound live, as above
 *   60 s UNBOUND                   KICK, EVT_BIND_CHANGED{REJECTED}
 *   nRF BUTTON LONG                window open 60 s, UI_STATE{pairing}
 *   nRF BUTTON VERY_LONG           list cleared, window closed,
 *                                  CUE{BINDS_CLEARED}, BLE session UNBOUND
 *   BIND_ALLOW{windowS}            open (0: close) the window
 *   USB HELLO from a web app       its clientId is added (plug in once, BLE
 *                                  works later)
 * Every change is published as EVT_BIND_CHANGED; every nRF button press as
 * EVT_BUTTON{BLE_BIND}.
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
#include "log_line.h"

/* About 24 KB of RAM: roughly 1000 crossings, many minutes of racing. The log
 * only bridges dropouts and transport switches; the app keeps the history. */
#define HOST_MANAGER_EVENT_LOG_CAPACITY  1024U

#define HOST_MANAGER_BIND_WINDOW_S       60U     /* BLE button LONG */
#define HOST_MANAGER_BIND_WINDOW_MAX_S   600U    /* BIND_ALLOW is clamped to this */
#define HOST_MANAGER_UNBOUND_GRACE_MS    60000U  /* then KICK */

ctrlStatus_e hostManagerInit(void);

ctrlStatus_e hostManagerRun(void);

/*
 * Logs an event for every session. event is the full protocol struct (for
 * example an hpEvtButton_t); its evSeq field is ignored and assigned here.
 */
ctrlStatus_e hostManagerPublishEvent(uint8_t msgId, const void *event, uint16_t size);

/* Best-effort stream frame to every session with room and budget. */
void hostManagerPublishStream(uint8_t msgId, const void *payload, uint16_t len);

/* A DBG_LOG line (hpLogLevel_e) to every session. Compiled out without
 * CTRL_ENABLE_DBG_LOG. */
void hostManagerLog(uint8_t level, const logLine_t *line);

/* Whether any host has an open session (for the status LED). */
bool hostManagerAnySessionOpen(void);

/* Whether a host is physically there (USB enumerated or a BLE central), session or not. */
bool hostManagerAnyLinkUp(void);

/* Frames in + events out across all sessions: changes on every bit of traffic
 * (the activity LED watches it). */
uint32_t hostManagerTrafficCount(void);

/* hpBleLinkState_e for STATUS. */
uint8_t hostManagerBleLinkState(void);
