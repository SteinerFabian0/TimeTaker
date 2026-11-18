/*
 * Bind button classifier (SW501, P0.00): debounce + hold-time classes.
 * BLE_FIRMWARE_GUIDE.md §7.1.
 *
 * The caller samples the button every BTN_TICK_MS and feeds the level here.
 * A level change counts once BTN_DEBOUNCE_SAMPLES equal samples agree; the
 * press and the release are delayed by the same amount, so the measured hold
 * time is the physical one.
 *
 *   held (debounced)        on release            while held (LED feedback)
 *   < 1 s                   BTN_EVT_SHORT         -
 *   1 s .. < 2 s            nothing (dead zone)   -
 *   2 s .. < 10 s           BTN_EVT_LONG          BTN_HOLD_LONG (fast blink)
 *   10 s .. < 30 s          BTN_EVT_VERY_LONG     BTN_HOLD_VERY_LONG (steady)
 *   >= 30 s                 nothing (stuck)       BTN_HOLD_STUCK (normal LED)
 *
 * The event values equal blePress_e (and the host protocol's pressKind).
 *
 * Pure logic: no Zephyr, no globals. Not reentrant per classifier.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BTN_TICK_MS               10U
#define BTN_DEBOUNCE_SAMPLES      3U

/* Thresholds in ticks of BTN_TICK_MS. */
#define BTN_DEAD_MIN_TICKS        (1000U  / BTN_TICK_MS)   /* 1 s  */
#define BTN_LONG_MIN_TICKS        (2000U  / BTN_TICK_MS)   /* 2 s  */
#define BTN_VERY_LONG_MIN_TICKS   (10000U / BTN_TICK_MS)   /* 10 s */
#define BTN_STUCK_MIN_TICKS       (30000U / BTN_TICK_MS)   /* 30 s */

typedef enum {
    BTN_EVT_NONE      = 0,
    BTN_EVT_SHORT     = 1,   /* = BLE_PRESS_SHORT     */
    BTN_EVT_LONG      = 2,   /* = BLE_PRESS_LONG      */
    BTN_EVT_VERY_LONG = 3,   /* = BLE_PRESS_VERY_LONG */
} btnEvent_e;

typedef enum {
    BTN_HOLD_NONE      = 0,  /* released                          */
    BTN_HOLD_SHORT     = 1,  /* held < 1 s                        */
    BTN_HOLD_DEAD      = 2,  /* held 1..2 s                       */
    BTN_HOLD_LONG      = 3,  /* held 2..10 s: "pairing" preview   */
    BTN_HOLD_VERY_LONG = 4,  /* held 10..30 s: "release to clear" */
    BTN_HOLD_STUCK     = 5,  /* held >= 30 s                      */
} btnHold_e;

typedef struct {
    bool     stable;       /* debounced level: true = pressed           */
    bool     last;         /* last raw sample                           */
    uint8_t  sameCount;    /* consecutive raw samples equal to `last`   */
    uint32_t heldTicks;    /* ticks since the debounced press (saturates) */
} btnClassifier_t;

/* Starts in the released state. */
void btnInit(btnClassifier_t *b);

/* Feeds one sample (true = pressed). Returns the class of a press that was
 * released with this sample, or BTN_EVT_NONE. */
btnEvent_e btnSample(btnClassifier_t *b, bool pressed);

/* Hold phase of the press in progress (BTN_HOLD_NONE when released). */
btnHold_e btnHold(const btnClassifier_t *b);

/* The class a press held for `ticks` gets on release. */
btnEvent_e btnClassify(uint32_t ticks);
