/*
 * Stream budget: a token bucket in bytes per second, one per transport
 * (CONTROL_FIRMWARE_GUIDE §5.2). USB takes all (rate 0 = unlimited); BLE gets
 * a cap set from the link parameters nrf_link reports. A stream chunk that
 * finds no budget is dropped, like one that finds no transport room: streams
 * are best effort and never delay an event.
 *
 * Pure logic: no HAL. Host-tested (test/host_nrf_link).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t bytesPerSec;   /* 0 = unlimited */
    uint32_t burst;         /* bucket size in bytes */
    uint32_t tokens;
    uint32_t lastMs;
    uint32_t dropped;       /* chunks refused */
} streamBudget_t;

void streamBudgetInit(streamBudget_t *b, uint32_t bytesPerSec, uint32_t burst, uint32_t nowMs);

/* Changes the rate (the bucket keeps what it has, capped at the new burst). */
void streamBudgetSetRate(streamBudget_t *b, uint32_t bytesPerSec, uint32_t burst);

/* Takes bytes from the bucket if they are all there. */
bool streamBudgetTake(streamBudget_t *b, uint32_t nowMs, uint32_t bytes);

/*
 * The BLE cap from the link parameters: (attMtu - 3) bytes per notification,
 * about two notifications per connection interval, then half of that as
 * headroom for the event stream, clamped to [STREAM_BUDGET_BLE_MIN,
 * STREAM_BUDGET_BLE_MAX]. intervalUnits is in 1.25 ms.
 */
#define STREAM_BUDGET_BLE_MIN  512U
#define STREAM_BUDGET_BLE_MAX  4096U
uint32_t streamBudgetBleRate(uint16_t attMtu, uint16_t intervalUnits);
