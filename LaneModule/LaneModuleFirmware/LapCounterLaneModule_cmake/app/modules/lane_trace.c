/*
 * SWO/ITM text trace. See lane_trace.h.
 */

#include "lane_trace.h"

#if LANE_ENABLE_SWO_TRACE

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

#include "stm32_hal.h"

#define LANE_TRACE_RING_SIZE  1024U
#define LANE_TRACE_LINE_MAX   96U

volatile uint8_t  laneTraceLevel   = LANE_TRACE_EVENTS;
volatile uint32_t laneTraceDropped;

static char     laneTraceRing[LANE_TRACE_RING_SIZE];
static uint32_t laneTraceHead;   /* next byte out */
static uint32_t laneTraceCount;

static uint32_t laneTraceAppendU32(char *line, uint32_t pos, uint32_t v, uint32_t base)
{
    char     tmp[10];
    uint32_t n = 0U;

    do {
        uint32_t d = v % base;
        tmp[n++] = (char)((d < 10U) ? ('0' + d) : ('a' + d - 10U));
        v /= base;
    } while (v != 0U && n < sizeof(tmp));

    while (n > 0U && pos < LANE_TRACE_LINE_MAX - 1U) {
        line[pos++] = tmp[--n];
    }
    return pos;
}

static void laneTraceCommit(const char *line, uint32_t len)
{
    if (len > LANE_TRACE_RING_SIZE - laneTraceCount) {
        laneTraceDropped++;
        return;
    }
    for (uint32_t i = 0U; i < len; i++) {
        laneTraceRing[(laneTraceHead + laneTraceCount) % LANE_TRACE_RING_SIZE] = line[i];
        laneTraceCount++;
    }
}

void laneTrace(uint8_t level, const char *fmt, ...)
{
    if (laneTraceLevel < level) {
        return;
    }

    char     line[LANE_TRACE_LINE_MAX];
    uint32_t pos = 0U;
    va_list  ap;

    va_start(ap, fmt);
    for (const char *p = fmt; *p != '\0' && pos < LANE_TRACE_LINE_MAX - 1U; p++) {
        if (*p != '%' || p[1] == '\0') {
            line[pos++] = *p;
            continue;
        }
        p++;
        switch (*p) {
        case 'u':
            pos = laneTraceAppendU32(line, pos, va_arg(ap, uint32_t), 10U);
            break;
        case 'x':
            pos = laneTraceAppendU32(line, pos, va_arg(ap, uint32_t), 16U);
            break;
        case 'd': {
            int32_t v = va_arg(ap, int32_t);
            if (v < 0) {
                line[pos++] = '-';
                pos = laneTraceAppendU32(line, pos, (uint32_t)(-(v + 1)) + 1U, 10U);
            } else {
                pos = laneTraceAppendU32(line, pos, (uint32_t)v, 10U);
            }
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            while (s != NULL && *s != '\0' && pos < LANE_TRACE_LINE_MAX - 1U) {
                line[pos++] = *s++;
            }
            break;
        }
        case 'c':
            line[pos++] = (char)va_arg(ap, int);
            break;
        default:
            line[pos++] = *p;
            break;
        }
    }
    va_end(ap);

    line[pos++] = '\n';
    laneTraceCommit(line, pos);
}

void laneTraceU64Line(uint8_t level, const char *label, uint64_t us)
{
    laneTrace(level, "%s %us+%uus", label, (uint32_t)(us / 1000000U), (uint32_t)(us % 1000000U));
}

void laneTraceFlush(void)
{
    bool itmOn = (ITM->TCR & ITM_TCR_ITMENA_Msk) != 0U && (ITM->TER & 1UL) != 0U;

    if (!itmOn) {
        /* No probe listening: nothing will ever drain it. */
        laneTraceHead  = 0U;
        laneTraceCount = 0U;
        return;
    }

    while (laneTraceCount > 0U && ITM->PORT[0].u32 != 0U) {
        ITM->PORT[0].u8 = (uint8_t)laneTraceRing[laneTraceHead];
        laneTraceHead   = (laneTraceHead + 1U) % LANE_TRACE_RING_SIZE;
        laneTraceCount--;
    }
}

#endif /* LANE_ENABLE_SWO_TRACE */
