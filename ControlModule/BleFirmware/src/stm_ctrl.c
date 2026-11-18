/*
 * The nRF's control lines to the STM32. See stm_ctrl.h.
 */

#include "stm_ctrl.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/atomic.h>

#define STM_LINES DT_NODELABEL(stm_lines)

/* The DTS flags carry GPIO_OPEN_DRAIN for nrst; the configure below adds
 * nothing that could make it push-pull. */
BUILD_ASSERT((DT_GPIO_FLAGS(STM_LINES, nrst_gpios) & GPIO_OPEN_DRAIN) == GPIO_OPEN_DRAIN,
             "NRST must be open drain in the board DTS");

static const struct gpio_dt_spec bleIrq  = GPIO_DT_SPEC_GET(STM_LINES, ble_irq_gpios);
static const struct gpio_dt_spec bootReq = GPIO_DT_SPEC_GET(STM_LINES, boot_req_gpios);
static const struct gpio_dt_spec nrst    = GPIO_DT_SPEC_GET(STM_LINES, nrst_gpios);
static const struct gpio_dt_spec boot0   = GPIO_DT_SPEC_GET(STM_LINES, boot0_gpios);

#define NRST_PULSE_MS       10
#define BOOT_REQ_HOLD_MS    200

typedef enum {
    SEQ_IDLE = 0,
    SEQ_PULSE_RELEASE,          /* NRST low; next: release NRST              */
    SEQ_BOOT_RELEASE_NRST,      /* BOOT_REQ + NRST low; next: release NRST   */
    SEQ_BOOT_RELEASE_REQ,       /* BOOT_REQ low; next: release BOOT_REQ      */
} seqState_e;

static atomic_t seqState;
static struct k_work_delayable seqWork;

static void seqStep(struct k_work *work)
{
    ARG_UNUSED(work);

    switch ((seqState_e)atomic_get(&seqState)) {
    case SEQ_PULSE_RELEASE:
        (void)gpio_pin_set_dt(&nrst, 0);
        atomic_set(&seqState, SEQ_IDLE);
        break;
    case SEQ_BOOT_RELEASE_NRST:
        (void)gpio_pin_set_dt(&nrst, 0);
        atomic_set(&seqState, SEQ_BOOT_RELEASE_REQ);
        (void)k_work_schedule(&seqWork, K_MSEC(BOOT_REQ_HOLD_MS));
        break;
    case SEQ_BOOT_RELEASE_REQ:
        (void)gpio_pin_set_dt(&bootReq, 0);
        atomic_set(&seqState, SEQ_IDLE);
        break;
    case SEQ_IDLE:
    default:
        break;
    }
}

int stmCtrlInit(void)
{
    if (!gpio_is_ready_dt(&bleIrq) || !gpio_is_ready_dt(&bootReq)
        || !gpio_is_ready_dt(&nrst) || !gpio_is_ready_dt(&boot0)) {
        return -ENODEV;
    }

    /* Inactive = released: with GPIO_OPEN_DRAIN | GPIO_ACTIVE_LOW the pin is
     * configured S0D1 with OUT = 1 in one PIN_CNF write. */
    int err = gpio_pin_configure_dt(&nrst, GPIO_OUTPUT_INACTIVE);
    err |= gpio_pin_configure_dt(&bootReq, GPIO_OUTPUT_INACTIVE);   /* high */
    err |= gpio_pin_configure_dt(&bleIrq, GPIO_OUTPUT_INACTIVE);    /* low  */
    err |= gpio_pin_configure_dt(&boot0, GPIO_DISCONNECTED);        /* v1: never driven */

    k_work_init_delayable(&seqWork, seqStep);
    atomic_set(&seqState, SEQ_IDLE);
    return err;
}

void stmCtrlSetBleIrq(bool connected)
{
    (void)gpio_pin_set_dt(&bleIrq, connected ? 1 : 0);
}

bool stmCtrlResetPulse(void)
{
    if (!atomic_cas(&seqState, SEQ_IDLE, SEQ_PULSE_RELEASE)) {
        return false;
    }
    (void)gpio_pin_set_dt(&nrst, 1);                 /* pull low */
    (void)k_work_schedule(&seqWork, K_MSEC(NRST_PULSE_MS));
    return true;
}

bool stmCtrlResetIntoBootloader(void)
{
    if (!atomic_cas(&seqState, SEQ_IDLE, SEQ_BOOT_RELEASE_NRST)) {
        return false;
    }
    (void)gpio_pin_set_dt(&bootReq, 1);              /* low: request */
    (void)gpio_pin_set_dt(&nrst, 1);                 /* pull low     */
    (void)k_work_schedule(&seqWork, K_MSEC(NRST_PULSE_MS));
    return true;
}

bool stmCtrlBusy(void)
{
    return atomic_get(&seqState) != SEQ_IDLE;
}
