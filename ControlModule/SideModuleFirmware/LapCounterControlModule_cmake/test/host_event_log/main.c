/*
 * Host test for event_log.c: numbering, ring wrap, the oldest/next window and
 * the size rules. One broken rule per rejection case.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "event_log.h"

static int failures;

static void check(const char *name, int passed)
{
    printf("%-60s %s\n", name, passed ? "ok" : "FAIL");
    failures += passed ? 0 : 1;
}

#define CAP 4U

static eventLogSlot_t slots[CAP];
static eventLog_t     elog;

static hpEvtButton_t button(uint8_t id)
{
    hpEvtButton_t e = { .evSeq = 0xDEADBEEFU, .button = id, .press = 1U, .sysUs = 1000U * id };
    return e;
}

static void testNumbering(void)
{
    eventLogInit(&elog, slots, CAP);
    check("empty: oldest == next == 0", eventLogOldest(&elog) == 0U && eventLogNext(&elog) == 0U);
    check("empty: get(0) is NULL", eventLogGet(&elog, 0U) == NULL);

    uint32_t seq = 99U;
    hpEvtButton_t e = button(2);
    check("append: OK, first evSeq is 0",
          eventLogAppend(&elog, HP_MSG_EVT_BUTTON, &e, sizeof(e), &seq) == CTRL_OK && seq == 0U);

    const eventLogSlot_t *s = eventLogGet(&elog, 0U);
    check("get: msgId and length kept, evSeq field stripped",
          s != NULL && s->msgId == HP_MSG_EVT_BUTTON && s->len == sizeof(e) - EVENT_LOG_EVSEQ_BYTES
          && memcmp(s->body, (const uint8_t *)&e + EVENT_LOG_EVSEQ_BYTES, s->len) == 0);
    check("append: NULL seq pointer is fine",
          eventLogAppend(&elog, HP_MSG_EVT_BUTTON, &e, sizeof(e), NULL) == CTRL_OK && eventLogNext(&elog) == 2U);
}

static void testWrap(void)
{
    eventLogInit(&elog, slots, CAP);
    for (uint8_t i = 0U; i < 10U; i++) {
        hpEvtButton_t e = button(i);
        (void)eventLogAppend(&elog, HP_MSG_EVT_BUTTON, &e, sizeof(e), NULL);
    }
    check("wrap: next counts every event", eventLogNext(&elog) == 10U);
    check("wrap: oldest = next - capacity", eventLogOldest(&elog) == 6U);
    check("wrap: overwritten event is gone", eventLogGet(&elog, 5U) == NULL);
    check("wrap: future event is not there", eventLogGet(&elog, 10U) == NULL);

    bool ok = true;
    for (uint32_t seq = 6U; seq < 10U; seq++) {
        const eventLogSlot_t *s = eventLogGet(&elog, seq);
        hpEvtButton_t e = button((uint8_t)seq);
        ok = ok && s != NULL && memcmp(s->body, (const uint8_t *)&e + EVENT_LOG_EVSEQ_BYTES, s->len) == 0;
    }
    check("wrap: every held event reads back intact", ok);
}

static void testSizes(void)
{
    hpEvtCrossing_t big = { .lane = 6U, .tEntryUs = 0x0102030405060708ULL };
    uint8_t oversize[HP_EVT_CROSSING_BYTES + 1U] = { 0 };
    uint8_t tooShort[EVENT_LOG_EVSEQ_BYTES - 1U] = { 0 };
    uint8_t seqOnly[EVENT_LOG_EVSEQ_BYTES]       = { 0 };

    eventLogInit(&elog, slots, CAP);
    check("size: the largest event (EVT_CROSSING) fits",
          eventLogAppend(&elog, HP_MSG_EVT_CROSSING, &big, sizeof(big), NULL) == CTRL_OK
          && eventLogGet(&elog, 0U)->len == EVENT_LOG_MAX_BODY);
    check("reject: one byte over the largest event",
          eventLogAppend(&elog, HP_MSG_EVT_CROSSING, oversize, sizeof(oversize), NULL) == CTRL_ERR_INVALID_ARG);
    check("reject: shorter than the evSeq field",
          eventLogAppend(&elog, HP_MSG_EVT_BOOT, tooShort, sizeof(tooShort), NULL) == CTRL_ERR_INVALID_ARG);
    check("accept: an event that is only its evSeq (empty body)",
          eventLogAppend(&elog, HP_MSG_EVT_BOOT, seqOnly, sizeof(seqOnly), NULL) == CTRL_OK
          && eventLogGet(&elog, 1U)->len == 0U);
    check("reject: NULL event",
          eventLogAppend(&elog, HP_MSG_EVT_BOOT, NULL, 8U, NULL) == CTRL_ERR_INVALID_ARG);
    check("rejections consumed no numbers", eventLogNext(&elog) == 2U);

    eventLog_t empty;
    eventLogInit(&empty, slots, 0U);
    check("reject: zero-capacity log", eventLogAppend(&empty, HP_MSG_EVT_BOOT, seqOnly, 4U, NULL) == CTRL_ERR_INVALID_ARG);
}

int main(void)
{
    testNumbering();
    testWrap();
    testSizes();

    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
