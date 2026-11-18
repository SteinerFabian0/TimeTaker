/*
 * Bind button classifier. See button_classifier.h.
 */

#include "button_classifier.h"

void btnInit(btnClassifier_t *b)
{
    *b = (btnClassifier_t){ .stable = false, .last = false, .sameCount = BTN_DEBOUNCE_SAMPLES };
}

btnEvent_e btnClassify(uint32_t ticks)
{
    if (ticks < BTN_DEAD_MIN_TICKS) {
        return BTN_EVT_SHORT;
    }
    if (ticks < BTN_LONG_MIN_TICKS) {
        return BTN_EVT_NONE;                 /* dead zone */
    }
    if (ticks < BTN_VERY_LONG_MIN_TICKS) {
        return BTN_EVT_LONG;
    }
    if (ticks < BTN_STUCK_MIN_TICKS) {
        return BTN_EVT_VERY_LONG;
    }
    return BTN_EVT_NONE;                     /* stuck button */
}

btnEvent_e btnSample(btnClassifier_t *b, bool pressed)
{
    if (pressed == b->last) {
        if (b->sameCount < BTN_DEBOUNCE_SAMPLES) {
            b->sameCount++;
        }
    } else {
        b->last      = pressed;
        b->sameCount = 1U;
    }

    if (b->stable && b->heldTicks < BTN_STUCK_MIN_TICKS) {
        b->heldTicks++;
    }

    if (b->sameCount < BTN_DEBOUNCE_SAMPLES || pressed == b->stable) {
        return BTN_EVT_NONE;
    }

    b->stable = pressed;
    if (pressed) {
        b->heldTicks = 0U;
        return BTN_EVT_NONE;
    }
    return btnClassify(b->heldTicks);
}

btnHold_e btnHold(const btnClassifier_t *b)
{
    if (!b->stable) {
        return BTN_HOLD_NONE;
    }
    if (b->heldTicks < BTN_DEAD_MIN_TICKS) {
        return BTN_HOLD_SHORT;
    }
    if (b->heldTicks < BTN_LONG_MIN_TICKS) {
        return BTN_HOLD_DEAD;
    }
    if (b->heldTicks < BTN_VERY_LONG_MIN_TICKS) {
        return BTN_HOLD_LONG;
    }
    if (b->heldTicks < BTN_STUCK_MIN_TICKS) {
        return BTN_HOLD_VERY_LONG;
    }
    return BTN_HOLD_STUCK;
}
