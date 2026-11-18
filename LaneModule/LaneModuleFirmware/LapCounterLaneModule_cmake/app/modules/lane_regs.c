/*
 * Register-map decoding and validation. See lane_regs.h for the rules.
 */

#include "lane_regs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "led_pattern.h"
#include "sync_time.h"

/* PS_IT codes 0..7 = 1, 1.5, 2, 2.5, 3, 3.5, 4, 8 T, in half T. */
static const uint8_t laneRegsItHalfT[LANE_CFG_PS_INTEGRATION_MAX + 1U] = { 2, 3, 4, 5, 6, 7, 8, 16 };

void laneRegsInit(laneRegs_t *r)
{
    r->badWrites  = 0U;
    r->lastReject = LANE_OK;
}

void laneRegsReject(laneRegs_t *r, laneStatus_e why)
{
    if (r->badWrites < 0xFFFFU) {
        r->badWrites++;
    }
    r->lastReject = why;
}

uint16_t laneRegsBadWrites(const laneRegs_t *r)
{
    return r->badWrites;
}

void laneRegsClearCounters(laneRegs_t *r)
{
    r->badWrites = 0U;
}

bool laneRegsIsKnown(uint8_t reg)
{
    return reg <= (uint8_t)LANE_REG_RAW_FIFO;
}

uint32_t laneRegsWriteSize(uint8_t reg)
{
    switch (reg) {
    case LANE_REG_LANE_ID:    return sizeof(laneIndex_t);
    case LANE_REG_CONFIG:     return sizeof(laneConfig_t);
    case LANE_REG_LED:        return sizeof(laneLed_t);
    case LANE_REG_SCHEDULE:   return sizeof(laneSchedule_t);
    case LANE_REG_COMMAND:    return sizeof(laneCommand_t);
    case LANE_REG_SYNC_ALIGN: return sizeof(laneSyncAlign_t);
    default:                  return 0U;
    }
}

uint32_t laneRegsConversionUs(uint8_t psIntegration, uint8_t psMultiPulse)
{
    uint32_t it     = (psIntegration <= LANE_CFG_PS_INTEGRATION_MAX) ? psIntegration : LANE_CFG_PS_INTEGRATION_MAX;
    uint32_t pulses = 1UL << ((psMultiPulse <= LANE_CFG_PS_MULTI_PULSE_MAX) ? psMultiPulse : LANE_CFG_PS_MULTI_PULSE_MAX);

    return LANE_REGS_VCNL_OVERHEAD_US + pulses * laneRegsItHalfT[it] * LANE_REGS_VCNL_T_US / 2U;
}

uint32_t laneRegsMinSamplePeriodUs(uint8_t psIntegration, uint8_t psMultiPulse)
{
    return LANE_REGS_I2C_TRIGGER_US + laneRegsConversionUs(psIntegration, psMultiPulse)
           + LANE_REGS_I2C_READ_US + LANE_REGS_SAMPLE_MARGIN_US;
}

void laneRegsDefaultConfig(laneConfig_t *c)
{
    memset(c, 0, sizeof(*c));
    c->samplePeriodUs        = 1000U;
    c->samplePhaseUs         = 0U;
    c->mergeGapUs            = 30000U;
    c->blockTimeoutUs        = 500000U;
    c->holdoffUs             = 0U;
    c->holdoffSamplePeriodUs = 0U;
    c->thrDropPermille       = 300U;
    c->hystPermille          = 100U;
    c->minDwell10us          = 200U;
    c->rawDecimation         = 1U;
    c->irCurrent             = 7U;
    c->psIntegration         = 3U;
    c->psMultiPulse          = 0U;
}

laneStatus_e laneRegsValidateConfig(const laneConfig_t *c)
{
    /* The period floor is the sensor cycle (checked below), and hyst < drop
     * also makes drop >= 1. */
    bool ok = c->samplePeriodUs <= LANE_CFG_SAMPLE_PERIOD_US_MAX
              && c->samplePhaseUs < c->samplePeriodUs
              && c->mergeGapUs <= LANE_CFG_MERGE_GAP_US_MAX
              && c->blockTimeoutUs > c->mergeGapUs
              && c->blockTimeoutUs <= LANE_CFG_BLOCK_TIMEOUT_US_MAX
              && c->holdoffUs <= LANE_CFG_HOLDOFF_US_MAX
              && (c->holdoffSamplePeriodUs == 0U
                  || (c->holdoffSamplePeriodUs >= c->samplePeriodUs
                      && c->holdoffSamplePeriodUs <= LANE_CFG_HOLDOFF_SAMPLE_PERIOD_US_MAX))
              && c->thrDropPermille <= LANE_CFG_THR_DROP_PERMILLE_MAX
              && c->hystPermille < c->thrDropPermille
              && (uint32_t)c->minDwell10us * 10U < c->blockTimeoutUs
              && c->rawDecimation >= LANE_CFG_RAW_DECIMATION_MIN && c->rawDecimation <= LANE_CFG_RAW_DECIMATION_MAX
              && c->irCurrent <= LANE_CFG_IR_CURRENT_MAX
              && c->psIntegration <= LANE_CFG_PS_INTEGRATION_MAX
              && c->psMultiPulse <= LANE_CFG_PS_MULTI_PULSE_MAX;

    /* Only once the codes are known valid: the period must fit the sensor. */
    if (ok && c->samplePeriodUs < laneRegsMinSamplePeriodUs(c->psIntegration, c->psMultiPulse)) {
        ok = false;
    }

    return ok ? LANE_OK : LANE_ERR_REG_VALUE;
}

static laneStatus_e laneRegsValidateLed(const laneLed_t *l)
{
    if (l->mode > (uint8_t)LANE_LED_BLINK || l->level > LED_LEVEL_MAX) {
        return LANE_ERR_REG_VALUE;
    }
    if (l->mode == (uint8_t)LANE_LED_BLINK
        && (l->blinkPeriodMs < LED_BLINK_PERIOD_MIN_MS || l->blinkPeriodMs > LED_BLINK_PERIOD_MAX_MS
            || l->blinkOnPermille < 1U || l->blinkOnPermille > 999U)) {
        return LANE_ERR_REG_VALUE;
    }
    return LANE_OK;
}

static laneStatus_e laneRegsValidateSchedule(const laneSchedule_t *s)
{
    if (s->op == (uint8_t)LANE_SCHED_CANCEL_ALL) {
        return LANE_OK;
    }
    if (s->op != (uint8_t)LANE_SCHED_ADD) {
        return LANE_ERR_REG_VALUE;
    }

    switch (s->action) {
    case LANE_ACT_LED_OFF:
    case LANE_ACT_SAMPLING_OFF:
    case LANE_ACT_SAMPLING_ON:
        return LANE_OK;
    case LANE_ACT_LED_ON:
        return (s->param <= LED_LEVEL_MAX) ? LANE_OK : LANE_ERR_REG_VALUE;
    case LANE_ACT_LED_BLINK:
        return (s->param >= LED_BLINK_PERIOD_MIN_MS && s->param <= LED_BLINK_PERIOD_MAX_MS)
               ? LANE_OK : LANE_ERR_REG_VALUE;
    case LANE_ACT_LED_PULSE:
        return ledPulseRiseValid(s->param) ? LANE_OK : LANE_ERR_REG_VALUE;
    default:
        return LANE_ERR_REG_VALUE;
    }
}

static laneStatus_e laneRegsValidateCommand(const laneCommand_t *c)
{
    switch (c->cmd) {
    case LANE_CMD_SAMPLING:
        return (c->arg <= (uint8_t)LANE_SAMPLING_RAW) ? LANE_OK : LANE_ERR_REG_VALUE;
    case LANE_CMD_SLEEP:
        if (c->arg <= (uint8_t)LANE_SLEEP_IDLE) {
            return LANE_OK;
        }
        return (c->arg <= (uint8_t)LANE_SLEEP_STANDBY) ? LANE_ERR_CMD_UNSUPPORTED : LANE_ERR_REG_VALUE;
    case LANE_CMD_CALIBRATE:
    case LANE_CMD_SINGLE_SAMPLE:
    case LANE_CMD_RELEASE_CHAIN:
    case LANE_CMD_RESET:
    case LANE_CMD_CLEAR_COUNTERS:
        return LANE_OK;
    case LANE_CMD_ENTER_BOOTLOADER:
        /* Reserved for the future I3C lane bootloader; an APP image refuses. */
        return LANE_ERR_CMD_UNSUPPORTED;
    default:
        return LANE_ERR_REG_VALUE;
    }
}

static laneStatus_e laneRegsValidateSyncAlign(const laneSyncAlign_t *s)
{
    if (s->periodUs < SYNC_TIME_PERIOD_MIN_US || s->periodUs > SYNC_TIME_PERIOD_MAX_US
        || s->phaseUs >= s->periodUs) {
        return LANE_ERR_REG_VALUE;
    }
    return LANE_OK;
}

static laneStatus_e laneRegsValidate(const laneRegsWrite_t *w)
{
    switch (w->reg) {
    case LANE_REG_LANE_ID:
        return (w->u.laneIndex.laneIndex <= LANE_MAX_LANES) ? LANE_OK : LANE_ERR_REG_VALUE;
    case LANE_REG_CONFIG:
        return laneRegsValidateConfig(&w->u.config);
    case LANE_REG_LED:
        return laneRegsValidateLed(&w->u.led);
    case LANE_REG_SCHEDULE:
        return laneRegsValidateSchedule(&w->u.schedule);
    case LANE_REG_COMMAND:
        return laneRegsValidateCommand(&w->u.command);
    case LANE_REG_SYNC_ALIGN:
        return laneRegsValidateSyncAlign(&w->u.syncAlign);
    default:
        return LANE_ERR_REG_READ_ONLY;
    }
}

laneStatus_e laneRegsDecode(laneRegs_t *r, const uint8_t *frame, uint32_t len,
                            laneRegsWrite_t *out)
{
    if (len == 0U) {
        return LANE_ERR_REG_LENGTH;
    }

    laneStatus_e status = LANE_OK;
    uint8_t      reg    = frame[0];
    uint32_t     size   = laneRegsWriteSize(reg);

    out->reg        = reg;
    out->payloadLen = 0U;

    if (!laneRegsIsKnown(reg)) {
        status = LANE_ERR_REG_UNKNOWN;
    } else if (len == 1U) {
        return LANE_OK;   /* read pointer only */
    } else if (size == 0U) {
        status = LANE_ERR_REG_READ_ONLY;
    } else if (len - 1U != size) {
        status = LANE_ERR_REG_LENGTH;
    } else {
        memcpy(&out->u, &frame[1], size);
        out->payloadLen = (uint8_t)size;
        status = laneRegsValidate(out);
    }

    if (status != LANE_OK) {
        laneRegsReject(r, status);
    }
    return status;
}
