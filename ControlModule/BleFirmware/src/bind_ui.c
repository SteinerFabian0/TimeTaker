/*
 * Bind button and BLE LED. See bind_ui.h.
 */

#include "bind_ui.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/atomic.h>

#include "ble_link_protocol.h"

BUILD_ASSERT((LED_TICK_MS % BTN_TICK_MS) == 0U, "LED tick must be a multiple of the button tick");
#define BTN_TICKS_PER_LED_TICK (LED_TICK_MS / BTN_TICK_MS)

static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_NODELABEL(bind_button), gpios);
static const struct gpio_dt_spec led    = GPIO_DT_SPEC_GET(DT_NODELABEL(ble_led), gpios);

static void (*wakeFn)(void);

/* ISR side (timer expiry). */
static btnClassifier_t classifier;
static uint8_t         subTick;
static atomic_t        pendingPress;       /* btnEvent_e, 0 = none */
static atomic_t        holdPhase;          /* btnHold_e */
static atomic_t        ledTicks;           /* LED ticks not yet rendered */
static struct k_timer  uiTimer;

/* Main side. */
static ledRenderer_t renderer;
static ledInputs_t   inputs;
static bool          ledOn;

static void uiTimerExpiry(struct k_timer *t)
{
    ARG_UNUSED(t);
    bool wake = false;

    const int level = gpio_pin_get_dt(&button);     /* 1 = pressed (active low in DTS) */
    const btnEvent_e ev = btnSample(&classifier, level > 0);
    if (ev != BTN_EVT_NONE) {
        atomic_set(&pendingPress, ev);
        wake = true;
    }
    const btnHold_e hold = btnHold(&classifier);
    if (atomic_set(&holdPhase, hold) != (atomic_val_t)hold) {
        wake = true;
    }

    if (++subTick >= BTN_TICKS_PER_LED_TICK) {
        subTick = 0U;
        atomic_inc(&ledTicks);
        wake = true;
    }
    if (wake && wakeFn != NULL) {
        wakeFn();
    }
}

int bindUiInit(void (*wake)(void))
{
    wakeFn = wake;
    if (!gpio_is_ready_dt(&button) || !gpio_is_ready_dt(&led)) {
        return -ENODEV;
    }
    /* No internal pull: R501 is the pull-up (guide §3.2). */
    int err = gpio_pin_configure_dt(&button, GPIO_INPUT);
    err |= gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
    if (err != 0) {
        return err;
    }

    btnInit(&classifier);
    ledRendererInit(&renderer);
    inputs = (ledInputs_t){ .session = BLE_UI_SESSION_NONE };

    k_timer_init(&uiTimer, uiTimerExpiry, NULL);
    k_timer_start(&uiTimer, K_MSEC(BTN_TICK_MS), K_MSEC(BTN_TICK_MS));
    return 0;
}

static ledHold_e toLedHold(btnHold_e h)
{
    switch (h) {
    case BTN_HOLD_LONG:      return LED_HOLD_LONG;
    case BTN_HOLD_VERY_LONG: return LED_HOLD_VERY_LONG;
    default:                 return LED_HOLD_NONE;   /* < 2 s, or stuck: normal LED */
    }
}

btnEvent_e bindUiProcess(void)
{
    const btnEvent_e press = (btnEvent_e)atomic_clear(&pendingPress);
    if (press == BTN_EVT_SHORT) {
        ledRendererCue(&renderer, LED_CUE_PRESS);
    }

    inputs.hold = toLedHold((btnHold_e)atomic_get(&holdPhase));
    const uint32_t elapsed = (uint32_t)atomic_clear(&ledTicks);
    const bool on = ledRendererTick(&renderer, &inputs, elapsed);
    if (on != ledOn) {
        ledOn = on;
        (void)gpio_pin_set_dt(&led, on ? 1 : 0);
    }
    return press;
}

void bindUiSetUiState(uint8_t session, bool pairWindow)
{
    inputs.session    = session;
    inputs.pairWindow = pairWindow;
}

void bindUiResetSession(void)
{
    inputs.session = BLE_UI_SESSION_NONE;
}

void bindUiCue(ledCue_e cue)
{
    ledRendererCue(&renderer, cue);
}

void bindUiSetLink(bool connected, bool advertiseOff)
{
    inputs.connected    = connected;
    inputs.advertiseOff = advertiseOff;
}
