/*
 * Host test for button_logic.c: debounce, short/long/very-long classification,
 * boot-held buttons, two buttons at once, ms tick wrap.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "button_logic.h"
#include "../test_check.h"

/* Feeds `raw` every `stepMs` for `durMs`, returns the new time. */
static uint32_t feed(buttonLogic_t *l, uint32_t t, bool raw, uint32_t durMs)
{
    for (uint32_t i = 0U; i < durMs; i++) {
        buttonLogicStep(l, raw, t++);
    }
    return t;
}

/* One clean press of holdMs, then 100 ms released. */
static uint32_t press(buttonLogic_t *l, uint32_t t, uint32_t holdMs)
{
    t = feed(l, t, true, holdMs);
    return feed(l, t, false, 100U);
}

static int drain(buttonLogic_t *l)
{
    int n = 0;
    while (buttonLogicTake(l) != BUTTON_PRESS_NONE) {
        n++;
    }
    return n;
}

static void testClassification(void)
{
    buttonLogic_t l;
    uint32_t      t = 1000U;

    buttonLogicInit(&l, false, t);
    t = press(&l, t, 100U);
    check("short press -> SHORT",              buttonLogicTake(&l) == BUTTON_PRESS_SHORT);
    check("a press is read once",              buttonLogicTake(&l) == BUTTON_PRESS_NONE);

    t = press(&l, t, 990U);
    check("just under 1 s -> SHORT",           buttonLogicTake(&l) == BUTTON_PRESS_SHORT);
    t = press(&l, t, 1000U);
    check("exactly 1 s -> LONG",               buttonLogicTake(&l) == BUTTON_PRESS_LONG);
    t = press(&l, t, 4990U);
    check("just under 5 s -> LONG",            buttonLogicTake(&l) == BUTTON_PRESS_LONG);
    t = press(&l, t, 5000U);
    check("exactly 5 s -> VERY_LONG",          buttonLogicTake(&l) == BUTTON_PRESS_VERY_LONG);

    /* One press is one event, however long it is held. */
    t = feed(&l, t, true, 8000U);
    check("nothing is reported while held",    buttonLogicTake(&l) == BUTTON_PRESS_NONE && buttonLogicIsPressed(&l));
    t = feed(&l, t, false, 100U);
    check("long hold: exactly one event",      buttonLogicTake(&l) == BUTTON_PRESS_VERY_LONG && drain(&l) == 0);
    check("released: not pressed",             !buttonLogicIsPressed(&l));

    t = press(&l, t, 5U);
    check("glitch below debounce: nothing",    buttonLogicTake(&l) == BUTTON_PRESS_NONE);
    t = press(&l, t, BUTTON_DEBOUNCE_MS + 5U);
    check("press just over debounce time counts", buttonLogicTake(&l) == BUTTON_PRESS_SHORT);
    (void)t;
}

static void testBounce(void)
{
    buttonLogic_t l;
    uint32_t      t = 0U;

    buttonLogicInit(&l, false, t);
    /* Bouncy press: on/off flutter for 8 ms, then solid. */
    for (int i = 0; i < 4; i++) {
        t = feed(&l, t, true, 1U);
        t = feed(&l, t, false, 1U);
    }
    t = feed(&l, t, true, 300U);
    /* Bouncy release inside a hold must not split the press. */
    for (int i = 0; i < 3; i++) {
        t = feed(&l, t, false, 2U);
        t = feed(&l, t, true, 2U);
    }
    t = feed(&l, t, true, 1500U);
    t = feed(&l, t, false, 3U);
    t = feed(&l, t, true, 2U);
    t = feed(&l, t, false, 200U);
    check("bouncy press and release: one LONG", buttonLogicTake(&l) == BUTTON_PRESS_LONG && drain(&l) == 0);
}

static void testBootHeld(void)
{
    buttonLogic_t l;
    uint32_t      t = 500U;

    buttonLogicInit(&l, true, t);
    t = feed(&l, t, true, 3000U);
    check("held across boot: no press while held", buttonLogicTake(&l) == BUTTON_PRESS_NONE && !buttonLogicIsPressed(&l));
    t = feed(&l, t, false, 100U);
    check("held across boot: release is no press", buttonLogicTake(&l) == BUTTON_PRESS_NONE);
    t = press(&l, t, 100U);
    check("held across boot: next press works",  buttonLogicTake(&l) == BUTTON_PRESS_SHORT);

    buttonLogicInit(&l, true, t);
    t = feed(&l, t, false, 5U);
    t = feed(&l, t, true, 5U);                 /* bounce while releasing */
    t = feed(&l, t, false, 100U);
    check("boot release bounce: still no press", buttonLogicTake(&l) == BUTTON_PRESS_NONE);
    t = press(&l, t, 50U);
    check("after bouncy boot release: works",   buttonLogicTake(&l) == BUTTON_PRESS_SHORT);
}

static void testTwoButtons(void)
{
    buttonLogic_t a, b;
    uint32_t      t = 0U;

    buttonLogicInit(&a, false, t);
    buttonLogicInit(&b, false, t);
    for (uint32_t i = 0U; i < 2500U; i++, t++) {
        buttonLogicStep(&a, i < 100U, t);              /* short */
        buttonLogicStep(&b, i >= 50U && i < 2050U, t); /* 2 s, overlapping */
    }
    check("two at once: first is SHORT",  buttonLogicTake(&a) == BUTTON_PRESS_SHORT && drain(&a) == 0);
    check("two at once: second is LONG",  buttonLogicTake(&b) == BUTTON_PRESS_LONG && drain(&b) == 0);
}

static void testQueue(void)
{
    buttonLogic_t l;
    uint32_t      t = 0U;

    buttonLogicInit(&l, false, t);
    t = press(&l, t, 100U);
    t = press(&l, t, 1200U);
    t = press(&l, t, 100U);
    check("unread presses queue in order",
          buttonLogicTake(&l) == BUTTON_PRESS_SHORT && buttonLogicTake(&l) == BUTTON_PRESS_LONG
          && buttonLogicTake(&l) == BUTTON_PRESS_SHORT && buttonLogicTake(&l) == BUTTON_PRESS_NONE);
    for (int i = 0; i < 10; i++) {
        t = press(&l, t, 100U);
    }
    check("queue overflow is bounded", drain(&l) == (int)BUTTON_QUEUE_DEPTH);
}

static void testWrap(void)
{
    buttonLogic_t l;
    uint32_t      t = 0xFFFFFFF0U;

    buttonLogicInit(&l, false, t);
    t = press(&l, t, 1500U);                   /* crosses the 32-bit wrap */
    check("press across tick wrap: LONG",      buttonLogicTake(&l) == BUTTON_PRESS_LONG);
    t = 0xFFFFFF00U;
    buttonLogicInit(&l, true, t);
    t = feed(&l, t, true, 400U);
    t = feed(&l, t, false, 100U);
    t = press(&l, t, 80U);
    check("boot hold across wrap, then SHORT", buttonLogicTake(&l) == BUTTON_PRESS_SHORT && drain(&l) == 0);
}

static void testSparsePolling(void)
{
    /* A slow loop (10 ms per pass) must classify the same. */
    buttonLogic_t l;
    uint32_t      t = 0U;

    buttonLogicInit(&l, false, t);
    for (; t < 1100U; t += 10U) {
        buttonLogicStep(&l, true, t);
    }
    for (; t < 1300U; t += 10U) {
        buttonLogicStep(&l, false, t);
    }
    check("10 ms poll: 1.1 s hold is LONG", buttonLogicTake(&l) == BUTTON_PRESS_LONG);
}

int main(void)
{
    testClassification();
    testBounce();
    testBootHeld();
    testTwoButtons();
    testQueue();
    testWrap();
    testSparsePolling();
    return finish();
}
