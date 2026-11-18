/*
 * sequence_manager.c
 *
 * @brief  Start sequence. See sequence_manager.h.
 */

#include "sequence_manager.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ctrl_status.h"
#include "host_protocol.h"
#include "lane_protocol.h"
#include "lane_logic.h"
#include "seq_planner.h"
#include "rng.h"
#include "sys_time.h"
#include "log_line.h"
#include "lane_manager.h"
#include "host_manager.h"

typedef enum {
    SEQ_IDLE = 0,
    SEQ_ARMING,      /* actions queued, waiting for every lane to confirm */
    SEQ_ARMED,       /* every lane confirmed, waiting for Tout */
} seqState_e;

static seqState_e seqState;
static seqPlan_t  seqPlan;
static uint8_t    seqLanes;          /* participating lanes (mask) */
static uint8_t    seqLights;

static void seqLog(const char *what, int32_t v)
{
    logLine_t l;
    logLineInit(&l, "SEQ ");
    logLineStr(&l, what);
    logLineKv(&l, "v", v);
    hostManagerLog(HP_LOG_INFO, &l);
}

static ctrlStatus_e seqAction(uint8_t lane, uint8_t op, uint8_t action, uint16_t param, uint64_t atUs)
{
    laneSchedule_t a;
    memset(&a, 0, sizeof(a));
    a.op     = op;
    a.action = action;
    a.param  = param;
    a.atUs   = atUs;
    return laneManagerSchedule(lane, &a);
}

ctrlStatus_e sequenceManagerInit(void)
{
    seqState = SEQ_IDLE;
    seqLanes = 0U;
    return CTRL_OK;
}

bool sequenceManagerIsRunning(void)
{
    return seqState != SEQ_IDLE;
}

/* Cancels every lane's actions and blinks. publish=false: START refused
 * (ERR_LANE_NACK), nothing is published (sim A12). */
static ctrlStatus_e seqStop(uint8_t reason, bool publish)
{
    if (seqState == SEQ_IDLE) {
        return CTRL_ERR_INVALID_STATE;
    }
    seqState = SEQ_IDLE;

    const laneLed_t blink = {
        .mode            = LANE_LED_BLINK,
        .level           = 1000U,
        .blinkPeriodMs   = SEQUENCE_MANAGER_ABORT_BLINK_MS,
        .blinkOnPermille = 500U,
        .blinkCount      = 3U,
    };
    for (uint8_t n = 1U; n <= LANE_MAX_LANES; n++) {
        if ((seqLanes & LANE_LOGIC_MASK(n)) != 0U) {
            (void)seqAction(n, LANE_SCHED_CANCEL_ALL, 0U, 0U, 0U);   /* gone lanes refuse: fine */
        }
    }
    if (seqLanes != 0U) {
        (void)laneManagerSetLed(seqLanes, &blink);
    }
    laneManagerScheduleForget();
    seqLanes  = 0U;
    seqLights = 0U;
    if (!publish) {
        return CTRL_OK;
    }

    const hpEvtSeqAborted_t evt = { .reason = reason, .atUs = sysTimeNowUs() };
    seqLog("ABORTED", reason);
    return hostManagerPublishEvent(HP_MSG_EVT_SEQ_ABORTED, &evt, sizeof(evt));
}

ctrlStatus_e sequenceManagerAbort(uint8_t reason)
{
    return seqStop(reason, true);
}

ctrlStatus_e sequenceManagerStart(const hpStartSequenceReq_t *req, uint64_t *t0Us)
{
    if (seqState != SEQ_IDLE) {
        return CTRL_ERR_SEQ_RUNNING;
    }
    if (req->armSampling > 1U) {
        return CTRL_ERR_INVALID_ARG;
    }

    const uint8_t lanes = laneManagerConfiguredMask();
    uint32_t word = 0U;
    (void)rngGet(&word);   /* a fallback word on an RNG failure still makes a fair hold */

    const seqPlanParams_t params = {
        .lights          = req->lights,
        .armSampling     = req->armSampling,
        .attentionPulses = req->attentionPulses,
        .pulseRiseMs     = req->pulseRiseMs,
        .stepMs    = req->stepMs,
        .holdMinMs = req->holdMinMs,
        .holdMaxMs = req->holdMaxMs,
        .leadMs    = req->leadMs,
    };
    CTRL_RETURN_IF_ERR(seqPlannerPlan(&params, lanes, sysTimeNowUs(), word, &seqPlan));

    laneManagerScheduleForget();
    seqLanes  = lanes;
    seqLights = seqPlan.litCount;
    seqState  = SEQ_ARMING;

    for (uint8_t n = 1U; n <= LANE_MAX_LANES; n++) {
        if ((lanes & LANE_LOGIC_MASK(n)) == 0U) {
            continue;
        }
        const seqPlanLane_t *pl = &seqPlan.lane[n - 1U];
        ctrlStatus_e st = seqAction(n, LANE_SCHED_CANCEL_ALL, 0U, 0U, 0U);
        for (uint8_t i = 0U; st == CTRL_OK && i < pl->count; i++) {
            st = seqAction(n, LANE_SCHED_ADD, pl->act[i].action, pl->act[i].param, pl->act[i].atUs);
        }
        const uint8_t expect = pl->count;
        if (st == CTRL_OK) {
            st = laneManagerScheduleVerify(n, expect);
        }
        if (st != CTRL_OK) {
            (void)seqStop(HP_ABORT_LANE_FAILED, false);
            return (st == CTRL_ERR_BUSY) ? CTRL_ERR_BUSY : CTRL_ERR_LANE_NACK;
        }
    }

    *t0Us = seqPlan.t0Us;
    const hpEvtSeqStarted_t evt = {
        .lights      = seqPlan.litCount,
        .armSampling = req->armSampling,
        .stepMs      = req->stepMs,
        .t0Us        = seqPlan.t0Us,
    };
    seqLog("STARTED", seqPlan.litCount);
    return hostManagerPublishEvent(HP_MSG_EVT_SEQ_STARTED, &evt, sizeof(evt));
}

ctrlStatus_e sequenceManagerRun(void)
{
    if (seqState == SEQ_IDLE) {
        return CTRL_OK;
    }
    const uint64_t now = sysTimeNowUs();

    if (!laneManagerIsPowered()) {
        return sequenceManagerAbort(HP_ABORT_POWER);
    }
    if ((laneManagerConfiguredMask() & seqLanes) != seqLanes) {
        return sequenceManagerAbort(HP_ABORT_LANE_LEFT);
    }

    if (seqState == SEQ_ARMING) {
        bool allOk = true;
        for (uint8_t n = 1U; n <= LANE_MAX_LANES; n++) {
            if ((seqLanes & LANE_LOGIC_MASK(n)) == 0U) {
                continue;
            }
            const laneSchedResult_e r = laneManagerScheduleResult(n);
            if (r == LANE_SCHED_RESULT_FAILED) {
                return sequenceManagerAbort(HP_ABORT_LANE_FAILED);
            }
            allOk = allOk && r == LANE_SCHED_RESULT_OK;
        }
        if (allOk) {
            seqState = SEQ_ARMED;
        } else if (now + (uint64_t)SEQUENCE_MANAGER_CONFIRM_MS * 1000U >= seqPlan.t0Us) {
            return sequenceManagerAbort(HP_ABORT_LANE_FAILED);   /* better than a dark lane */
        }
    }

    if (now >= seqPlan.toutUs) {
        const hpEvtLightsOut_t evt = { .lights = seqLights, .toutUs = seqPlan.toutUs };
        const int32_t holdUs = (int32_t)seqPlan.holdUs;
        seqState = SEQ_IDLE;
        seqLanes = 0U;
        seqLights = 0U;
        laneManagerScheduleForget();
        seqLog("LIGHTS OUT", holdUs);
        return hostManagerPublishEvent(HP_MSG_EVT_LIGHTS_OUT, &evt, sizeof(evt));
    }
    return CTRL_OK;
}
