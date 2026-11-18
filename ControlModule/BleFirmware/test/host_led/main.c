/*
 * Host test for led_table.c: BLE_FIRMWARE_GUIDE.md §7.2.
 *
 * Every row of the selection table, and first-match-wins between each pair
 * of neighbouring rows (each case is valid for both rows and breaks only the
 * order). The rendered timing of every pattern (period, on-time, number of
 * flashes), cue playback over a base pattern, and phase restart.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../check.h"
#include "ble_link_protocol.h"
#include "led_table.h"

#define MS(t) ((t) / LED_TICK_MS)

static ledInputs_t base(void)
{
    return (ledInputs_t){ .cue = LED_CUE_NONE, .hold = LED_HOLD_NONE, .advertiseOff = false,
                          .pairWindow = false, .connected = false, .session = BLE_UI_SESSION_NONE };
}

/* Rising edges (flashes) and total on-ticks over [0, ticks). */
static void scan(ledPattern_e p, uint32_t ticks, int *flashes, uint32_t *onTicks)
{
    bool prev = false;
    *flashes = 0;
    *onTicks = 0;
    for (uint32_t t = 0; t < ticks; t++) {
        const bool on = ledLevel(p, t);
        if (on && !prev) {
            (*flashes)++;
        }
        if (on) {
            (*onTicks)++;
        }
        prev = on;
    }
}

static void selectionRows(void)
{
    ledInputs_t in;

    /* Row 7: advertising, nobody connected. */
    in = base();
    check("row 7: advertising -> short blink every 3 s", ledSelect(&in) == LED_PAT_ADV_BLINK);

    /* Row 6: connected, UNBOUND or NONE. */
    in = base(); in.connected = true; in.session = BLE_UI_SESSION_UNBOUND;
    check("row 6: connected unbound -> double blink", ledSelect(&in) == LED_PAT_DOUBLE_BLINK);
    in.session = BLE_UI_SESSION_NONE;
    check("row 6: connected, no session -> double blink", ledSelect(&in) == LED_PAT_DOUBLE_BLINK);

    /* Row 5: connected, BOUND. */
    in = base(); in.connected = true; in.session = BLE_UI_SESSION_BOUND;
    check("row 5: connected bound -> steady", ledSelect(&in) == LED_PAT_STEADY);

    /* Row 5 needs the connection: a stale BOUND while advertising. */
    in = base(); in.session = BLE_UI_SESSION_BOUND;
    check("row 5 needs connected: bound but advertising -> row 7", ledSelect(&in) == LED_PAT_ADV_BLINK);

    /* Row 4 beats row 5. */
    in = base(); in.connected = true; in.session = BLE_UI_SESSION_BOUND; in.pairWindow = true;
    check("row 4 over 5: pair window beats bound", ledSelect(&in) == LED_PAT_FAST_BLINK);
    in = base(); in.pairWindow = true;
    check("row 4: pair window while advertising -> fast blink", ledSelect(&in) == LED_PAT_FAST_BLINK);

    /* Row 3 beats row 4. */
    in = base(); in.pairWindow = true; in.advertiseOff = true;
    check("row 3 over 4: BLE off beats pair window", ledSelect(&in) == LED_PAT_OFF);
    in = base(); in.advertiseOff = true;
    check("row 3: BLE off -> off", ledSelect(&in) == LED_PAT_OFF);

    /* Row 2 beats row 3. */
    in = base(); in.advertiseOff = true; in.hold = LED_HOLD_LONG;
    check("row 2 over 3: hold 2 s beats BLE off", ledSelect(&in) == LED_PAT_FAST_BLINK);
    in = base(); in.advertiseOff = true; in.hold = LED_HOLD_VERY_LONG;
    check("row 2 over 3: hold 10 s beats BLE off", ledSelect(&in) == LED_PAT_STEADY);
    in = base(); in.connected = true; in.session = BLE_UI_SESSION_BOUND; in.hold = LED_HOLD_LONG;
    check("row 2: hold 2 s while bound -> fast blink", ledSelect(&in) == LED_PAT_FAST_BLINK);
    in = base(); in.pairWindow = true; in.hold = LED_HOLD_VERY_LONG;
    check("row 2: hold 10 s in pair window -> steady", ledSelect(&in) == LED_PAT_STEADY);
    in = base(); in.hold = LED_HOLD_NONE; in.connected = true; in.session = BLE_UI_SESSION_BOUND;
    check("row 2: short/dead/stuck hold has no feedback", ledSelect(&in) == LED_PAT_STEADY);

    /* Row 1 beats row 2. */
    in = base(); in.hold = LED_HOLD_VERY_LONG; in.cue = LED_CUE_BIND_ACCEPTED;
    check("row 1 over 2: cue beats hold", ledSelect(&in) == LED_PAT_CUE_BIND_ACCEPTED);
    in = base(); in.advertiseOff = true; in.cue = LED_CUE_BINDS_CLEARED;
    check("row 1: cue plays even with BLE off", ledSelect(&in) == LED_PAT_CUE_BINDS_CLEARED);
    in = base(); in.cue = LED_CUE_PRESS;
    check("row 1: local press cue", ledSelect(&in) == LED_PAT_CUE_PRESS);
}

static void timing(void)
{
    int      flashes;
    uint32_t on;

    check("off: never on", (scan(LED_PAT_OFF, 1000, &flashes, &on), on == 0));
    check("steady: always on", (scan(LED_PAT_STEADY, 1000, &flashes, &on), on == 1000));

    /* 5 Hz: period 200 ms, 50 % duty. */
    check("fast blink: period 200 ms", ledPatternTicks(LED_PAT_FAST_BLINK) == MS(200));
    scan(LED_PAT_FAST_BLINK, MS(1000), &flashes, &on);
    check("fast blink: 5 flashes per second", flashes == 5 && on == MS(500));

    /* Double blink every 2 s. */
    check("double blink: period 2 s", ledPatternTicks(LED_PAT_DOUBLE_BLINK) == MS(2000));
    scan(LED_PAT_DOUBLE_BLINK, MS(2000), &flashes, &on);
    check("double blink: 2 flashes per period", flashes == 2);
    scan(LED_PAT_DOUBLE_BLINK, MS(6000), &flashes, &on);
    check("double blink: repeats (6 flashes in 6 s)", flashes == 6);
    check("double blink: starts on (no dead lead-in)", ledLevel(LED_PAT_DOUBLE_BLINK, 0));

    /* Short blink every 3 s. */
    check("adv blink: period 3 s", ledPatternTicks(LED_PAT_ADV_BLINK) == MS(3000));
    scan(LED_PAT_ADV_BLINK, MS(9000), &flashes, &on);
    check("adv blink: one short flash per period", flashes == 3 && on <= 3U * MS(200));

    /* Cues are one-shots. */
    check("bind accepted: one-shot", ledPatternIsOneShot(LED_PAT_CUE_BIND_ACCEPTED));
    scan(LED_PAT_CUE_BIND_ACCEPTED, 1000, &flashes, &on);
    check("bind accepted: 3 quick flashes", flashes == 3);
    check("bind accepted: done within 1.5 s", ledPatternTicks(LED_PAT_CUE_BIND_ACCEPTED) <= MS(1500));
    check("bind accepted: off past its end",
          !ledLevel(LED_PAT_CUE_BIND_ACCEPTED, ledPatternTicks(LED_PAT_CUE_BIND_ACCEPTED)));

    check("binds cleared: one-shot", ledPatternIsOneShot(LED_PAT_CUE_BINDS_CLEARED));
    scan(LED_PAT_CUE_BINDS_CLEARED, 1000, &flashes, &on);
    check("binds cleared: one 1 s flash", flashes == 1 && on == MS(1000));

    scan(LED_PAT_CUE_PRESS, 1000, &flashes, &on);
    check("press cue: one blink", flashes == 1 && ledPatternIsOneShot(LED_PAT_CUE_PRESS));

    check("repeating patterns are not one-shots",
          !ledPatternIsOneShot(LED_PAT_FAST_BLINK) && !ledPatternIsOneShot(LED_PAT_STEADY));
    check("cue frames start off (visible over steady on)",
          !ledLevel(LED_PAT_CUE_BIND_ACCEPTED, 0) && !ledLevel(LED_PAT_CUE_BINDS_CLEARED, 0));
}

static void renderer(void)
{
    ledRenderer_t r;
    ledInputs_t   in = base();
    in.connected = true;
    in.session   = BLE_UI_SESSION_BOUND;              /* base: steady on */

    ledRendererInit(&r);
    check("renderer: bound -> on", ledRendererTick(&r, &in, 0));

    ledRendererCue(&r, LED_CUE_BIND_ACCEPTED);
    int flashes = 0;
    bool prev = ledRendererTick(&r, &in, 0);
    const uint32_t len = ledPatternTicks(LED_PAT_CUE_BIND_ACCEPTED);
    for (uint32_t t = 1; t < len; t++) {
        const bool on = ledRendererTick(&r, &in, 1);
        flashes += (on && !prev) ? 1 : 0;
        prev = on;
    }
    check("renderer: cue plays its 3 flashes over steady", flashes == 3);
    check("renderer: back to steady after the cue", ledRendererTick(&r, &in, 1) && r.cue == LED_CUE_NONE);

    /* Pattern change restarts at phase 0: switch to double blink mid-way
     * through a steady phase, the first tick of it must be "on". */
    ledRendererTick(&r, &in, 7);
    in.session = BLE_UI_SESSION_UNBOUND;
    check("renderer: new pattern starts at phase 0", ledRendererTick(&r, &in, 13) && r.phase == 0);
    ledRendererTick(&r, &in, 2);
    check("renderer: phase advances by elapsed ticks", r.phase == 2 && !ledRendererTick(&r, &in, 0));

    /* A cue survives a big elapsed jump only for its length. */
    ledRendererCue(&r, LED_CUE_BINDS_CLEARED);
    ledRendererTick(&r, &in, 0);
    ledRendererTick(&r, &in, ledPatternTicks(LED_PAT_CUE_BINDS_CLEARED));
    check("renderer: cue ends after its length", r.cue == LED_CUE_NONE);

    /* Restarting a cue restarts it from the beginning. */
    ledRendererCue(&r, LED_CUE_PRESS);
    ledRendererTick(&r, &in, 0);
    ledRendererTick(&r, &in, 5);
    ledRendererCue(&r, LED_CUE_PRESS);
    ledRendererTick(&r, &in, 0);
    check("renderer: cue restart rewinds", r.cue == LED_CUE_PRESS && r.cueTick == 0);
}

int main(void)
{
    selectionRows();
    timing();
    renderer();
    return checkSummary("host_led");
}
