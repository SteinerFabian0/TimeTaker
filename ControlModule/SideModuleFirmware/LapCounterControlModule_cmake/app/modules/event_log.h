/*
 * Event log: every host event of this boot, numbered and replayable.
 *
 * Every fact the control module reports goes in here first: a crossing, a lane
 * joining, a button press. It gets the next evSeq (0 = EVT_BOOT, then +1 per
 * event). Each host session keeps its own cursor and sends log[cursor..next)
 * whenever its transport has room. Nothing is ever pushed to a session
 * directly. That is what makes RESUME work: after a dropout, a tab reload, or
 * a switch from USB to BLE, the app names the last evSeq it has and the log
 * replays the rest (SYSTEM_ARCHITECTURE §7.4).
 *
 * Ring of fixed slots. When full, the oldest event is overwritten.
 * eventLogOldest() then moves up, and a RESUME from before it gets
 * ERR_LOG_GAP. The app keeps the long-term history (IndexedDB). This only has
 * to bridge dropouts.
 *
 * A slot stores the event payload WITHOUT its leading evSeq (the slot index
 * implies it), so the largest event, EVT_CROSSING at 24 B, needs 20 B of body.
 *
 * Pure logic: no HAL, no globals; storage is the caller's. Host-tested
 * (test/host_event_log). Not reentrant: append and read from the main loop
 * only.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "host_protocol.h"

#define EVENT_LOG_EVSEQ_BYTES  4U
#define EVENT_LOG_MAX_BODY     (HP_EVT_CROSSING_BYTES - EVENT_LOG_EVSEQ_BYTES)

typedef struct {
    uint8_t msgId;
    uint8_t len;                          /* body bytes in use */
    uint8_t body[EVENT_LOG_MAX_BODY];
} eventLogSlot_t;

typedef struct {
    eventLogSlot_t *slots;
    uint32_t        capacity;
    uint32_t        next;                 /* evSeq the next event gets */
} eventLog_t;

void eventLogInit(eventLog_t *log, eventLogSlot_t *slots, uint32_t capacity);

/*
 * Appends one event. event points at the full protocol struct (for example an
 * hpEvtButton_t); its first EVENT_LOG_EVSEQ_BYTES (the evSeq field) are
 * ignored, because the log assigns the number. Returns CTRL_ERR_INVALID_ARG
 * if size is shorter than the evSeq field or the body exceeds
 * EVENT_LOG_MAX_BODY. Otherwise returns CTRL_OK and the number in *seq (seq
 * may be NULL).
 */
ctrlStatus_e eventLogAppend(eventLog_t *log, uint8_t msgId, const void *event, uint16_t size,
                            uint32_t *seq);

/* Oldest evSeq still held (== eventLogNext() while empty). */
uint32_t eventLogOldest(const eventLog_t *log);

/* evSeq the next event will get; also the count of events since init. */
uint32_t eventLogNext(const eventLog_t *log);

/* The slot of event seq, or NULL if it was overwritten or is not written yet. */
const eventLogSlot_t *eventLogGet(const eventLog_t *log, uint32_t seq);
