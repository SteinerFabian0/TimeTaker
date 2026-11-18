/*
 * BLE LED (D603, P0.20) pattern table, selection and rendering.
 * BLE_FIRMWARE_GUIDE.md §7.2.
 *
 * The nRF52805 has no PWM, so the LED is on or off. Patterns are tables of
 * on/off durations in ticks of LED_TICK_MS. The selection is the guide's
 * table, first matching row wins:
 *
 *   1. a cue is playing            the cue (one-shot)
 *   2. button held 2..10 s         fast blink   (preview of "pairing")
 *      button held 10..30 s        steady on    ("release to clear all")
 *   3. ADVERTISE{0}                off
 *   4. pairWindow = 1              fast blink 5 Hz
 *   5. connected, session BOUND    steady on
 *   6. connected, other session    double blink every 2 s
 *   7. otherwise (advertising)     short blink every 3 s
 *
 * Pure logic: no Zephyr, no globals.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define LED_TICK_MS 50U

typedef enum {
    LED_PAT_OFF = 0,
    LED_PAT_STEADY,
    LED_PAT_FAST_BLINK,           /* 5 Hz                                   */
    LED_PAT_DOUBLE_BLINK,         /* two blinks every 2 s                   */
    LED_PAT_ADV_BLINK,            /* one short blink every 3 s              */
    LED_PAT_CUE_BIND_ACCEPTED,    /* one-shot: 3 quick flashes              */
    LED_PAT_CUE_BINDS_CLEARED,    /* one-shot: one 1 s flash                */
    LED_PAT_CUE_PRESS,            /* one-shot: one blink (short press, local) */
    LED_PAT_COUNT
} ledPattern_e;

typedef enum {
    LED_CUE_NONE = 0,
    LED_CUE_BIND_ACCEPTED,        /* CUE{BIND_ACCEPTED} from the STM */
    LED_CUE_BINDS_CLEARED,        /* CUE{BINDS_CLEARED} from the STM */
    LED_CUE_PRESS,                /* local feedback for a short press */
} ledCue_e;

typedef enum {
    LED_HOLD_NONE = 0,            /* not held, or no feedback for this hold */
    LED_HOLD_LONG,                /* held 2..10 s  */
    LED_HOLD_VERY_LONG,           /* held 10..30 s */
} ledHold_e;

typedef struct {
    ledCue_e  cue;                /* cue playing (renderer fills this in) */
    ledHold_e hold;
    bool      advertiseOff;       /* the STM sent ADVERTISE{0}            */
    bool      pairWindow;         /* UI_STATE.pairWindow                  */
    bool      connected;
    uint8_t   session;            /* UI_STATE.session (bleUiSession_e)    */
} ledInputs_t;

/* The pattern for these inputs (the table above). */
ledPattern_e ledSelect(const ledInputs_t *in);

/* LED level of pattern p at `tick` since the pattern started. A one-shot
 * pattern is off past its end. */
bool ledLevel(ledPattern_e p, uint32_t tick);

/* Period of a repeating pattern, length of a one-shot, in ticks. */
uint32_t ledPatternTicks(ledPattern_e p);

bool ledPatternIsOneShot(ledPattern_e p);

/* The one-shot pattern of a cue (LED_PAT_OFF for LED_CUE_NONE). */
ledPattern_e ledCuePattern(ledCue_e cue);

/* --- Renderer: pattern phase, cue playback -------------------------------- */

typedef struct {
    ledPattern_e pattern;         /* pattern shown at the last tick */
    uint32_t     phase;           /* ticks since `pattern` started  */
    ledCue_e     cue;             /* cue playing, or LED_CUE_NONE   */
    uint32_t     cueTick;         /* ticks since the cue started    */
} ledRenderer_t;

void ledRendererInit(ledRenderer_t *r);

/* Starts a cue (restarts it if one is playing). */
void ledRendererCue(ledRenderer_t *r, ledCue_e cue);

/* Advances by `elapsed` ticks (0 = re-evaluate now) and returns the LED
 * level. A pattern change restarts the new pattern at phase 0. in->cue is
 * ignored; the renderer's own cue is used. */
bool ledRendererTick(ledRenderer_t *r, const ledInputs_t *in, uint32_t elapsed);
