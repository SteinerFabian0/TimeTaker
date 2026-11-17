/*
 * TIM2 timebase, SYNC capture and alarms. See lane_timer.h for the ownership
 * rules.
 */

#include "lane_timer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "stm32_hal.h"
#include "mx_tim2.h"
#include "lane_status.h"
#include "sync_time.h"
#include "irq_guard.h"

/* Written by the TIM2 ISR; everyone else reads a copy taken under the guard. */
static syncTime_t laneTimerSync;

static volatile uint32_t laneTimerOverruns;

typedef struct {
    laneTimerAlarmFn_t fn;
    uint32_t           due;
    bool               armed;
} laneTimerAlarmSlot_t;

static laneTimerAlarmSlot_t laneTimerAlarms[LANE_TIMER_ALARM_COUNT];

static const hal_tim_channel_t laneTimerAlarmChannel[LANE_TIMER_ALARM_COUNT] = {
    [LANE_TIMER_ALARM_SAMPLE]   = HAL_TIM_CHANNEL_2,
    [LANE_TIMER_ALARM_SCHEDULE] = HAL_TIM_CHANNEL_4,
};

/* Register-level alarm control: the HAL's channel state machine would trap on
 * re-arming from an ISR, and these are single register writes anyway. */
static void laneTimerAlarmHw(laneTimerAlarm_e alarm, bool enable, uint32_t due)
{
    if (laneTimerAlarmChannel[alarm] == HAL_TIM_CHANNEL_2) {
        if (enable) {
            LL_TIM_OC_SetCompareCH2(TIM2, due);
            LL_TIM_ClearFlag_CC2(TIM2);
            LL_TIM_EnableIT_CC2(TIM2);
        } else {
            LL_TIM_DisableIT_CC2(TIM2);
            LL_TIM_ClearFlag_CC2(TIM2);
        }
    } else {
        if (enable) {
            LL_TIM_OC_SetCompareCH4(TIM2, due);
            LL_TIM_ClearFlag_CC4(TIM2);
            LL_TIM_EnableIT_CC4(TIM2);
        } else {
            LL_TIM_DisableIT_CC4(TIM2);
            LL_TIM_ClearFlag_CC4(TIM2);
        }
    }
}

static void laneTimerAlarmForce(laneTimerAlarm_e alarm)
{
    if (laneTimerAlarmChannel[alarm] == HAL_TIM_CHANNEL_2) {
        LL_TIM_GenerateEvent_CC2(TIM2);
    } else {
        LL_TIM_GenerateEvent_CC4(TIM2);
    }
}

laneStatus_e laneTimerInit(void)
{
    hal_tim_handle_t *htim = mx_tim2_gethandle();

    (void)syncTimeInit(&laneTimerSync, SYNC_TIME_DEFAULT_PERIOD_US);
    laneTimerOverruns = 0U;
    for (uint32_t i = 0U; i < (uint32_t)LANE_TIMER_ALARM_COUNT; i++) {
        laneTimerAlarms[i].armed = false;
    }

    if (HAL_TIM_IC_StartChannel_IT(htim, HAL_TIM_CHANNEL_1) != HAL_OK) {
        return LANE_ERR_TIMER;
    }
    if (HAL_TIM_Start(htim) != HAL_OK) {
        return LANE_ERR_TIMER;
    }

    return LANE_OK;
}

uint32_t laneTimerNow(void)
{
    return LL_TIM_GetCounter(TIM2);
}

laneStatus_e laneTimerSetSyncPeriod(uint32_t periodUs)
{
    if (periodUs < SYNC_TIME_PERIOD_MIN_US || periodUs > SYNC_TIME_PERIOD_MAX_US) {
        return LANE_ERR_INVALID_ARG;
    }

    uint32_t guard = irqGuardEnter();
    laneStatus_e status = syncTimeInit(&laneTimerSync, periodUs);
    irqGuardExit(guard);

    return status;
}

laneStatus_e laneTimerSyncAlign(uint32_t mbTick, uint32_t mbPhaseUs, uint32_t localRx)
{
    uint32_t guard = irqGuardEnter();
    laneStatus_e status = syncTimeAlign(&laneTimerSync, mbTick, mbPhaseUs, localRx);
    irqGuardExit(guard);

    return status;
}

void laneTimerGetSync(syncTime_t *out)
{
    uint32_t guard = irqGuardEnter();
    *out = laneTimerSync;
    irqGuardExit(guard);
}

uint8_t laneTimerToMainboard(uint32_t local, uint64_t *mainboardUs)
{
    syncTime_t snap;
    laneTimerGetSync(&snap);

    return syncTimeToMainboard(&snap, local, laneTimerNow(), mainboardUs);
}

laneStatus_e laneTimerToLocal(uint64_t mainboardUs, uint32_t *local)
{
    syncTime_t snap;
    laneTimerGetSync(&snap);

    return syncTimeToLocal(&snap, mainboardUs, local);
}

bool laneTimerSyncLocked(void)
{
    syncTime_t snap;
    laneTimerGetSync(&snap);

    return syncTimeIsLocked(&snap, laneTimerNow());
}

uint32_t laneTimerCaptureOverruns(void)
{
    return laneTimerOverruns;
}

laneStatus_e laneTimerArmAlarm(laneTimerAlarm_e alarm, uint32_t dueLocal,
                               laneTimerAlarmFn_t fn)
{
    if ((uint32_t)alarm >= (uint32_t)LANE_TIMER_ALARM_COUNT || fn == NULL) {
        return LANE_ERR_INVALID_ARG;
    }

    laneStatus_e status = LANE_OK;
    uint32_t     guard  = irqGuardEnter();
    laneTimerAlarmSlot_t *slot = &laneTimerAlarms[alarm];

    if (slot->armed) {
        status = LANE_ERR_ALARM_BUSY;
    } else if ((int32_t)(dueLocal - laneTimerNow()) > (int32_t)LANE_TIMER_ALARM_MAX_AHEAD) {
        status = LANE_ERR_ALARM_RANGE;
    } else {
        slot->fn    = fn;
        slot->due   = dueLocal;
        slot->armed = true;
        laneTimerAlarmHw(alarm, true, dueLocal);

        /* The compare only matches on equality. Checked after it is live, so
         * a due time the counter passed meanwhile cannot slip through: force
         * the flag and let the ISR fire it now. */
        if ((int32_t)(dueLocal - laneTimerNow()) <= 0) {
            laneTimerAlarmForce(alarm);
        }
    }

    irqGuardExit(guard);
    return status;
}

void laneTimerCancelAlarm(laneTimerAlarm_e alarm)
{
    if ((uint32_t)alarm >= (uint32_t)LANE_TIMER_ALARM_COUNT) {
        return;
    }

    uint32_t guard = irqGuardEnter();
    laneTimerAlarmHw(alarm, false, 0U);
    laneTimerAlarms[alarm].armed = false;
    irqGuardExit(guard);
}

bool laneTimerAlarmArmed(laneTimerAlarm_e alarm)
{
    return ((uint32_t)alarm < (uint32_t)LANE_TIMER_ALARM_COUNT)
           && laneTimerAlarms[alarm].armed;
}


/* =========================================================================
 *                     HAL CALLBACKS (TIM2 ISR, priority 1)
 * ========================================================================= */

void HAL_TIM_InputCaptureCallback(hal_tim_handle_t *htim, hal_tim_channel_t channel)
{
    if (htim != mx_tim2_gethandle() || channel != HAL_TIM_CHANNEL_1) {
        return;
    }

    /* Reading CCR1 clears CC1IF; an overcapture means an edge was lost
     * unread, which sync_time sees as a missed period. */
    uint32_t capture = LL_TIM_IC_GetCaptureCH1(TIM2);
    if (LL_TIM_IsActiveFlag_CC1OVR(TIM2) != 0U) {
        LL_TIM_ClearFlag_CC1OVR(TIM2);
        laneTimerOverruns++;
    }

    (void)syncTimeOnEdge(&laneTimerSync, capture);
}

void HAL_TIM_CompareMatchCallback(hal_tim_handle_t *htim, hal_tim_channel_t channel)
{
    if (htim != mx_tim2_gethandle()) {
        return;
    }

    uint32_t now = laneTimerNow();

    for (uint32_t i = 0U; i < (uint32_t)LANE_TIMER_ALARM_COUNT; i++) {
        laneTimerAlarmSlot_t *slot = &laneTimerAlarms[i];
        if (laneTimerAlarmChannel[i] != channel || !slot->armed) {
            continue;
        }

        /* One-shot: disarmed before the call, so the callback may re-arm. */
        laneTimerAlarmHw((laneTimerAlarm_e)i, false, 0U);
        slot->armed = false;
        slot->fn(slot->due, now);
    }
}
