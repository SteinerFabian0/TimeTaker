/* Host shim: the HAL names app/ code needs off the board. The status values
 * are the C5 HAL's real ones — HAL_OK is not 0 there, and a shim that made it
 * 0 would hide exactly the `status == 0` bug the target punishes. */

#pragma once

#include <stdint.h>

typedef enum {
    HAL_OK            = 0xEAEAEAEAU,
    HAL_ERROR         = 0xF5F5F5F5U,
    HAL_BUSY          = 0x55555555U,
    HAL_INVALID_PARAM = 0xAAAAAAAAU,
    HAL_TIMEOUT       = 0x5A5A5A5AU,
} hal_status_t;

/* Host clock: a test advances it by hand, so nothing depends on wall time. */
extern uint32_t hostTickMs;

static inline uint32_t HAL_GetTick(void)
{
    return hostTickMs;
}

/* No interrupts on the host: irq_guard compiles to nothing. */
static inline uint32_t __get_PRIMASK(void)
{
    return 0U;
}

static inline void __disable_irq(void)
{
}

static inline void __set_PRIMASK(uint32_t primask)
{
    (void)primask;
}

/* TIM: the slice indicator_led uses. The handle is opaque; a suite that links
 * a TIM user defines the functions below as fakes (test/host_led). */
typedef struct hostTimHandle hal_tim_handle_t;

typedef enum {
    HAL_TIM_CHANNEL_1 = 0U,
    HAL_TIM_CHANNEL_2 = 1U,
    HAL_TIM_CHANNEL_3 = 2U,
    HAL_TIM_CHANNEL_4 = 3U,
} hal_tim_channel_t;

typedef enum {
    HAL_TIM_OC_COMPARE_UNIT_1 = 1U,
    HAL_TIM_OC_COMPARE_UNIT_2 = 2U,
    HAL_TIM_OC_COMPARE_UNIT_3 = 4U,
    HAL_TIM_OC_COMPARE_UNIT_4 = 8U,
} hal_tim_oc_compare_unit_t;

static inline hal_tim_oc_compare_unit_t hal_tim_oc_channel_to_compare_unit(hal_tim_channel_t channel)
{
    static const hal_tim_oc_compare_unit_t map[] = {
        HAL_TIM_OC_COMPARE_UNIT_1, HAL_TIM_OC_COMPARE_UNIT_2,
        HAL_TIM_OC_COMPARE_UNIT_3, HAL_TIM_OC_COMPARE_UNIT_4,
    };
    return map[channel];
}

uint32_t     HAL_TIM_GetPeriod(const hal_tim_handle_t *htim);
hal_status_t HAL_TIM_OC_SetCompareUnitPulse(hal_tim_handle_t *htim,
                                            hal_tim_oc_compare_unit_t compare_unit,
                                            uint32_t pulse);
hal_status_t HAL_TIM_OC_StartChannel(hal_tim_handle_t *htim, hal_tim_channel_t channel);
hal_status_t HAL_TIM_OC_StopChannel(hal_tim_handle_t *htim, hal_tim_channel_t channel);
hal_status_t HAL_TIM_Start(hal_tim_handle_t *htim);
hal_status_t HAL_TIM_Stop(hal_tim_handle_t *htim);
