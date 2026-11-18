/*
 * Pure button logic: debounce and press classification for one button.
 *
 * No hardware, so it is host-tested (test/host_button). button_handler feeds
 * it the raw pin level once per super-loop pass.
 *
 * One press yields exactly one classification, reported when the release is
 * confirmed, from how long the button was held (first raw edge to first raw
 * release edge):
 *   SHORT      < BUTTON_LONG_MS
 *   LONG       BUTTON_LONG_MS .. < BUTTON_VERY_LONG_MS
 *   VERY_LONG  >= BUTTON_VERY_LONG_MS
 * A button already down at init is ignored until it has been released.
 * Finished presses queue (BUTTON_QUEUE_DEPTH) until taken. Every time
 * difference is an unsigned 32-bit subtraction, so the ms tick may wrap.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "button_handler.h"

#define BUTTON_QUEUE_DEPTH  4U

typedef enum {
    BUTTON_IDLE = 0,
    BUTTON_DEBOUNCE_PRESS,
    BUTTON_HELD,
    BUTTON_DEBOUNCE_RELEASE,
    BUTTON_WAIT_RELEASE,        /* down at boot: not a press */
} buttonState_e;

typedef struct {
    buttonState_e state;
    uint32_t      edgeMs;       /* start of the current debounce window */
    uint32_t      pressMs;      /* first raw press edge */
    uint32_t      releaseMs;    /* first raw release edge of the pending release */
    uint8_t       queue[BUTTON_QUEUE_DEPTH];
    uint8_t       queued;
} buttonLogic_t;

void buttonLogicInit(buttonLogic_t *l, bool rawPressed, uint32_t nowMs);
void buttonLogicStep(buttonLogic_t *l, bool rawPressed, uint32_t nowMs);

/* Oldest finished press, or BUTTON_PRESS_NONE. */
buttonPress_e buttonLogicTake(buttonLogic_t *l);

/* Confirmed down (after debounce, until the release is confirmed). */
bool buttonLogicIsPressed(const buttonLogic_t *l);
