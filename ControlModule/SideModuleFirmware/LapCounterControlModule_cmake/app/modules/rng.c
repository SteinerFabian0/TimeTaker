/*
 * TRNG. See rng.h.
 */

#include "rng.h"

#include <stdbool.h>
#include <stdint.h>

#include "stm32_hal.h"
#include "mx_rng.h"
#include "ctrl_status.h"

/* A word takes well under 1 ms; the HAL timeout counts 1 ms ticks, so 2 could
 * expire after one tick. */
#define RNG_TIMEOUT_MS  10U

static hal_rng_handle_t *rngHandle;
static uint32_t          rngLast = 0x9E3779B9U;

ctrlStatus_e rngInit(void)
{
    rngHandle = mx_rng_init();
    if (rngHandle == NULL) {
        return CTRL_ERR_HAL;
    }
    /* The Yuhka hub's fixed mx_rng.c does these two after the NIST config;
     * our generated mx_rng.c lacks them (CubeMX change pending), so do them
     * here (state IDLE, config not locked). Auto-reset lets the RNG recover
     * from a seed error by itself; clock error detection flags a lost
     * 48 MHz kernel clock instead of silently stalling. */
    if (HAL_RNG_EnableAutoReset(rngHandle) != HAL_OK ||
        HAL_RNG_EnableClockErrorDetection(rngHandle) != HAL_OK) {
        return CTRL_ERR_HAL;     /* RNG still works; rngGet falls back as needed */
    }
    return CTRL_OK;
}

ctrlStatus_e rngGet(uint32_t *word)
{
    if (rngHandle != NULL) {
        uint32_t v = 0U;
        for (uint8_t attempt = 0U; attempt < 2U; attempt++) {
            hal_status_t st = HAL_RNG_GenerateRandomNumber(rngHandle, &v, 1U, RNG_TIMEOUT_MS);
            if (st == HAL_OK) {
                rngLast = v;
                *word = v;
                return CTRL_OK;
            }
            /* Only a seed error leaves the HAL in the ERROR state, and only
             * then is recovery legal; a timeout leaves it IDLE (recovering
             * there trips the HAL state assert). One recovery + one retry. */
            if (st != HAL_ERROR || HAL_RNG_RecoverSeedError(rngHandle) != HAL_OK) {
                break;
            }
        }
    }
    /* Fallback: never block a start on the RNG. */
    rngLast = (rngLast * 1664525U) + 1013904223U + HAL_GetTick() + (LL_TIM_GetCounter(TIM12) << 16);
    *word = rngLast;
    return CTRL_ERR_HAL;
}
