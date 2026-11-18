/*
 * TRNG. See rng.h.
 */

#include "rng.h"

#include <stdbool.h>
#include <stdint.h>

#include "stm32_hal.h"
#include "mx_rng.h"
#include "ctrl_status.h"

#define RNG_TIMEOUT_MS  2U

static hal_rng_handle_t *rngHandle;
static uint32_t          rngLast = 0x9E3779B9U;

ctrlStatus_e rngInit(void)
{
    rngHandle = mx_rng_init();
    return (rngHandle != NULL) ? CTRL_OK : CTRL_ERR_HAL;
}

ctrlStatus_e rngGet(uint32_t *word)
{
    uint32_t v = 0U;
    if (rngHandle != NULL && HAL_RNG_GenerateRandomNumber(rngHandle, &v, 1U, RNG_TIMEOUT_MS) == HAL_OK) {
        rngLast = v;
        *word = v;
        return CTRL_OK;
    }
    if (rngHandle != NULL) {
        (void)HAL_RNG_RecoverSeedError(rngHandle);
    }
    /* Fallback: never block a start on the RNG. */
    rngLast = (rngLast * 1664525U) + 1013904223U + HAL_GetTick() + (LL_TIM_GetCounter(TIM12) << 16);
    *word = rngLast;
    return CTRL_ERR_HAL;
}
