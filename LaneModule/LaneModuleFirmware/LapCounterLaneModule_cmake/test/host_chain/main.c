/*
 * Host test for chain_logic.c: CHAIN_OUT = has address AND CHAIN_IN high,
 * CHAIN_IN debounce, forgetting the address when CHAIN_IN falls, and the
 * hot-join schedule (at once on a rise, backoff while unanswered, a hold
 * after an RSTDAA so identical IDs never join together).
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "chain_logic.h"

static int failures;

static void check(const char *name, int passed)
{
    printf("%-58s %s\n", name, passed ? "ok" : "FAIL");
    failures += passed ? 0 : 1;
}

static chainLogic_t c;
static uint32_t     now;

/* Holds `level` for ms milliseconds, one update per ms; returns the last
 * non-NONE event. */
static chainEvent_e hold(bool level, uint32_t ms)
{
    chainEvent_e last = CHAIN_EVENT_NONE;
    for (uint32_t i = 0U; i <= ms; i++) {
        chainEvent_e e = chainLogicUpdate(&c, level, now);
        if (e != CHAIN_EVENT_NONE) {
            last = e;
        }
        if (i < ms) {
            now++;
        }
    }
    return last;
}

static void testDebounce(void)
{
    now = 1000U;
    chainLogicInit(&c, now);
    check("init: CHAIN_OUT low, no join", !chainLogicChainOut(&c) && !chainLogicJoinDue(&c, now));

    check("debounce: 2 ms high is a glitch", hold(true, CHAIN_DEBOUNCE_MS - 1U) == CHAIN_EVENT_NONE);
    (void)hold(false, 5U);
    check("debounce: glitch left CHAIN_IN low", !chainLogicChainIn(&c));

    check("debounce: 3 ms high rises", hold(true, CHAIN_DEBOUNCE_MS) == CHAIN_EVENT_ROSE);
    check("rise: CHAIN_IN high", chainLogicChainIn(&c));
    check("rise: no address yet, CHAIN_OUT stays low", !chainLogicChainOut(&c));
    check("rise: hot-join due at once", chainLogicJoinDue(&c, now));

    check("debounce: a 2 ms low glitch does not fall", hold(false, CHAIN_DEBOUNCE_MS - 1U) == CHAIN_EVENT_NONE);
    (void)hold(true, 5U);
    check("debounce: still high after the glitch", chainLogicChainIn(&c));
}

static void testChainOutRule(void)
{
    now = 0U;
    chainLogicInit(&c, now);

    chainLogicAddressAssigned(&c);
    check("rule: address but CHAIN_IN low -> CHAIN_OUT low", !chainLogicChainOut(&c));

    (void)hold(true, 10U);
    check("rule: CHAIN_IN rising forgets nothing assigned while low",
          chainLogicHasAddress(&c) && chainLogicChainOut(&c));

    check("fall: CHAIN_IN low for the debounce", hold(false, 10U) == CHAIN_EVENT_FELL);
    check("fall: address forgotten", !chainLogicHasAddress(&c));
    check("fall: CHAIN_OUT low (cascades down)", !chainLogicChainOut(&c));
    check("fall: no hot-join while CHAIN_IN is low", !chainLogicJoinDue(&c, now + 10000U));

    (void)hold(true, 10U);
    check("re-rise: join due", chainLogicJoinDue(&c, now));
    chainLogicJoinSent(&c, now);
    check("re-rise: CHAIN_OUT low until addressed", !chainLogicChainOut(&c));
    chainLogicAddressAssigned(&c);
    check("addressed + CHAIN_IN high -> CHAIN_OUT high", chainLogicChainOut(&c));
    check("addressed: no more joins", !chainLogicJoinDue(&c, now + 100000U));
}

static void testBackoff(void)
{
    now = 0U;
    chainLogicInit(&c, now);
    (void)hold(true, 10U);

    uint32_t gaps[8];
    uint32_t last = now;
    for (uint32_t i = 0U; i < 8U; i++) {
        while (!chainLogicJoinDue(&c, now)) {
            now++;
        }
        gaps[i] = now - last;
        last    = now;
        chainLogicJoinSent(&c, now);
    }
    check("backoff: first retry after the minimum", gaps[1] == CHAIN_JOIN_BACKOFF_MIN_MS);
    check("backoff: doubles", gaps[2] == 2U * CHAIN_JOIN_BACKOFF_MIN_MS && gaps[3] == 4U * CHAIN_JOIN_BACKOFF_MIN_MS);
    check("backoff: capped at the maximum", gaps[7] == CHAIN_JOIN_BACKOFF_MAX_MS);

    chainLogicAddressAssigned(&c);
    chainLogicAddressLost(&c, now);
    check("RSTDAA: no join during the hold", !chainLogicJoinDue(&c, now + CHAIN_REJOIN_HOLD_MS - 1U));
    check("RSTDAA: join after the hold", chainLogicJoinDue(&c, now + CHAIN_REJOIN_HOLD_MS));
    chainLogicJoinSent(&c, now + CHAIN_REJOIN_HOLD_MS);
    check("RSTDAA: backoff restarted at the minimum",
          !chainLogicJoinDue(&c, now + CHAIN_REJOIN_HOLD_MS + CHAIN_JOIN_BACKOFF_MIN_MS - 1U)
          && chainLogicJoinDue(&c, now + CHAIN_REJOIN_HOLD_MS + CHAIN_JOIN_BACKOFF_MIN_MS));
}

static void testRiseJoinsAtOnce(void)
{
    /* A long backoff pending from before must not delay the join after the
     * chain was re-plugged. */
    now = 0U;
    chainLogicInit(&c, now);
    (void)hold(true, 10U);
    for (uint32_t i = 0U; i < 6U; i++) {
        chainLogicJoinSent(&c, now);
    }
    (void)hold(false, 10U);
    (void)hold(true, CHAIN_DEBOUNCE_MS);
    check("re-plug: join due at once despite the old backoff", chainLogicJoinDue(&c, now));
}

/* ENTDAA is answered in hardware by every enabled target without an
 * address: the target may only be enabled when this lane is the one meant. */
static void testTargetEnable(void)
{
    now = 0U;
    chainLogicInit(&c, now);
    check("target: disabled at boot (CHAIN_IN low)", !chainLogicTargetEnabled(&c, now));
    (void)hold(true, CHAIN_DEBOUNCE_MS - 1U);
    check("target: disabled while CHAIN_IN high is not debounced", !chainLogicTargetEnabled(&c, now));
    (void)hold(true, 5U);
    check("target: enabled once CHAIN_IN is high and a join is due",
          chainLogicJoinDue(&c, now) && chainLogicTargetEnabled(&c, now));

    chainLogicJoinSent(&c, now);
    check("target: stays enabled between join retries", chainLogicTargetEnabled(&c, now + 1U));

    chainLogicAddressAssigned(&c);
    chainLogicAddressLost(&c, now);
    check("target: disabled during the RSTDAA hold", !chainLogicTargetEnabled(&c, now + CHAIN_REJOIN_HOLD_MS - 1U));
    check("target: enabled after the hold with CHAIN_IN high", chainLogicTargetEnabled(&c, now + CHAIN_REJOIN_HOLD_MS));

    chainLogicAddressAssigned(&c);
    check("target: enabled with an address", chainLogicTargetEnabled(&c, now));
    (void)hold(false, 10U);
    check("target: disabled once CHAIN_IN fell (address forgotten)", !chainLogicTargetEnabled(&c, now + 100000U));

    /* RSTDAA, then the upstream re-addressed: the rise ends the hold. */
    (void)hold(true, 10U);
    chainLogicAddressAssigned(&c);
    chainLogicAddressLost(&c, now);
    (void)hold(false, 10U);
    (void)hold(true, CHAIN_DEBOUNCE_MS);
    check("target: a fresh rise ends the hold", chainLogicTargetEnabled(&c, now) && chainLogicJoinDue(&c, now));
}

static void testTickWrap(void)
{
    now = 0xFFFFFFF0U;
    chainLogicInit(&c, now);
    (void)hold(true, 40U);
    check("wrap: rise across the ms tick wrap", chainLogicChainIn(&c));
    chainLogicJoinSent(&c, now);
    check("wrap: retry timing survives the wrap",
          !chainLogicJoinDue(&c, now + CHAIN_JOIN_BACKOFF_MIN_MS - 1U)
          && chainLogicJoinDue(&c, now + CHAIN_JOIN_BACKOFF_MIN_MS));
}

int main(void)
{
    testDebounce();
    testChainOutRule();
    testBackoff();
    testRiseJoinsAtOnce();
    testTickWrap();
    testTargetEnable();

    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
