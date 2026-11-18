/*
 * Log line builder for DBG_LOG streams: text, decimals and hex appended into
 * a fixed buffer, never past its end. No printf: newlib-nano's costs a few KB
 * of flash and the bootloader shares this budget.
 *
 * Mnemonics stay stable (like the hub's log vocabulary): the first word names
 * the subsystem ("LANE", "I3C", "BLE", "BIND", "SEQ", "SET", "FAULT", "BOOT").
 *
 * Pure logic: no HAL, no globals.
 */

#pragma once

#include <stdint.h>

#define LOG_LINE_MAX  96U

typedef struct {
    char     text[LOG_LINE_MAX];
    uint16_t len;
} logLine_t;

void logLineInit(logLine_t *l, const char *head);
void logLineStr(logLine_t *l, const char *s);
void logLineU32(logLine_t *l, uint32_t v);
void logLineI32(logLine_t *l, int32_t v);
void logLineHex(logLine_t *l, uint32_t v, uint8_t digits);
/* " key=value" (decimal / hex). */
void logLineKv(logLine_t *l, const char *key, int32_t v);
void logLineKx(logLine_t *l, const char *key, uint32_t v, uint8_t digits);
