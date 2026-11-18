/*
 * Pure button logic. See button_logic.h.
 */

#include "button_logic.h"

static void buttonLogicPush(buttonLogic_t *l, buttonPress_e press)
{
    if (l->queued < BUTTON_QUEUE_DEPTH) {
        l->queue[l->queued++] = (uint8_t)press;
    }
    /* Full: the loop stalled for BUTTON_QUEUE_DEPTH presses; drop the newest. */
}

static buttonPress_e buttonLogicClassify(uint32_t heldMs)
{
    if (heldMs >= BUTTON_VERY_LONG_MS) {
        return BUTTON_PRESS_VERY_LONG;
    }
    return (heldMs >= BUTTON_LONG_MS) ? BUTTON_PRESS_LONG : BUTTON_PRESS_SHORT;
}

void buttonLogicInit(buttonLogic_t *l, bool rawPressed, uint32_t nowMs)
{
    l->queued    = 0U;
    l->edgeMs    = nowMs;
    l->pressMs   = nowMs;
    l->releaseMs = nowMs;
    l->state     = rawPressed ? BUTTON_WAIT_RELEASE : BUTTON_IDLE;
}

void buttonLogicStep(buttonLogic_t *l, bool raw, uint32_t nowMs)
{
    switch (l->state) {
    case BUTTON_IDLE:
        if (raw) {
            l->state   = BUTTON_DEBOUNCE_PRESS;
            l->edgeMs  = nowMs;
            l->pressMs = nowMs;
        }
        break;

    case BUTTON_DEBOUNCE_PRESS:
        if (!raw) {
            l->state = BUTTON_IDLE;                 /* bounce or glitch */
        } else if (nowMs - l->edgeMs >= BUTTON_DEBOUNCE_MS) {
            l->state = BUTTON_HELD;
        }
        break;

    case BUTTON_HELD:
        if (!raw) {
            l->state     = BUTTON_DEBOUNCE_RELEASE;
            l->edgeMs    = nowMs;
            l->releaseMs = nowMs;
        }
        break;

    case BUTTON_DEBOUNCE_RELEASE:
        if (raw) {
            l->state = BUTTON_HELD;                 /* bounce: still held */
        } else if (nowMs - l->edgeMs >= BUTTON_DEBOUNCE_MS) {
            buttonLogicPush(l, buttonLogicClassify(l->releaseMs - l->pressMs));
            l->state = BUTTON_IDLE;
        }
        break;

    case BUTTON_WAIT_RELEASE:
        if (raw) {
            l->edgeMs = nowMs;                      /* restart the quiet window */
        } else if (nowMs - l->edgeMs >= BUTTON_DEBOUNCE_MS) {
            l->state = BUTTON_IDLE;
        }
        break;
    }
}

buttonPress_e buttonLogicTake(buttonLogic_t *l)
{
    if (l->queued == 0U) {
        return BUTTON_PRESS_NONE;
    }
    buttonPress_e press = (buttonPress_e)l->queue[0];
    l->queued--;
    for (uint8_t i = 0U; i < l->queued; i++) {
        l->queue[i] = l->queue[i + 1U];
    }
    return press;
}

bool buttonLogicIsPressed(const buttonLogic_t *l)
{
    return l->state == BUTTON_HELD || l->state == BUTTON_DEBOUNCE_RELEASE;
}
