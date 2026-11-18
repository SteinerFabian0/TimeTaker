/*
 * Panel buttons. See button_handler.h; the logic is in button_logic.c.
 */

#include "button_handler.h"

#include <stdbool.h>
#include <stdint.h>

#include "stm32_hal.h"
#include "mx_gpio_default.h"
#include "button_logic.h"

typedef struct {
    hal_gpio_t    port;
    uint32_t      pin;
    buttonLogic_t logic;
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

void buttonHandlerInit(void)
{
    const uint32_t nowMs = HAL_GetTick();
    for (uint32_t i = 0U; i < (uint32_t)BUTTON_COUNT; i++) {
        buttonLogicInit(&buttons[i].logic, buttonRawPressed(&buttons[i]), nowMs);
    }
}

void buttonHandlerPoll(uint32_t nowMs)
{
    for (uint32_t i = 0U; i < (uint32_t)BUTTON_COUNT; i++) {
        buttonLogicStep(&buttons[i].logic, buttonRawPressed(&buttons[i]), nowMs);
    }
}

buttonPress_e buttonHandlerTakePress(buttonId_e id)
{
    if (!buttonValid(id)) {
        return BUTTON_PRESS_NONE;
    }
    return buttonLogicTake(&buttons[id - 1].logic);
}

bool buttonHandlerIsPressed(buttonId_e id)
{
    return buttonValid(id) && buttonLogicIsPressed(&buttons[id - 1].logic);
}
