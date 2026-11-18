/*
 * Host test for button_classifier.c: BLE_FIRMWARE_GUIDE.md §7.1 as a pure
 * function of sample sequences (one sample = BTN_TICK_MS).
 *
 * Every row of the table at its boundaries (both sides), the dead zone, the
 * stuck button, debounce of glitches on press and release, one event per
 * press exactly at the debounced release, and the hold phases for the LED.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../check.h"
#include "ble_link_protocol.h"
#include "button_classifier.h"

static btnClassifier_t b;
static int             events;
static btnEvent_e      lastEvent;
static uint32_t        lastEventAt;     /* sample index of the last event */
static uint32_t        sampleIndex;

static void reset(void)
{
    btnInit(&b);
    events      = 0;
    lastEvent   = BTN_EVT_NONE;
    lastEventAt = 0;
    sampleIndex = 0;
}

static void feed(bool pressed, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        const btnEvent_e e = btnSample(&b, pressed);
        if (e != BTN_EVT_NONE) {
            events++;
            lastEvent   = e;
            lastEventAt = sampleIndex;
        }
        sampleIndex++;
    }
}

/* Released for a while, pressed for `ticks` samples, released long enough to
 * debounce. Returns the event (NONE if none), and checks there was at most
 * one. */
static btnEvent_e pressFor(uint32_t ticks, bool *single)
{
    reset();
    feed(false, 10);
    feed(true, ticks);
    feed(false, 10);
    *single = (events <= 1);
    return (events == 1) ? lastEvent : BTN_EVT_NONE;
}

static void row(const char *name, uint32_t ticks, btnEvent_e want)
{
    bool single;
    const btnEvent_e got = pressFor(ticks, &single);
    char label[96];
    snprintf(label, sizeof(label), "%s (%u ticks)", name, (unsigned)ticks);
    check(label, single && got == want);
}

int main(void)
{
    /* The event numbers are the protocol's. */
    check("SHORT == BLE_PRESS_SHORT", BTN_EVT_SHORT == (int)BLE_PRESS_SHORT);
    check("LONG == BLE_PRESS_LONG", BTN_EVT_LONG == (int)BLE_PRESS_LONG);
    check("VERY_LONG == BLE_PRESS_VERY_LONG", BTN_EVT_VERY_LONG == (int)BLE_PRESS_VERY_LONG);

    /* §7.1 rows, at both sides of every boundary. */
    row("short: shortest debounced press", BTN_DEBOUNCE_SAMPLES, BTN_EVT_SHORT);
    row("short: 0.5 s", 50, BTN_EVT_SHORT);
    row("short: just under 1 s", 99, BTN_EVT_SHORT);
    row("dead zone: 1 s", 100, BTN_EVT_NONE);
    row("dead zone: 1.5 s", 150, BTN_EVT_NONE);
    row("dead zone: just under 2 s", 199, BTN_EVT_NONE);
    row("long: 2 s", 200, BTN_EVT_LONG);
    row("long: 5 s", 500, BTN_EVT_LONG);
    row("long: just under 10 s", 999, BTN_EVT_LONG);
    row("very long: 10 s", 1000, BTN_EVT_VERY_LONG);
    row("very long: 20 s", 2000, BTN_EVT_VERY_LONG);
    row("very long: just under 30 s", 2999, BTN_EVT_VERY_LONG);
    row("stuck: 30 s", 3000, BTN_EVT_NONE);
    row("stuck: 2 min", 12000, BTN_EVT_NONE);

    /* Debounce: glitches shorter than the debounce count are no press. */
    {
        reset();
        feed(false, 10);
        feed(true, BTN_DEBOUNCE_SAMPLES - 1);
        feed(false, 10);
        feed(true, 1);
        feed(false, 10);
        check("glitch below debounce: no event", events == 0);
        check("glitch below debounce: never held", btnHold(&b) == BTN_HOLD_NONE);
    }
    {
        /* A bouncing press (L H L L L ...) is one press. */
        reset();
        feed(false, 10);
        feed(true, 1);
        feed(false, 1);
        feed(true, 60);
        feed(false, 10);
        check("bounce on press: one SHORT", events == 1 && lastEvent == BTN_EVT_SHORT);
    }
    {
        /* A release glitch inside a long hold does not split it. */
        reset();
        feed(false, 10);
        feed(true, 150);
        feed(false, BTN_DEBOUNCE_SAMPLES - 1);
        feed(true, 150);
        feed(false, 10);
        check("glitch during hold: one LONG, no early event",
              events == 1 && lastEvent == BTN_EVT_LONG);
    }
    {
        /* The event comes at the debounced release: the DEBOUNCE-th
         * released sample, not before. */
        reset();
        feed(false, 10);
        feed(true, 40);
        const uint32_t releaseStart = sampleIndex;
        feed(false, BTN_DEBOUNCE_SAMPLES - 1);
        check("no event before the release is debounced", events == 0);
        feed(false, 1);
        check("event on the debounced release sample",
              events == 1 && lastEventAt == releaseStart + BTN_DEBOUNCE_SAMPLES - 1U);
        feed(false, 500);
        check("exactly one event per press", events == 1);
    }
    {
        /* A button already held at boot counts from the first samples. */
        reset();
        feed(true, 250);
        feed(false, 10);
        check("held at boot: LONG on release", events == 1 && lastEvent == BTN_EVT_LONG);
    }

    /* Hold phases while held (LED feedback). */
    {
        reset();
        feed(false, 10);
        check("hold: released", btnHold(&b) == BTN_HOLD_NONE);
        /* The press is recognised on its DEBOUNCE-th sample; count from
         * there (the release is delayed by the same, so classes match). */
        feed(true, BTN_DEBOUNCE_SAMPLES - 1U);
        check("hold: not yet pressed one sample before debounce", btnHold(&b) == BTN_HOLD_NONE);
        feed(true, 1);
        check("hold: pressed after debounce", btnHold(&b) == BTN_HOLD_SHORT);
        feed(true, 50);
        check("hold: 0.5 s short", btnHold(&b) == BTN_HOLD_SHORT);
        feed(true, 100);                      /* 1.5 s */
        check("hold: 1.5 s dead", btnHold(&b) == BTN_HOLD_DEAD);
        feed(true, 49);                       /* 1.99 s */
        check("hold: just under 2 s still dead", btnHold(&b) == BTN_HOLD_DEAD);
        feed(true, 1);                        /* 2.0 s */
        check("hold: 2 s long", btnHold(&b) == BTN_HOLD_LONG);
        feed(true, 799);                      /* 9.99 s */
        check("hold: just under 10 s long", btnHold(&b) == BTN_HOLD_LONG);
        feed(true, 1);
        check("hold: 10 s very long", btnHold(&b) == BTN_HOLD_VERY_LONG);
        feed(true, 1999);
        check("hold: just under 30 s very long", btnHold(&b) == BTN_HOLD_VERY_LONG);
        feed(true, 1);
        check("hold: 30 s stuck", btnHold(&b) == BTN_HOLD_STUCK);
        feed(true, 100000);
        check("hold: stays stuck (counter saturates)", btnHold(&b) == BTN_HOLD_STUCK);
        feed(false, 10);
        check("hold: released after stuck, no event", btnHold(&b) == BTN_HOLD_NONE && events == 0);
    }

    /* The pure class function, at its edges. */
    check("classify 0 -> SHORT", btnClassify(0) == BTN_EVT_SHORT);
    check("classify 99 -> SHORT", btnClassify(99) == BTN_EVT_SHORT);
    check("classify 100 -> NONE", btnClassify(100) == BTN_EVT_NONE);
    check("classify 200 -> LONG", btnClassify(200) == BTN_EVT_LONG);
    check("classify 1000 -> VERY_LONG", btnClassify(1000) == BTN_EVT_VERY_LONG);
    check("classify 3000 -> NONE", btnClassify(3000) == BTN_EVT_NONE);

    return checkSummary("host_button");
}
