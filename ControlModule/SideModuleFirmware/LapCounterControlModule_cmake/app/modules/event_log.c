/*
 * Event log. See event_log.h.
 */

#include "event_log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

void eventLogInit(eventLog_t *log, eventLogSlot_t *slots, uint32_t capacity)
{
    log->slots    = slots;
    log->capacity = capacity;
    log->next     = 0U;
}

ctrlStatus_e eventLogAppend(eventLog_t *log, uint8_t msgId, const void *event, uint16_t size,
                            uint32_t *seq)
{
    if (event == NULL || log->capacity == 0U || size < EVENT_LOG_EVSEQ_BYTES
        || size - EVENT_LOG_EVSEQ_BYTES > EVENT_LOG_MAX_BODY) {
        return CTRL_ERR_INVALID_ARG;
    }

    eventLogSlot_t *slot = &log->slots[log->next % log->capacity];
    slot->msgId = msgId;
    slot->len   = (uint8_t)(size - EVENT_LOG_EVSEQ_BYTES);
    memcpy(slot->body, (const uint8_t *)event + EVENT_LOG_EVSEQ_BYTES, slot->len);

    if (seq != NULL) {
        *seq = log->next;
    }
    log->next++;
    return CTRL_OK;
}

uint32_t eventLogOldest(const eventLog_t *log)
{
    return (log->next > log->capacity) ? log->next - log->capacity : 0U;
}

uint32_t eventLogNext(const eventLog_t *log)
{
    return log->next;
}

const eventLogSlot_t *eventLogGet(const eventLog_t *log, uint32_t seq)
{
    if (seq < eventLogOldest(log) || seq >= log->next) {
        return NULL;
    }
    return &log->slots[seq % log->capacity];
}
