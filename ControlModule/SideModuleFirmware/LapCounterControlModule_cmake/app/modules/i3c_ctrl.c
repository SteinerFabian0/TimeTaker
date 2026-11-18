/*
 * I3C1 controller. See i3c_ctrl.h.
 */

#include "i3c_ctrl.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "stm32_hal.h"
#include "mx_i3c1.h"
#include "mx_gpio_default.h"
#include "ctrl_status.h"
#include "irq_guard.h"

typedef struct {
    i3cCtrlXfer_t q[I3C_CTRL_QUEUE_DEPTH];
    uint8_t       head;
    uint8_t       count;
} i3cCtrlQueue_t;

typedef enum {
    I3C_CTRL_RESULT_NONE = 0,
    I3C_CTRL_RESULT_DONE,
    I3C_CTRL_RESULT_ERROR,
    I3C_CTRL_RESULT_ABORTED,
} i3cCtrlResult_e;

static hal_i3c_handle_t *i3cCtrlHandle;
static bool              i3cCtrlRunning;
static i3cCtrlQueue_t    i3cCtrlPrio;
static i3cCtrlQueue_t    i3cCtrlNormal;

/* The transfer in flight. The HAL keeps pointers into these until it is done. */
static bool                   i3cCtrlBusy;
static i3cCtrlXfer_t          i3cCtrlCur;
static uint8_t                i3cCtrlTx[1U + I3C_CTRL_WR_MAX];
static uint32_t               i3cCtrlTc[2];
static hal_i3c_transfer_ctx_t i3cCtrlCtx;
static uint32_t               i3cCtrlStartMs;
static bool                   i3cCtrlAborting;
static uint8_t                i3cCtrlErrStreak;
/* OP_READ with I3C_CTRL_READ_STOP_TURNAROUND: the pointer write is in flight
 * and its completion starts the read (ISR). */
static volatile bool          i3cCtrlReadPending;

/* Written by the ISR, read by the loop. */
static volatile uint8_t  i3cCtrlResult;
static volatile uint16_t i3cCtrlRxLen;
static volatile uint32_t i3cCtrlSer;
static volatile uint32_t i3cCtrlHotJoins;
static volatile uint8_t  i3cCtrlDaaTargets;

static i3cCtrlStats_t i3cCtrlStats;


/* --- Queues ---------------------------------------------------------------- */

static void i3cCtrlQueueClear(i3cCtrlQueue_t *q)
{
    q->head  = 0U;
    q->count = 0U;
}

static bool i3cCtrlQueuePush(i3cCtrlQueue_t *q, const i3cCtrlXfer_t *x)
{
    if (q->count >= I3C_CTRL_QUEUE_DEPTH) {
        return false;
    }
    q->q[(q->head + q->count) % I3C_CTRL_QUEUE_DEPTH] = *x;
    q->count++;
    return true;
}

static bool i3cCtrlQueuePop(i3cCtrlQueue_t *q, i3cCtrlXfer_t *x)
{
    if (q->count == 0U) {
        return false;
    }
    *x = q->q[q->head];
    q->head = (uint8_t)((q->head + 1U) % I3C_CTRL_QUEUE_DEPTH);
    q->count--;
    return true;
}


/* --- Bus recovery ------------------------------------------------------------ */

static void i3cCtrlDelayUs(uint32_t us)
{
    volatile uint32_t n = us * (SystemCoreClock / 4000000U);
    while (n > 0U) {
        n--;
    }
}

/* SDA held low by a target that lost a bit: clock SCL as GPIO until it lets
 * go (at most 9 clocks, about 10 µs each). The pins are I3C again after. */
static void i3cCtrlRecoverBus(void)
{
    i3cCtrlStats.recoveries++;
    mx_i3c1_deinit();

    LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_8, LL_GPIO_MODE_INPUT);
    LL_GPIO_SetOutputPin(GPIOA, LL_GPIO_PIN_9);
    LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_9, LL_GPIO_MODE_OUTPUT);
    for (uint8_t i = 0U; i < 9U && LL_GPIO_IsInputPinSet(GPIOA, LL_GPIO_PIN_8) == 0U; i++) {
        LL_GPIO_ResetOutputPin(GPIOA, LL_GPIO_PIN_9);
        i3cCtrlDelayUs(5U);
        LL_GPIO_SetOutputPin(GPIOA, LL_GPIO_PIN_9);
        i3cCtrlDelayUs(5U);
    }

    i3cCtrlHandle = mx_i3c1_init();   /* pins back to AF2 */
    if (i3cCtrlHandle != NULL) {
        (void)HAL_I3C_CTRL_ActivateNotification(i3cCtrlHandle, HAL_I3C_CTRL_NOTIFICATION_HJ
                                                                | HAL_I3C_CTRL_NOTIFICATION_IBI);
    } else {
        i3cCtrlRunning = false;
    }
}


/* --- Starting a transfer ------------------------------------------------------- */

/* The read half of OP_READ as its own frame (STOP turnaround). ISR or loop. */
static hal_status_t i3cCtrlStartRead(void)
{
    const i3cCtrlXfer_t *x = &i3cCtrlCur;

    (void)HAL_I3C_CTRL_ResetTransferCtx(&i3cCtrlCtx);
    i3cCtrlTc[1] = (uint32_t)x->rdLen | LL_I3C_DIRECTION_READ | ((uint32_t)x->addr << I3C_CR_ADD_Pos)
                   | LL_I3C_CONTROLLER_MTYPE_PRIVATE | LL_I3C_GENERATE_STOP;
    i3cCtrlCtx.p_tc_data     = &i3cCtrlTc[1];
    i3cCtrlCtx.tc_size_word  = 1U;
    i3cCtrlCtx.p_rx_data     = x->rd;
    i3cCtrlCtx.rx_size_byte  = x->rdLen;
    i3cCtrlCtx.transfer_mode = (hal_i3c_transfer_mode_t)LL_I3C_CONTROLLER_MTYPE_PRIVATE;
    return HAL_I3C_CTRL_Transfer_IT(i3cCtrlHandle, &i3cCtrlCtx);
}

static hal_status_t i3cCtrlLaunch(void)
{
    i3cCtrlXfer_t *x = &i3cCtrlCur;

    i3cCtrlReadPending = false;

    if (x->prepare != NULL) {
        x->prepare(x->wr, x->wrLen);
    }

    i3cCtrlResult     = I3C_CTRL_RESULT_NONE;
    i3cCtrlRxLen      = 0U;
    i3cCtrlDaaTargets = 0U;
    i3cCtrlStartMs    = HAL_GetTick();

    if (x->op == I3C_CTRL_OP_ENTDAA) {
        return HAL_I3C_CTRL_DynAddrAssign_IT(i3cCtrlHandle, HAL_I3C_DYN_ADDR_ONLY_ENTDAA);
    }

    (void)HAL_I3C_CTRL_ResetTransferCtx(&i3cCtrlCtx);
    const uint32_t addr = (uint32_t)x->addr << I3C_CR_ADD_Pos;

    if (x->op == I3C_CTRL_OP_CCC) {
        memcpy(i3cCtrlTx, x->wr, x->wrLen);
        i3cCtrlTc[0] = (uint32_t)x->wrLen | ((uint32_t)x->reg << I3C_CR_CCC_Pos)
                       | LL_I3C_CONTROLLER_MTYPE_CCC | LL_I3C_GENERATE_STOP;
        i3cCtrlCtx.p_tc_data     = i3cCtrlTc;
        i3cCtrlCtx.tc_size_word  = 1U;
        i3cCtrlCtx.p_tx_data     = (x->wrLen != 0U) ? i3cCtrlTx : NULL;
        i3cCtrlCtx.tx_size_byte  = x->wrLen;
        i3cCtrlCtx.nb_tx_frame   = 1U;
        i3cCtrlCtx.transfer_mode = HAL_I3C_CCC_BROADCAST_WITHOUT_DEFBYTE_STOP;
        return HAL_I3C_CTRL_Transfer_IT(i3cCtrlHandle, &i3cCtrlCtx);
    }

    /* Private: built by hand, see the HAL note in i3c_ctrl.h. */
    i3cCtrlTx[0] = x->reg;
    memcpy(&i3cCtrlTx[1], x->wr, x->wrLen);
    i3cCtrlCtx.p_tx_data     = i3cCtrlTx;
    i3cCtrlCtx.nb_tx_frame   = 1U;
    i3cCtrlCtx.p_tc_data     = i3cCtrlTc;
    i3cCtrlCtx.transfer_mode = (hal_i3c_transfer_mode_t)LL_I3C_CONTROLLER_MTYPE_PRIVATE;

    if (x->op == I3C_CTRL_OP_READ && I3C_CTRL_READ_STOP_TURNAROUND != 0U) {
        /* Pointer write as a frame of its own; its completion starts the
         * read. The lane serves a read after a STOP the same way. */
        i3cCtrlTc[0] = 1U | LL_I3C_DIRECTION_WRITE | addr | LL_I3C_CONTROLLER_MTYPE_PRIVATE
                       | LL_I3C_GENERATE_STOP;
        i3cCtrlCtx.tc_size_word = 1U;
        i3cCtrlCtx.tx_size_byte = 1U;
        i3cCtrlReadPending      = true;
    } else if (x->op == I3C_CTRL_OP_READ) {
        i3cCtrlTc[0] = 1U | LL_I3C_DIRECTION_WRITE | addr | LL_I3C_CONTROLLER_MTYPE_PRIVATE
                       | LL_I3C_GENERATE_RESTART;
        i3cCtrlTc[1] = (uint32_t)x->rdLen | LL_I3C_DIRECTION_READ | addr | LL_I3C_CONTROLLER_MTYPE_PRIVATE
                       | LL_I3C_GENERATE_STOP;
        i3cCtrlCtx.tc_size_word = 2U;
        i3cCtrlCtx.tx_size_byte = 1U;
        i3cCtrlCtx.p_rx_data    = x->rd;
        i3cCtrlCtx.rx_size_byte = x->rdLen;
    } else {
        i3cCtrlTc[0] = (1U + (uint32_t)x->wrLen) | LL_I3C_DIRECTION_WRITE | addr
                       | LL_I3C_CONTROLLER_MTYPE_PRIVATE | LL_I3C_GENERATE_STOP;
        i3cCtrlCtx.tc_size_word = 1U;
        i3cCtrlCtx.tx_size_byte = 1U + (uint32_t)x->wrLen;
    }
    return HAL_I3C_CTRL_Transfer_IT(i3cCtrlHandle, &i3cCtrlCtx);
}

static ctrlStatus_e i3cCtrlErrorStatus(uint32_t ser)
{
    if ((ser & LL_I3C_SER_ANACK) != 0U) {
        i3cCtrlStats.addrNacks++;
        return CTRL_ERR_LANE_ABSENT;
    }
    if ((ser & LL_I3C_SER_DNACK) != 0U) {
        i3cCtrlStats.dataNacks++;
        return CTRL_ERR_LANE_NACK;
    }
    return CTRL_ERR_I3C;
}

static void i3cCtrlFinish(ctrlStatus_e status, uint16_t rxLen)
{
    i3cCtrlBusy = false;
    i3cCtrlStats.xfers++;
    if (status != CTRL_OK) {
        i3cCtrlStats.errors++;
        i3cCtrlErrStreak++;
    } else {
        i3cCtrlErrStreak = 0U;
    }
    if (i3cCtrlCur.done != NULL) {
        i3cCtrlCur.done(i3cCtrlCur.tag, status, rxLen);
    }

    /* Many errors in a row on an idle bus with SDA low: a stuck target. */
    if (i3cCtrlErrStreak >= I3C_CTRL_RECOVER_AFTER && i3cCtrlRunning
        && LL_GPIO_IsInputPinSet(GPIOA, LL_GPIO_PIN_8) == 0U) {
        i3cCtrlErrStreak = 0U;
        i3cCtrlRecoverBus();
    }
}


/* --- API ------------------------------------------------------------------------ */

ctrlStatus_e i3cCtrlStart(void)
{
    if (i3cCtrlRunning) {
        return CTRL_OK;
    }
    i3cCtrlQueueClear(&i3cCtrlPrio);
    i3cCtrlQueueClear(&i3cCtrlNormal);
    i3cCtrlBusy      = false;
    i3cCtrlAborting  = false;
    i3cCtrlErrStreak = 0U;
    i3cCtrlHotJoins  = 0U;

    i3cCtrlHandle = mx_i3c1_init();
    if (i3cCtrlHandle == NULL) {
        return CTRL_ERR_I3C;
    }
    if (HAL_I3C_CTRL_ActivateNotification(i3cCtrlHandle, HAL_I3C_CTRL_NOTIFICATION_HJ
                                                         | HAL_I3C_CTRL_NOTIFICATION_IBI) != HAL_OK) {
        mx_i3c1_deinit();
        return CTRL_ERR_I3C;
    }
    i3cCtrlRunning = true;
    return CTRL_OK;
}

void i3cCtrlStop(void)
{
    if (!i3cCtrlRunning) {
        return;
    }
    i3cCtrlRunning = false;
    i3cCtrlBusy    = false;
    i3cCtrlQueueClear(&i3cCtrlPrio);
    i3cCtrlQueueClear(&i3cCtrlNormal);
    mx_i3c1_deinit();   /* NVIC off, peripheral reset, pins back to reset state */
}

bool i3cCtrlIsRunning(void)
{
    return i3cCtrlRunning;
}

ctrlStatus_e i3cCtrlSubmit(const i3cCtrlXfer_t *xfer, bool priority)
{
    if (!i3cCtrlRunning) {
        return CTRL_ERR_INVALID_STATE;
    }
    if (xfer->wrLen > I3C_CTRL_WR_MAX || (xfer->op == I3C_CTRL_OP_READ && (xfer->rd == NULL || xfer->rdLen == 0U))) {
        return CTRL_ERR_INVALID_ARG;
    }
    if (!i3cCtrlQueuePush(priority ? &i3cCtrlPrio : &i3cCtrlNormal, xfer)) {
        i3cCtrlStats.queueFull++;
        return CTRL_ERR_BUSY;
    }
    return CTRL_OK;
}

void i3cCtrlRun(void)
{
    if (!i3cCtrlRunning) {
        return;
    }

    if (i3cCtrlBusy) {
        const uint8_t result = i3cCtrlResult;
        if (result == I3C_CTRL_RESULT_DONE) {
            uint16_t n = i3cCtrlRxLen;
            if (i3cCtrlCur.op == I3C_CTRL_OP_ENTDAA) {
                n = i3cCtrlDaaTargets;
                if (n > 1U) {
                    i3cCtrlStats.daaMulti++;
                }
            } else if (i3cCtrlCur.op == I3C_CTRL_OP_READ && n < i3cCtrlCur.rdLen) {
                i3cCtrlStats.earlyEnds++;
            }
            i3cCtrlFinish(CTRL_OK, n);
        } else if (result == I3C_CTRL_RESULT_ERROR) {
            i3cCtrlStats.lastSer = i3cCtrlSer;
            /* ENTDAA that nobody answered ends with the 7E NACK (CE2): no joiner. */
            if (i3cCtrlCur.op == I3C_CTRL_OP_ENTDAA && i3cCtrlDaaTargets == 0U) {
                i3cCtrlFinish(CTRL_OK, 0U);
            } else {
                i3cCtrlFinish(i3cCtrlErrorStatus(i3cCtrlSer), 0U);
            }
        } else if (result == I3C_CTRL_RESULT_ABORTED) {
            i3cCtrlAborting = false;
            i3cCtrlStats.timeouts++;
            i3cCtrlFinish(CTRL_ERR_TIMEOUT, 0U);
        } else if (!i3cCtrlAborting && HAL_GetTick() - i3cCtrlStartMs > I3C_CTRL_XFER_TIMEOUT_MS) {
            i3cCtrlAborting = true;
            if (HAL_I3C_Abort_IT(i3cCtrlHandle) != HAL_OK) {
                i3cCtrlAborting = false;
                i3cCtrlStats.timeouts++;
                i3cCtrlFinish(CTRL_ERR_TIMEOUT, 0U);
            }
        } else if (i3cCtrlAborting && HAL_GetTick() - i3cCtrlStartMs > 4U * I3C_CTRL_XFER_TIMEOUT_MS) {
            /* Not even the abort finished: start the peripheral over. */
            i3cCtrlAborting = false;
            i3cCtrlStats.timeouts++;
            i3cCtrlFinish(CTRL_ERR_TIMEOUT, 0U);
            i3cCtrlRecoverBus();
        }
        if (i3cCtrlBusy || !i3cCtrlRunning) {
            return;
        }
    }

    if (i3cCtrlQueuePop(&i3cCtrlPrio, &i3cCtrlCur) || i3cCtrlQueuePop(&i3cCtrlNormal, &i3cCtrlCur)) {
        i3cCtrlBusy = true;
        if (i3cCtrlLaunch() != HAL_OK) {
            i3cCtrlFinish(CTRL_ERR_I3C, 0U);   /* HAL busy or refused: the queue goes on */
        }
    }
}

bool i3cCtrlIsIdle(void)
{
    return !i3cCtrlBusy && i3cCtrlPrio.count == 0U && i3cCtrlNormal.count == 0U;
}

uint32_t i3cCtrlTakeHotJoins(void)
{
    uint32_t guard = irqGuardEnter();
    uint32_t n = i3cCtrlHotJoins;
    i3cCtrlHotJoins = 0U;
    irqGuardExit(guard);
    return n;
}

const i3cCtrlStats_t *i3cCtrlGetStats(void)
{
    return &i3cCtrlStats;
}


/* --- HAL callbacks (ISR, prio 2) ----------------------------------------------- */

void HAL_I3C_CTRL_TransferCpltCallback(hal_i3c_handle_t *hi3c)
{
    if (i3cCtrlReadPending) {
        /* STOP turnaround: the pointer is written, now the read. The HAL is
         * idle again before this callback runs. */
        i3cCtrlReadPending = false;
        if (i3cCtrlStartRead() != HAL_OK) {
            i3cCtrlSer    = 0U;
            i3cCtrlResult = I3C_CTRL_RESULT_ERROR;
        }
        return;
    }
    /* SR describes the last message: the read, for OP_READ. */
    i3cCtrlRxLen  = (uint16_t)LL_I3C_GetXferDataCount(I3C1);
    i3cCtrlResult = I3C_CTRL_RESULT_DONE;
    (void)hi3c;
}

void HAL_I3C_CTRL_TgtReqDynAddrCallback(hal_i3c_handle_t *hi3c, uint64_t target_payload)
{
    (void)target_payload;   /* PID/BCR/DCR: identical on every lane, nothing to learn */
    const uint8_t addr = (uint8_t)(i3cCtrlCur.addr + i3cCtrlDaaTargets);
    i3cCtrlDaaTargets++;
    (void)HAL_I3C_CTRL_SetDynAddr(hi3c, addr);
}

void HAL_I3C_CTRL_DAACpltCallback(hal_i3c_handle_t *hi3c)
{
    (void)hi3c;
    i3cCtrlResult = I3C_CTRL_RESULT_DONE;
}

void HAL_I3C_ErrorCallback(hal_i3c_handle_t *hi3c)
{
    (void)hi3c;
    /* GET_LAST_ERRORS is off in the CubeMX config, so read SER directly; the
     * HAL cleared ERRF, SER may already be clear (then: generic ERR_I3C). */
    i3cCtrlSer         = LL_I3C_GetRegister_SER(I3C1);
    i3cCtrlReadPending = false;
    i3cCtrlResult      = I3C_CTRL_RESULT_ERROR;
}

void HAL_I3C_AbortCpltCallback(hal_i3c_handle_t *hi3c)
{
    (void)hi3c;
    i3cCtrlReadPending = false;
    i3cCtrlResult      = I3C_CTRL_RESULT_ABORTED;
}

void HAL_I3C_NotifyCallback(hal_i3c_handle_t *hi3c, uint32_t notify_id)
{
    (void)hi3c;
    if ((notify_id & HAL_I3C_CTRL_NOTIFICATION_HJ) != 0U) {
        i3cCtrlHotJoins++;
        i3cCtrlStats.hotJoins++;
    }
    if ((notify_id & HAL_I3C_CTRL_NOTIFICATION_IBI) != 0U) {
        i3cCtrlStats.ibis++;   /* never acknowledged in v1: counted only */
    }
}
