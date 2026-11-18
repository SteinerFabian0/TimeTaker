/*
 * Stream budget. See stream_budget.h.
 */

#include "stream_budget.h"

#include <stdbool.h>
#include <stdint.h>

void streamBudgetInit(streamBudget_t *b, uint32_t bytesPerSec, uint32_t burst, uint32_t nowMs)
{
    b->bytesPerSec = bytesPerSec;
    b->burst       = burst;
    b->tokens      = burst;
    b->lastMs      = nowMs;
    b->dropped     = 0U;
}

void streamBudgetSetRate(streamBudget_t *b, uint32_t bytesPerSec, uint32_t burst)
{
    b->bytesPerSec = bytesPerSec;
    b->burst       = burst;
    if (b->tokens > burst) {
        b->tokens = burst;
    }
}

bool streamBudgetTake(streamBudget_t *b, uint32_t nowMs, uint32_t bytes)
{
    if (b->bytesPerSec == 0U) {
        return true;
    }

    const uint32_t elapsed = nowMs - b->lastMs;
    if (elapsed > 0U) {
        /* Whole seconds and the rest apart, so nothing overflows u32 for any
         * rate below 4 MB/s, and no 64-bit division is needed. */
        const uint32_t ms    = (elapsed > 10000U) ? 10000U : elapsed;
        const uint32_t add   = (ms / 1000U) * b->bytesPerSec + ((ms % 1000U) * b->bytesPerSec) / 1000U;
        const uint32_t space = b->burst - b->tokens;
        b->tokens += (add > space) ? space : add;
        b->lastMs  = nowMs;
    }

    if (bytes > b->tokens) {
        b->dropped++;
        return false;
    }
    b->tokens -= bytes;
    return true;
}

uint32_t streamBudgetBleRate(uint16_t attMtu, uint16_t intervalUnits)
{
    if (attMtu <= 3U || intervalUnits == 0U) {
        return STREAM_BUDGET_BLE_MIN;
    }
    /* bytes/s = (mtu - 3) * 2 per interval / (interval * 1.25 ms) / 2
     *         = (mtu - 3) * 800 / intervalUnits */
    uint32_t rate = ((uint32_t)attMtu - 3U) * 800U / intervalUnits;
    if (rate < STREAM_BUDGET_BLE_MIN) {
        rate = STREAM_BUDGET_BLE_MIN;
    }
    if (rate > STREAM_BUDGET_BLE_MAX) {
        rate = STREAM_BUDGET_BLE_MAX;
    }
    return rate;
}
