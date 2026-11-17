/*
 * Panel buttons. See button_handler.h.
 */

#include "button_handler.h"

#include <stdbool.h>
#include <stdint.h>

#include "stm32_hal.h"
#include "mx_gpio_default.h"

typedef enum {
    BUTTON_IDLE = 0,
    BUTTON_DEBOUNCE_PRESS,
    BUTTON_HELD,
    BUTTON_DEBOUNCE_RELEASE,
} buttonState_e;

typedef struct {
    hal_gpio_t    port;
    uint32_t      pin;
    buttonState_e state;
    uint32_t      edgeMs;       /* start of the current debounce window */
    uint32_t      pressedMs;    /* confirmed press instant */
    uint8_t       reported;     /* highest classification already reported in this press */
    buttonPress_e latched;      /* unread classification */
} button_t;

static button_t buttons[BUTTON_COUNT] = {
    [BUTTON_BTN1 - 1] = { .port = BTN1_PORT, .pin = BTN1_PIN },
    [BUTTON_BTN2 - 1] = { .port = BTN2_PORT, .pin = BTN2_PIN },
    [BUTTON_BTN3 - 1] = { .port = BTN3_PORT, .pin = BTN3_PIN },
};

static bool buttonValid(buttonId_e id)
{
    return id >= BUTTON_BTN1 && id <= BUTTON_COUNT;
}

static bool buttonRawPressed(const button_t *b)
{
    return HAL_GPIO_ReadPin(b->port, b->pin) == HAL_GPIO_PIN_RESET;
}

static void buttonReport(button_t *b, buttonPress_e press)
{
    b->latched  = press;
    b->reported = (uint8_t)press;
}

static void buttonStep(button_t *b, bool raw, uint32_t nowMs)
{
    switch (b->state) {
    case BUTTON_IDLE:
        if (raw) {
            b->state  = BUTTON_DEBOUNCE_PRESS;
            b->edgeMs = nowMs;
        }
        break;

    case BUTTON_DEBOUNCE_PRESS:
        if (!raw) {
            b->state = BUTTON_IDLE;                 /* bounce or glitch */
        } else if (nowMs - b->edgeMs >= BUTTON_DEBOUNCE_MS) {
            b->state     = BUTTON_HELD;
            b->pressedMs = nowMs;
            b->reported  = (uint8_t)BUTTON_PRESS_NONE;
        }
        break;

    case BUTTON_HELD: {
        uint32_t heldMs = nowMs - b->pressedMs;
        if (b->reported < (uint8_t)BUTTON_PRESS_LONG && heldMs >= BUTTON_LONG_MS) {
            buttonReport(b, BUTTON_PRESS_LONG);
        }
        if (b->reported < (uint8_t)BUTTON_PRESS_VERY_LONG && heldMs >= BUTTON_VERY_LONG_MS) {
            buttonReport(b, BUTTON_PRESS_VERY_LONG);
        }
        if (!raw) {
            b->state  = BUTTON_DEBOUNCE_RELEASE;
            b->edgeMs = nowMs;
        }
        break;
    }

    case BUTTON_DEBOUNCE_RELEASE:
        if (raw) {
            b->state = BUTTON_HELD;                 /* bounce: still held */
        } else if (nowMs - b->edgeMs >= BUTTON_DEBOUNCE_MS) {
            if (b->reported == (uint8_t)BUTTON_PRESS_NONE) {
                buttonReport(b, BUTTON_PRESS_SHORT);
            }
            b->state = BUTTON_IDLE;
        }
        break;
    }
}

void buttonHandlerInit(void)
{
    for (uint32_t i = 0U; i < (uint32_t)BUTTON_COUNT; i++) {
        buttons[i].state    = BUTTON_IDLE;
        buttons[i].reported = (uint8_t)BUTTON_PRESS_NONE;
        buttons[i].latched  = BUTTON_PRESS_NONE;
    }
}

void buttonHandlerPoll(uint32_t nowMs)
{
    for (uint32_t i = 0U; i < (uint32_t)BUTTON_COUNT; i++) {
        buttonStep(&buttons[i], buttonRawPressed(&buttons[i]), nowMs);
    }
}

buttonPress_e buttonHandlerTakePress(buttonId_e id)
{
    if (!buttonValid(id)) {
        return BUTTON_PRESS_NONE;
    }
    button_t     *b     = &buttons[id - 1];
    buttonPress_e press = b->latched;
    b->latched = BUTTON_PRESS_NONE;
    return press;
}

bool buttonHandlerIsPressed(buttonId_e id)
{
    if (!buttonValid(id)) {
        return false;
    }
    buttonState_e s = buttons[id - 1].state;
    return s == BUTTON_HELD || s == BUTTON_DEBOUNCE_RELEASE;
}
