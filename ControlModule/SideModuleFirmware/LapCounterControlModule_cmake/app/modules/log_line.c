/*
 * Log line builder. See log_line.h.
 */

#include "log_line.h"

#include <stdint.h>

static void logLinePut(logLine_t *l, char c)
{
    if (l->len < LOG_LINE_MAX) {
        l->text[l->len++] = c;
    }
}

void logLineInit(logLine_t *l, const char *head)
{
    l->len = 0U;
    logLineStr(l, head);
}

void logLineStr(logLine_t *l, const char *s)
{
    while (*s != '\0') {
        logLinePut(l, *s++);
    }
}

void logLineU32(logLine_t *l, uint32_t v)
{
    char    tmp[10];
    uint8_t n = 0U;
    do {
        tmp[n++] = (char)('0' + (v % 10U));
        v /= 10U;
    } while (v != 0U && n < sizeof(tmp));
    while (n > 0U) {
        logLinePut(l, tmp[--n]);
    }
}

void logLineI32(logLine_t *l, int32_t v)
{
    if (v < 0) {
        logLinePut(l, '-');
        logLineU32(l, (uint32_t)(-(v + 1)) + 1U);
    } else {
        logLineU32(l, (uint32_t)v);
    }
}

void logLineHex(logLine_t *l, uint32_t v, uint8_t digits)
{
    static const char hex[] = "0123456789abcdef";
    for (int8_t i = (int8_t)(digits - 1U); i >= 0; i--) {
        logLinePut(l, hex[(v >> (4U * (uint8_t)i)) & 0xFU]);
    }
}

void logLineKv(logLine_t *l, const char *key, int32_t v)
{
    logLinePut(l, ' ');
    logLineStr(l, key);
    logLinePut(l, '=');
    logLineI32(l, v);
}

void logLineKx(logLine_t *l, const char *key, uint32_t v, uint8_t digits)
{
    logLinePut(l, ' ');
    logLineStr(l, key);
    logLineStr(l, "=0x");
    logLineHex(l, v, digits);
}
