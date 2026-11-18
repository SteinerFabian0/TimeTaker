/*
 * System time on TIM12. See sys_time.h.
 */

#include "sys_time.h"

#include <stdbool.h>
#include <stdint.h>

#include "stm32_hal.h"
#include "mx_tim12.h"
#include "irq_guard.h"
#include "ctrl_status.h"

static volatile uint64_t sysTimeTickCount;
static bool              sysTimeSyncOn;

ctrlStatus_e sysTimeInit(void)
{
    sysTimeTickCount = 0U;
    sysTimeSyncOn    = false;

    /* Counter + update interrupt. The CH2 output stays disabled, which parks
     * SYNC low. */
    if (HAL_TIM_Start_IT(mx_tim12_gethandle()) != HAL_OK) {
        return CTRL_ERR_TIMER;
    }
    return CTRL_OK;
}

uint64_t sysTimeNowUs(void)
{
    uint32_t guard = irqGuardEnter();
    uint64_t ticks = sysTimeTickCount;
    uint32_t cnt   = LL_TIM_GetCounter(TIM12);

    /* The counter may have wrapped after the last ISR ran (we are masked, or
     * called from a higher-priority ISR). Then UIF is still pending: count
     * that tick here and read CNT again, so a value read just before the wrap
     * cannot pair with the old tick count. */
    if (LL_TIM_IsActiveFlag_UPDATE(TIM12) != 0U) {
        cnt = LL_TIM_GetCounter(TIM12);
        ticks++;
    }
    irqGuardExit(guard);

    return ticks * SYS_TIME_TICK_US + cnt;
}

void sysTimeNowSplit(uint32_t *tick, uint32_t *phaseUs)
{
    uint32_t guard = irqGuardEnter();
    uint64_t ticks = sysTimeTickCount;
    uint32_t cnt   = LL_TIM_GetCounter(TIM12);
    if (LL_TIM_IsActiveFlag_UPDATE(TIM12) != 0U) {
        cnt = LL_TIM_GetCounter(TIM12);
        ticks++;
    }
    irqGuardExit(guard);

    *tick    = (uint32_t)ticks;
    *phaseUs = cnt;
}

uint64_t sysTimeTicks(void)
{
    uint32_t guard = irqGuardEnter();
    uint64_t ticks = sysTimeTickCount;
    irqGuardExit(guard);
    return ticks;
}

ctrlStatus_e sysTimeSetSyncOutput(bool on)
{
    if (on == sysTimeSyncOn) {
        return CTRL_OK;
    }

    hal_tim_handle_t *htim = mx_tim12_gethandle();
    hal_status_t      hs   = on ? HAL_TIM_OC_StartChannel(htim, HAL_TIM_CHANNEL_2)
                                : HAL_TIM_OC_StopChannel(htim, HAL_TIM_CHANNEL_2);
    if (hs != HAL_OK) {
        return CTRL_ERR_TIMER;
    }
    sysTimeSyncOn = on;
    return CTRL_OK;
}

bool sysTimeIsSyncOutputOn(void)
{
    return sysTimeSyncOn;
}

/* The HAL clears UIF before calling this, so sysTimeNowUs() sees the flag only
 * while this ISR is still pending. */
void HAL_TIM_UpdateCallback(hal_tim_handle_t *htim)
{
    if (htim == mx_tim12_gethandle()) {
        sysTimeTickCount++;
    }
}
