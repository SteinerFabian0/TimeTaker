/*
 * VCNL4040 driver on I2C1. See vcnl4040.h for ownership and the recovery
 * ladder.
 */

#include "vcnl4040.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "stm32_hal.h"
#include "mx_i2c1.h"
#include "lane_status.h"
#include "lane_timer.h"
#include "irq_guard.h"

/* Registers (command codes). */
#define VCNL_REG_ALS_CONF   0x00U
#define VCNL_REG_PS_CONF12  0x03U
#define VCNL_REG_PS_CONF3MS 0x04U
#define VCNL_REG_PS_DATA    0x08U
#define VCNL_REG_ID         0x0CU

/* Bits. BENCH: verify against the datasheet revision on the first board. */
#define VCNL_ALS_SD         0x01U        /* ALS_CONF                      */
#define VCNL_PS_SD          0x01U        /* PS_CONF1                      */
#define VCNL_PS_IT_SHIFT    1U           /* PS_CONF1 3:1                  */
#define VCNL_PS_HD          0x08U        /* PS_CONF2: 16-bit output       */
#define VCNL_PS_MPS_SHIFT   5U           /* PS_CONF3 6:5                  */
#define VCNL_PS_AF          0x08U        /* PS_CONF3: active force mode   */
#define VCNL_PS_TRIG        0x04U        /* PS_CONF3: one conversion      */
#define VCNL_WHITE_DIS      0x80U        /* PS_MS: white channel off      */
#define VCNL_LED_I_MASK     0x07U        /* PS_MS 2:0                     */

/* Bus clear: 9 SCL pulses at ~100 kHz. */
#define VCNL_BUS_CLEAR_PULSES   9U
#define VCNL_BUS_CLEAR_HALF_US  5U

#define VCNL_SCRIPT_MAX     5U

/* A transfer without a completion callback this long is dead (bus hung, ISR
 * lost): abandoned, bus cleared. One transfer is ~100-200 us at 400 kHz. */
#define VCNL_XFER_TIMEOUT_US 5000U

typedef struct {
    uint8_t reg;
    bool    read;
    uint8_t lo;
    uint8_t hi;
} vcnlStep_t;

typedef enum {
    VCNL_OP_NONE = 0,
    VCNL_OP_SCRIPT,
    VCNL_OP_SAMPLE,
} vcnlOp_e;

static volatile vcnlOp_e     vcnlOp;
static volatile bool         vcnlXferDone;
static volatile uint32_t     vcnlOpStartUs;
static volatile laneStatus_e vcnlXferStatus;
static vcnl4040DoneFn_t      vcnlSampleDone;
static bool                  vcnlSampleIsRead;

static uint8_t vcnlTx[2];
static uint8_t vcnlRx[2];
static uint8_t vcnlTrig[2];
static uint8_t vcnlData[2];

static vcnlStep_t vcnlScript[VCNL_SCRIPT_MAX];
static uint32_t   vcnlScriptLen;
static uint32_t   vcnlScriptIdx;
static uint32_t   vcnlRetries;
static bool       vcnlScriptActivates;   /* ends ACTIVE (else SHUTDOWN) */

static volatile vcnl4040State_e vcnlState;
static bool               vcnlWantActive;
static vcnl4040Settings_t vcnlWant;
static vcnl4040Settings_t vcnlApplied;
static bool               vcnlRecoveryWanted;
static bool               vcnlIdOk;
static uint16_t           vcnlLastId;
static uint32_t           vcnlFailedScripts;
static uint32_t           vcnlFaultSinceMs;

static volatile uint32_t vcnlErrors;
static volatile uint32_t vcnlSampleErrorRun;
static uint32_t          vcnlRecoveries;
static volatile uint32_t vcnlInts;

static bool vcnlSettingsDiffer(void);
static void vcnlSampleFinished(laneStatus_e status, uint16_t value);

static uint8_t vcnlConf3(const vcnl4040Settings_t *s)
{
    return (uint8_t)(((uint32_t)(s->psMultiPulse & 0x03U) << VCNL_PS_MPS_SHIFT) | VCNL_PS_AF);
}

static uint8_t vcnlMs(const vcnl4040Settings_t *s)
{
    return (uint8_t)(VCNL_WHITE_DIS | (s->irCurrent & VCNL_LED_I_MASK));
}

static uint8_t vcnlConf1(const vcnl4040Settings_t *s, bool shutdown)
{
    return (uint8_t)(((uint32_t)(s->psIntegration & 0x07U) << VCNL_PS_IT_SHIFT)
                     | (shutdown ? VCNL_PS_SD : 0U));
}

static void vcnlAddStep(uint8_t reg, bool read, uint8_t lo, uint8_t hi)
{
    if (vcnlScriptLen < VCNL_SCRIPT_MAX) {
        vcnlScript[vcnlScriptLen].reg  = reg;
        vcnlScript[vcnlScriptLen].read = read;
        vcnlScript[vcnlScriptLen].lo   = lo;
        vcnlScript[vcnlScriptLen].hi   = hi;
        vcnlScriptLen++;
    }
}

/* Builds the script for the wanted state, with an ID probe first if asked. */
static void vcnlBuildScript(bool probe)
{
    vcnlScriptLen = 0U;
    vcnlScriptIdx = 0U;
    vcnlRetries   = 0U;

    if (probe) {
        vcnlAddStep(VCNL_REG_ID, true, 0U, 0U);
    }

    vcnlScriptActivates = vcnlWantActive;
    if (vcnlWantActive) {
        vcnlApplied = vcnlWant;
        vcnlAddStep(VCNL_REG_ALS_CONF, false, VCNL_ALS_SD, 0U);
        vcnlAddStep(VCNL_REG_PS_CONF12, false, vcnlConf1(&vcnlWant, false), VCNL_PS_HD);
        vcnlAddStep(VCNL_REG_PS_CONF3MS, false, vcnlConf3(&vcnlWant), vcnlMs(&vcnlWant));
    } else {
        vcnlAddStep(VCNL_REG_PS_CONF12, false, vcnlConf1(&vcnlApplied, true), VCNL_PS_HD);
        vcnlAddStep(VCNL_REG_ALS_CONF, false, VCNL_ALS_SD, 0U);
    }
}

static void vcnlStartStep(void)
{
    hal_i2c_handle_t *h    = mx_i2c1_i2c_gethandle();
    const vcnlStep_t *step = &vcnlScript[vcnlScriptIdx];
    hal_status_t      hs;

    vcnlXferDone  = false;
    vcnlOp        = VCNL_OP_SCRIPT;
    vcnlOpStartUs = laneTimerNow();

    if (step->read) {
        hs = HAL_I2C_MASTER_MemRead_IT(h, VCNL4040_I2C_ADDR << 1, step->reg,
                                       HAL_I2C_MEM_ADDR_8BIT, vcnlRx, 2U);
    } else {
        vcnlTx[0] = step->lo;
        vcnlTx[1] = step->hi;
        hs = HAL_I2C_MASTER_MemWrite_IT(h, VCNL4040_I2C_ADDR << 1, step->reg,
                                        HAL_I2C_MEM_ADDR_8BIT, vcnlTx, 2U);
    }

    if (hs != HAL_OK) {
        vcnlXferStatus = laneStatusFromHalI2c(hs);
        vcnlXferDone   = true;
    }
}

static void vcnlDelayUs(uint32_t us)
{
    uint32_t start = laneTimerNow();
    while ((uint32_t)(laneTimerNow() - start) < us) {
    }
}

/* Frees a sensor that holds SDA low mid-byte: I2C1 off, up to 9 SCL pulses
 * until SDA is released, a STOP, I2C1 back on. About 100 µs, main loop only. */
static void vcnlBusClear(void)
{
    hal_gpio_config_t g;

    mx_i2c1_i2c_deinit();
    HAL_RCC_GPIOH_EnableClock();

    g.mode        = HAL_GPIO_MODE_OUTPUT;
    g.output_type = HAL_GPIO_OUTPUT_OPENDRAIN;
    g.pull        = HAL_GPIO_PULL_NO;
    g.speed       = HAL_GPIO_SPEED_FREQ_LOW;
    g.alternate   = HAL_GPIO_AF_0;
    g.init_state  = HAL_GPIO_PIN_SET;
    (void)HAL_GPIO_Init(SENS_SCL_PORT, SENS_SCL_PIN, &g);
    (void)HAL_GPIO_Init(SENS_SDA_PORT, SENS_SDA_PIN, &g);   /* released (high) */

    for (uint32_t i = 0U; i < VCNL_BUS_CLEAR_PULSES; i++) {
        if (HAL_GPIO_ReadPin(SENS_SDA_PORT, SENS_SDA_PIN) == HAL_GPIO_PIN_SET && i > 0U) {
            break;
        }
        HAL_GPIO_WritePin(SENS_SCL_PORT, SENS_SCL_PIN, HAL_GPIO_PIN_RESET);
        vcnlDelayUs(VCNL_BUS_CLEAR_HALF_US);
        HAL_GPIO_WritePin(SENS_SCL_PORT, SENS_SCL_PIN, HAL_GPIO_PIN_SET);
        vcnlDelayUs(VCNL_BUS_CLEAR_HALF_US);
    }

    /* STOP: SDA low -> high while SCL is high. */
    HAL_GPIO_WritePin(SENS_SDA_PORT, SENS_SDA_PIN, HAL_GPIO_PIN_RESET);
    vcnlDelayUs(VCNL_BUS_CLEAR_HALF_US);
    HAL_GPIO_WritePin(SENS_SDA_PORT, SENS_SDA_PIN, HAL_GPIO_PIN_SET);
    vcnlDelayUs(VCNL_BUS_CLEAR_HALF_US);

    (void)mx_i2c1_i2c_init();
    vcnlRecoveries++;
}

static laneStatus_e vcnlScriptFailed(uint32_t nowMs, laneStatus_e why)
{
    vcnlFailedScripts++;
    vcnlScriptLen = 0U;

    if (vcnlFailedScripts <= 1U && why != LANE_ERR_SENSOR_ID) {
        /* First failure: clear the bus and start over from the probe. */
        vcnlRecoveryWanted = true;
        vcnlState          = VCNL4040_STATE_BUSY;
    } else {
        vcnlState        = VCNL4040_STATE_FAULT;
        vcnlFaultSinceMs = nowMs;
    }
    return why;
}

laneStatus_e vcnl4040Init(void)
{
    vcnlOp             = VCNL_OP_NONE;
    vcnlState          = VCNL4040_STATE_RESET;
    vcnlWantActive     = false;
    vcnlRecoveryWanted = false;
    vcnlIdOk           = false;
    vcnlFailedScripts  = 0U;
    vcnlScriptLen      = 0U;
    vcnlWant.irCurrent     = 7U;
    vcnlWant.psIntegration = 3U;
    vcnlWant.psMultiPulse  = 0U;
    vcnlApplied        = vcnlWant;

    return (mx_i2c1_i2c_gethandle() != NULL) ? LANE_OK : LANE_ERR_NOT_INITIALIZED;
}

laneStatus_e vcnl4040Run(uint32_t nowMs)
{
    if (vcnlOp == VCNL_OP_SAMPLE) {
        /* The sampler has the bus; a sample that never completes is failed
         * here (its callback gets the error) and the bus is cleared. */
        uint32_t guard = irqGuardEnter();
        if (vcnlOp == VCNL_OP_SAMPLE
            && (uint32_t)(laneTimerNow() - vcnlOpStartUs) > VCNL_XFER_TIMEOUT_US) {
            vcnlSampleFinished(LANE_ERR_I2C_TIMEOUT, 0U);
            vcnlRecoveryWanted = true;
        }
        irqGuardExit(guard);
        return LANE_OK;
    }

    if (vcnlOp == VCNL_OP_SCRIPT) {
        bool timedOut = false;

        if (!vcnlXferDone) {
            if ((uint32_t)(laneTimerNow() - vcnlOpStartUs) <= VCNL_XFER_TIMEOUT_US) {
                return LANE_OK;
            }
            uint32_t guard = irqGuardEnter();
            if (!vcnlXferDone) {
                timedOut = true;       /* a late callback now finds vcnlOp idle */
            }
            irqGuardExit(guard);
            if (!timedOut) {
                return LANE_OK;
            }
        }
        vcnlOp = VCNL_OP_NONE;
        if (timedOut) {
            vcnlErrors++;
            return vcnlScriptFailed(nowMs, LANE_ERR_I2C_TIMEOUT);
        }

        if (vcnlXferStatus == LANE_OK) {
            const vcnlStep_t *step = &vcnlScript[vcnlScriptIdx];
            if (step->read && step->reg == VCNL_REG_ID) {
                vcnlLastId = (uint16_t)(vcnlRx[0] | ((uint16_t)vcnlRx[1] << 8));
                vcnlIdOk   = vcnlLastId == VCNL4040_ID;
                if (!vcnlIdOk) {
                    return vcnlScriptFailed(nowMs, LANE_ERR_SENSOR_ID);
                }
            }
            vcnlScriptIdx++;
            vcnlRetries = 0U;
        } else {
            vcnlErrors++;
            vcnlRetries++;
            if (vcnlRetries > VCNL4040_RETRIES) {
                return vcnlScriptFailed(nowMs, vcnlXferStatus);
            }
        }

        if (vcnlScriptIdx < vcnlScriptLen) {
            vcnlStartStep();
        } else {
            vcnlScriptLen      = 0U;
            vcnlFailedScripts  = 0U;
            vcnlSampleErrorRun = 0U;
            vcnlState = vcnlScriptActivates ? VCNL4040_STATE_ACTIVE : VCNL4040_STATE_SHUTDOWN;
        }
        return LANE_OK;
    }

    /* Bus idle: decide the next script. */
    bool probe   = false;
    bool recover = false;

    if (vcnlRecoveryWanted) {
        recover = true;
        probe   = true;
    } else if (vcnlState == VCNL4040_STATE_RESET) {
        probe = true;
    } else if (vcnlState == VCNL4040_STATE_FAULT) {
        if ((uint32_t)(nowMs - vcnlFaultSinceMs) < VCNL4040_REPROBE_MS) {
            return LANE_OK;
        }
        vcnlFailedScripts = 0U;
        recover = true;
        probe   = true;
    } else if (vcnlWantActive && vcnlState == VCNL4040_STATE_ACTIVE && vcnlSettingsDiffer()) {
        /* New settings while active. */
    } else if (vcnlWantActive == (vcnlState == VCNL4040_STATE_ACTIVE)) {
        return LANE_OK;   /* already there */
    }

    uint32_t guard = irqGuardEnter();
    bool     free  = vcnlOp == VCNL_OP_NONE;
    if (free) {
        vcnlOp    = VCNL_OP_SCRIPT;   /* claims the bus against the sampler */
        vcnlState = VCNL4040_STATE_BUSY;
    }
    irqGuardExit(guard);

    if (free) {
        /* Bus clear only after the claim: the sampler ISR cannot start a
         * transfer on the half-reconfigured pins. */
        if (recover) {
            vcnlRecoveryWanted = false;
            vcnlBusClear();
        }
        vcnlBuildScript(probe);
        vcnlStartStep();
    }
    return LANE_OK;
}

laneStatus_e vcnl4040Activate(const vcnl4040Settings_t *s)
{
    if (s == NULL) {
        return LANE_ERR_INVALID_ARG;
    }
    vcnlWant       = *s;
    vcnlWantActive = true;
    return LANE_OK;
}

void vcnl4040Shutdown(void)
{
    vcnlWantActive = false;
}

void vcnl4040RequestRecovery(void)
{
    vcnlRecoveryWanted = true;
}

vcnl4040State_e vcnl4040State(void)
{
    return vcnlState;
}

static bool vcnlSettingsDiffer(void)
{
    return vcnlApplied.irCurrent != vcnlWant.irCurrent
           || vcnlApplied.psIntegration != vcnlWant.psIntegration
           || vcnlApplied.psMultiPulse != vcnlWant.psMultiPulse;
}

bool vcnl4040Settled(void)
{
    return vcnlWantActive && vcnlState == VCNL4040_STATE_ACTIVE && !vcnlSettingsDiffer();
}

bool vcnl4040IsOk(void)
{
    return vcnlIdOk && vcnlState != VCNL4040_STATE_FAULT;
}

/* Claims the bus for one sample op. ISR-safe. */
static laneStatus_e vcnlClaimSample(vcnl4040DoneFn_t done, bool isRead)
{
    laneStatus_e status = LANE_OK;
    uint32_t     guard  = irqGuardEnter();

    if (vcnlState != VCNL4040_STATE_ACTIVE) {
        status = LANE_ERR_SENSOR_NOT_READY;
    } else if (vcnlOp != VCNL_OP_NONE) {
        status = LANE_ERR_I2C_BUSY;
    } else {
        vcnlOp           = VCNL_OP_SAMPLE;
        vcnlSampleDone   = done;
        vcnlSampleIsRead = isRead;
        vcnlOpStartUs    = laneTimerNow();
    }

    irqGuardExit(guard);
    return status;
}

static void vcnlSampleFinished(laneStatus_e status, uint16_t value)
{
    vcnl4040DoneFn_t done = vcnlSampleDone;

    if (status == LANE_OK) {
        vcnlSampleErrorRun = 0U;
    } else {
        vcnlErrors++;
        vcnlSampleErrorRun++;
    }
    vcnlOp = VCNL_OP_NONE;
    if (done != NULL) {
        done(status, value);
    }
}

laneStatus_e vcnl4040Trigger(vcnl4040DoneFn_t done)
{
    LANE_RETURN_IF_ERR(vcnlClaimSample(done, false));

    vcnlTrig[0] = (uint8_t)(vcnlConf3(&vcnlApplied) | VCNL_PS_TRIG);
    vcnlTrig[1] = vcnlMs(&vcnlApplied);

    hal_status_t hs = HAL_I2C_MASTER_MemWrite_IT(mx_i2c1_i2c_gethandle(), VCNL4040_I2C_ADDR << 1,
                                                 VCNL_REG_PS_CONF3MS, HAL_I2C_MEM_ADDR_8BIT,
                                                 vcnlTrig, 2U);
    if (hs != HAL_OK) {
        vcnlSampleDone = NULL;
        vcnlSampleFinished(laneStatusFromHalI2c(hs), 0U);
        return laneStatusFromHalI2c(hs);
    }
    return LANE_OK;
}

laneStatus_e vcnl4040ReadPs(vcnl4040DoneFn_t done)
{
    LANE_RETURN_IF_ERR(vcnlClaimSample(done, true));

    hal_status_t hs = HAL_I2C_MASTER_MemRead_IT(mx_i2c1_i2c_gethandle(), VCNL4040_I2C_ADDR << 1,
                                                VCNL_REG_PS_DATA, HAL_I2C_MEM_ADDR_8BIT,
                                                vcnlData, 2U);
    if (hs != HAL_OK) {
        vcnlSampleDone = NULL;
        vcnlSampleFinished(laneStatusFromHalI2c(hs), 0U);
        return laneStatusFromHalI2c(hs);
    }
    return LANE_OK;
}

void vcnl4040OnInt(void)
{
    vcnlInts++;
}

uint32_t vcnl4040ErrorCount(void)
{
    return vcnlErrors;
}

uint32_t vcnl4040RecoveryCount(void)
{
    return vcnlRecoveries;
}

uint32_t vcnl4040IntCount(void)
{
    return vcnlInts;
}

uint16_t vcnl4040LastId(void)
{
    return vcnlLastId;
}

uint32_t vcnl4040SampleErrorRun(void)
{
    return vcnlSampleErrorRun;
}


/* =========================================================================
 *                   HAL CALLBACKS (I2C1 ISR, priority 3)
 * ========================================================================= */

static void vcnlTransferEnded(laneStatus_e status)
{
    if (vcnlOp == VCNL_OP_SAMPLE) {
        uint16_t value = vcnlSampleIsRead ? (uint16_t)(vcnlData[0] | ((uint16_t)vcnlData[1] << 8)) : 0U;
        vcnlSampleFinished(status, value);
    } else if (vcnlOp == VCNL_OP_SCRIPT) {
        vcnlXferStatus = status;
        vcnlXferDone   = true;
    }
}

void HAL_I2C_MASTER_MemTxCpltCallback(hal_i2c_handle_t *hi2c)
{
    if (hi2c == mx_i2c1_i2c_gethandle()) {
        vcnlTransferEnded(LANE_OK);
    }
}

void HAL_I2C_MASTER_MemRxCpltCallback(hal_i2c_handle_t *hi2c)
{
    if (hi2c == mx_i2c1_i2c_gethandle()) {
        vcnlTransferEnded(LANE_OK);
    }
}

void HAL_I2C_ErrorCallback(hal_i2c_handle_t *hi2c)
{
    if (hi2c == mx_i2c1_i2c_gethandle()) {
        vcnlTransferEnded(LANE_ERR_I2C);
    }
}
