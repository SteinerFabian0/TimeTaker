/*
 * CHAIN rule. See chain_logic.h.
 */

#include "chain_logic.h"

#include <stdbool.h>
#include <stdint.h>

void chainLogicInit(chainLogic_t *c, uint32_t nowMs)
{
    c->rawLevel   = false;
    c->rawSinceMs = nowMs;
    c->stableHigh = false;
    c->hasAddress = false;
    c->nextJoinMs = nowMs;
    c->backoffMs  = CHAIN_JOIN_BACKOFF_MIN_MS;
    c->holdUntilMs = nowMs;
}

chainEvent_e chainLogicUpdate(chainLogic_t *c, bool level, uint32_t nowMs)
{
    if (level != c->rawLevel) {
        c->rawLevel   = level;
        c->rawSinceMs = nowMs;
        return CHAIN_EVENT_NONE;
    }

    if (level == c->stableHigh || (uint32_t)(nowMs - c->rawSinceMs) < CHAIN_DEBOUNCE_MS) {
        return CHAIN_EVENT_NONE;
    }

    c->stableHigh = level;
    c->backoffMs  = CHAIN_JOIN_BACKOFF_MIN_MS;

    if (level) {
        c->holdUntilMs = nowMs;   /* a fresh rise: the upstream is addressed */
        c->nextJoinMs = nowMs;
        return CHAIN_EVENT_ROSE;
    }

    c->hasAddress = false;
    return CHAIN_EVENT_FELL;
}

void chainLogicAddressAssigned(chainLogic_t *c)
{
    c->hasAddress = true;
    c->backoffMs  = CHAIN_JOIN_BACKOFF_MIN_MS;
}

void chainLogicAddressLost(chainLogic_t *c, uint32_t nowMs)
{
    /* The backoff is at its minimum already: the assignment reset it. */
    c->hasAddress = false;
    c->nextJoinMs = nowMs + CHAIN_REJOIN_HOLD_MS;
    c->holdUntilMs = c->nextJoinMs;
}

bool chainLogicChainOut(const chainLogic_t *c)
{
    return c->hasAddress && c->stableHigh;
}

bool chainLogicChainIn(const chainLogic_t *c)
{
    return c->stableHigh;
}

bool chainLogicHasAddress(const chainLogic_t *c)
{
    return c->hasAddress;
}

bool chainLogicJoinDue(const chainLogic_t *c, uint32_t nowMs)
{
    return c->stableHigh && !c->hasAddress && (int32_t)(nowMs - c->nextJoinMs) >= 0;
}

void chainLogicJoinSent(chainLogic_t *c, uint32_t nowMs)
{
    c->nextJoinMs = nowMs + c->backoffMs;
    c->backoffMs *= 2U;
    if (c->backoffMs > CHAIN_JOIN_BACKOFF_MAX_MS) {
        c->backoffMs = CHAIN_JOIN_BACKOFF_MAX_MS;
    }
}

bool chainLogicTargetEnabled(const chainLogic_t *c, uint32_t nowMs)
{
    return c->hasAddress || (c->stableHigh && (int32_t)(nowMs - c->holdUntilMs) >= 0);
}
