/*
 * The CHAIN rule of guide §3.2: when to drive CHAIN_OUT and when to ask the
 * controller for an address (I3C hot-join).
 *
 * Pure logic: no HAL, no globals, host-tested (test/host_chain). lane_chain
 * owns the pins and feeds it the raw CHAIN_IN level from the main loop; it
 * is not ISR-safe.
 *
 * Rules:
 *   - CHAIN_IN is debounced: a level counts once it has held for
 *     CHAIN_DEBOUNCE_MS. Shorter glitches are ignored.
 *   - CHAIN_OUT = (has dynamic address) AND (debounced CHAIN_IN high).
 *   - CHAIN_IN falling (debounced) means the upstream lane reset or was
 *     unplugged: the address is forgotten (CHAIN_OUT drops with it, which
 *     cascades down the chain) and the lane waits for CHAIN_IN to rise.
 *   - Hot-join only while CHAIN_IN is high and the lane has no address:
 *     first at once when CHAIN_IN rises, then retried with exponential
 *     backoff (CHAIN_JOIN_BACKOFF_MIN_MS doubling to ..._MAX_MS) while the
 *     controller does not answer.
 *   - An address lost while CHAIN_IN stays high (RSTDAA, RELEASE_CHAIN) waits
 *     CHAIN_REJOIN_HOLD_MS before the first hot-join. Every lane hears an
 *     RSTDAA at once, and every lane has the same provisioned ID: the hold
 *     lets the CHAIN_OUT drops cascade down the chain first, so only lane 1
 *     (whose CHAIN_IN the controller drives) can join and two identical IDs
 *     never meet in one ENTDAA.
 *   - The I3C target is enabled only while it may take part in ENTDAA or
 *     already has an address: (has address) OR (CHAIN_IN high AND no RSTDAA
 *     hold running). ENTDAA is answered by the peripheral in hardware by every
 *     enabled target without an address, so a lane whose CHAIN_IN is low must
 *     keep its target disabled, or it joins (with the same provisioned ID) the
 *     ENTDAA meant for the lane upstream (lane_protocol.h "Enumeration").
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CHAIN_DEBOUNCE_MS           3U
#define CHAIN_JOIN_BACKOFF_MIN_MS   20U
#define CHAIN_JOIN_BACKOFF_MAX_MS   1000U
/* Six lanes × (debounce + a loop pass), with margin. */
#define CHAIN_REJOIN_HOLD_MS        60U

typedef enum {
    CHAIN_EVENT_NONE = 0,
    CHAIN_EVENT_ROSE,         /* debounced CHAIN_IN went high                  */
    CHAIN_EVENT_FELL,         /* debounced CHAIN_IN went low: address forgotten */
} chainEvent_e;

typedef struct {
    bool     rawLevel;
    uint32_t rawSinceMs;
    bool     stableHigh;
    bool     hasAddress;
    uint32_t nextJoinMs;
    uint32_t backoffMs;
    uint32_t holdUntilMs;     /* end of the RSTDAA / RELEASE_CHAIN hold         */
} chainLogic_t;

/* Starts with CHAIN_IN low and no address. */
void chainLogicInit(chainLogic_t *c, uint32_t nowMs);

/* Feeds the raw CHAIN_IN level; returns the debounced edge, if any. */
chainEvent_e chainLogicUpdate(chainLogic_t *c, bool level, uint32_t nowMs);

/* The controller gave the lane its dynamic address (ENTDAA). */
void chainLogicAddressAssigned(chainLogic_t *c);

/* The address went away with CHAIN_IN still high (RSTDAA, RELEASE_CHAIN). */
void chainLogicAddressLost(chainLogic_t *c, uint32_t nowMs);

/* Level CHAIN_OUT must drive now. */
bool chainLogicChainOut(const chainLogic_t *c);

/* Debounced CHAIN_IN. */
bool chainLogicChainIn(const chainLogic_t *c);

bool chainLogicHasAddress(const chainLogic_t *c);

/* A hot-join request is due now. */
bool chainLogicJoinDue(const chainLogic_t *c, uint32_t nowMs);

/* A hot-join request went out at nowMs: schedules the retry. */
void chainLogicJoinSent(chainLogic_t *c, uint32_t nowMs);

/* The I3C target must be enabled now (the ENTDAA rule above). Whenever a
 * join is due this is true as well. */
bool chainLogicTargetEnabled(const chainLogic_t *c, uint32_t nowMs);
