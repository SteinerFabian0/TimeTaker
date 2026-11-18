/*
 * Panel buttons BTN1..BTN3: debounce and press classification.
 *
 *   BTN1 = PC15 (SW601), BTN2 = PC13 (SW602), BTN3 = PE2 (SW603)
 *   Active low, MCU internal pull-ups (CubeMX), no external pull resistors.
 *
 * Polling, no interrupts: call buttonHandlerPoll() every super-loop pass. The
 * EXTI lines CubeMX enabled on these pins stay harmless (their HAL callbacks
 * are the weak defaults); they become the Stop1 wake sources once sys_power
 * is ported.
 *
 * One press = one classification, reported when the release is confirmed, by
 * how long it was held:
 *   SHORT      < BUTTON_LONG_MS
 *   LONG       BUTTON_LONG_MS .. < BUTTON_VERY_LONG_MS
 *   VERY_LONG  >= BUTTON_VERY_LONG_MS
 * A button held across boot is ignored until released. Finished presses queue
 * (a few per button) until buttonHandlerTakePress() reads them, so a slow loop
 * never loses one. Logic: button_logic.c (host-tested).
 *
 * The values of buttonId_e and buttonPress_e equal hpButtonId_e and
 * hpPressKind_e in host_protocol.h, so they go into EVT_BUTTON unchanged.
 * The policy (what a press MEANS) belongs to panel_manager and the web app.
 *
 * Owns: the three button pins (read only).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BUTTON_DEBOUNCE_MS   20U
#define BUTTON_LONG_MS       1000U
#define BUTTON_VERY_LONG_MS  5000U

typedef enum {
    BUTTON_BTN1  = 1,
    BUTTON_BTN2  = 2,
    BUTTON_BTN3  = 3,
    BUTTON_COUNT = 3,   /* ids are 1-based: the count is the last id */
} buttonId_e;

typedef enum {
    BUTTON_PRESS_NONE      = 0,
    BUTTON_PRESS_SHORT     = 1,
    BUTTON_PRESS_LONG      = 2,
    BUTTON_PRESS_VERY_LONG = 3,
} buttonPress_e;

/* Idles every button. Call once after mx_gpio_default_init(). */
void buttonHandlerInit(void);

/* Reads every button and advances its state machine. Non-blocking. */
void buttonHandlerPoll(uint32_t nowMs);

/* Returns and clears the latched classification of a button. */
buttonPress_e buttonHandlerTakePress(buttonId_e id);

/* True while the button is confirmed pressed (after debounce). */
bool buttonHandlerIsPressed(buttonId_e id);
