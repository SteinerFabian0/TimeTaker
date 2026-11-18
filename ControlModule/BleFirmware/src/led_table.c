/*
 * BLE LED patterns. See led_table.h. The LED code is a table, not logic.
 */

#include "led_table.h"

#include <stddef.h>

#include "ble_link_protocol.h"

/* Durations in LED_TICK_MS ticks, alternating, starting with `firstOn`. */
typedef struct {
    const uint8_t *steps;
    uint8_t        count;
    bool           firstOn;
    bool           oneShot;
} ledPatternDef_t;

static const uint8_t stepsOne[]          = { 1 };
static const uint8_t stepsFastBlink[]    = { 2, 2 };                       /* 100 on, 100 off: 5 Hz */
static const uint8_t stepsDoubleBlink[]  = { 2, 3, 2, 33 };                /* 2 s period            */
static const uint8_t stepsAdvBlink[]     = { 2, 58 };                      /* 3 s period            */
static const uint8_t stepsBindAccepted[] = { 3, 3, 3, 3, 3, 3, 3 };        /* off-framed 3 flashes  */
static const uint8_t stepsBindsCleared[] = { 4, 20, 4 };                   /* off 200, on 1 s, off  */
static const uint8_t stepsPress[]        = { 3, 4, 3 };                    /* off, on 200 ms, off   */

#define DEF(arr, on, shot) { (arr), (uint8_t)(sizeof(arr) / sizeof((arr)[0])), (on), (shot) }

static const ledPatternDef_t patterns[LED_PAT_COUNT] = {
    [LED_PAT_OFF]               = DEF(stepsOne,          false, false),
    [LED_PAT_STEADY]            = DEF(stepsOne,          true,  false),
    [LED_PAT_FAST_BLINK]        = DEF(stepsFastBlink,    true,  false),
    [LED_PAT_DOUBLE_BLINK]      = DEF(stepsDoubleBlink,  true,  false),
    [LED_PAT_ADV_BLINK]         = DEF(stepsAdvBlink,     true,  false),
    [LED_PAT_CUE_BIND_ACCEPTED] = DEF(stepsBindAccepted, false, true),
    [LED_PAT_CUE_BINDS_CLEARED] = DEF(stepsBindsCleared, false, true),
    [LED_PAT_CUE_PRESS]         = DEF(stepsPress,        false, true),
};

uint32_t ledPatternTicks(ledPattern_e p)
{
    if ((unsigned)p >= LED_PAT_COUNT) {
        return 1U;
    }
    uint32_t total = 0U;
    for (uint8_t i = 0U; i < patterns[p].count; i++) {
        total += patterns[p].steps[i];
    }
    return total;
}

bool ledPatternIsOneShot(ledPattern_e p)
{
    return (unsigned)p < LED_PAT_COUNT && patterns[p].oneShot;
}

bool ledLevel(ledPattern_e p, uint32_t tick)
{
    if ((unsigned)p >= LED_PAT_COUNT) {
        return false;
    }
    const ledPatternDef_t *d = &patterns[p];
    const uint32_t total = ledPatternTicks(p);

    if (d->oneShot) {
        if (tick >= total) {
            return false;
        }
    } else {
        tick %= total;
    }

    bool on = d->firstOn;
    for (uint8_t i = 0U; i < d->count; i++) {
        if (tick < d->steps[i]) {
            return on;
        }
        tick -= d->steps[i];
        on = !on;
    }
    return false;                     /* not reached */
}

ledPattern_e ledCuePattern(ledCue_e cue)
{
    switch (cue) {
    case LED_CUE_BIND_ACCEPTED: return LED_PAT_CUE_BIND_ACCEPTED;
    case LED_CUE_BINDS_CLEARED: return LED_PAT_CUE_BINDS_CLEARED;
    case LED_CUE_PRESS:         return LED_PAT_CUE_PRESS;
    case LED_CUE_NONE:
    default:                    return LED_PAT_OFF;
    }
}

ledPattern_e ledSelect(const ledInputs_t *in)
{
    if (in->cue != LED_CUE_NONE) {
        return ledCuePattern(in->cue);
    }
    if (in->hold == LED_HOLD_LONG) {
        return LED_PAT_FAST_BLINK;
    }
    if (in->hold == LED_HOLD_VERY_LONG) {
        return LED_PAT_STEADY;
    }
    if (in->advertiseOff) {
        return LED_PAT_OFF;
    }
    if (in->pairWindow) {
        return LED_PAT_FAST_BLINK;
    }
    if (in->connected) {
        return (in->session == BLE_UI_SESSION_BOUND) ? LED_PAT_STEADY : LED_PAT_DOUBLE_BLINK;
    }
    return LED_PAT_ADV_BLINK;
}

void ledRendererInit(ledRenderer_t *r)
{
    *r = (ledRenderer_t){ .pattern = LED_PAT_OFF, .phase = 0U, .cue = LED_CUE_NONE, .cueTick = 0U };
}

void ledRendererCue(ledRenderer_t *r, ledCue_e cue)
{
    r->cue     = cue;
    r->cueTick = 0U;
    /* Force a restart even if the same cue was already playing. */
    r->pattern = LED_PAT_COUNT;
}

bool ledRendererTick(ledRenderer_t *r, const ledInputs_t *in, uint32_t elapsed)
{
    if (r->cue != LED_CUE_NONE) {
        r->cueTick += elapsed;
        if (r->cueTick >= ledPatternTicks(ledCuePattern(r->cue))) {
            r->cue = LED_CUE_NONE;
        }
    }

    ledInputs_t eff = *in;
    eff.cue = r->cue;
    const ledPattern_e want = ledSelect(&eff);

    if (want != r->pattern) {
        r->pattern = want;
        r->phase   = 0U;
    } else {
        r->phase += elapsed;
    }

    return ledLevel(r->pattern, (r->cue != LED_CUE_NONE) ? r->cueTick : r->phase);
}
