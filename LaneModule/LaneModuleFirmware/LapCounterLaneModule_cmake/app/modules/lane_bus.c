/*
 * I3C target. See lane_bus.h for why the data path bypasses the HAL's
 * transfer API, and for the transfer rules.
 *
 * BENCH (BENCH_TESTS.md procedures 8-10): every register-level assumption
 * here is from the reference manual's description and the HAL source, not a
 * board: FC at a repeated START, XDCNT counting bytes actually clocked out on
 * a target read, PRELOAD ending the read with the T-bit, and the CCC flags
 * arriving no later than their FC.
 */

#include "lane_bus.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "stm32_hal.h"
#include "mx_i3c1.h"
#include "lane_protocol.h"
#include "lane_timer.h"
#include "irq_guard.h"

/* Everything but TXFNF, which is on only while bytes wait for the FIFO. */
#define LANE_BUS_IT_BASE  (LL_I3C_IER_RXFNEIE | LL_I3C_IER_FCIE | LL_I3C_IER_ERRIE   \
                           | LL_I3C_IER_DAUPDIE | LL_I3C_IER_IBIENDIE                 \
                           | LL_I3C_IER_GETIE | LL_I3C_IER_STAIE                      \
                           | LL_I3C_IER_INTUPDIE | LL_I3C_IER_RSTIE)

static laneBusBuildFn_t    laneBusBuild;
static laneBusReadDoneFn_t laneBusDone;

/* Frame being received. */
static uint8_t  laneBusRx[LANE_BUS_RX_MAX];
static uint32_t laneBusRxLen;
static bool     laneBusRxOverflow;

/* Preloaded read response. */
static uint8_t  laneBusTx[LANE_BUS_TX_MAX];
static uint32_t laneBusTxLen;
static uint32_t laneBusTxPos;
static uint8_t  laneBusTxOffered;
static uint8_t  laneBusTxReg = LANE_REG_STATUS;   /* register the preload was built for */
static uint8_t  laneBusPointer = LANE_REG_STATUS;

static bool laneBusCccSeen;
static bool laneBusErrSeen;     /* the ERR interrupt fired since the last frame end */

/* Mailbox, ISR -> main loop. */
static laneBusFrame_t    laneBusBox[LANE_BUS_MAILBOX];
static volatile uint32_t laneBusBoxHead;
static volatile uint32_t laneBusBoxTail;

static volatile uint32_t laneBusEvents;
static volatile bool     laneBusAddressed;
static volatile uint8_t  laneBusDynAddr;
static volatile bool     laneBusIbiInFlight;
static volatile uint32_t laneBusTrafficMs;

static volatile laneBusStats_t laneBusStatsData;

static hal_status_t laneBusIsr(hal_i3c_handle_t *hi3c, uint32_t itMasks);

/* Keeps the TX FIFO topped up from the response buffer. */
static void laneBusFillTx(void)
{
    while (laneBusTxPos < laneBusTxLen && LL_I3C_IsActiveFlag_TXFNF(I3C1) != 0U) {
        LL_I3C_TransmitData8(I3C1, laneBusTx[laneBusTxPos]);
        laneBusTxPos++;
    }
    if (laneBusTxPos < laneBusTxLen) {
        LL_I3C_EnableIT(I3C1, LL_I3C_IER_TXFNFIE);
    } else {
        LL_I3C_DisableIT(I3C1, LL_I3C_IER_TXFNFIE);
    }
}

/* Builds and preloads the response for the current pointer. ISR context. */
static void laneBusPrepare(void)
{
    LL_I3C_DisableIT(I3C1, LL_I3C_IER_TXFNFIE);
    LL_I3C_RequestTxFIFOFlush(I3C1);

    laneBusTxOffered = 0U;
    laneBusTxReg     = laneBusPointer;
    laneBusTxLen     = (laneBusBuild != NULL)
                       ? laneBusBuild(laneBusPointer, laneBusTx, sizeof(laneBusTx), &laneBusTxOffered)
                       : 0U;
    laneBusTxPos = 0U;

    if (laneBusTxLen > 0U) {
        LL_I3C_ConfigTxPreload(I3C1, (uint16_t)laneBusTxLen);
        laneBusFillTx();
    }
}

static void laneBusDrainRx(void)
{
    while (LL_I3C_IsActiveFlag_RXFNE(I3C1) != 0U) {
        uint8_t b = LL_I3C_ReceiveData8(I3C1);
        if (laneBusRxLen < LANE_BUS_RX_MAX) {
            laneBusRx[laneBusRxLen] = b;
            laneBusRxLen++;
        } else {
            laneBusRxOverflow = true;
        }
    }
}

static void laneBusPostFrame(void)
{
    uint32_t tail = laneBusBoxTail;
    uint32_t next = (tail + 1U) % LANE_BUS_MAILBOX;

    if (next == laneBusBoxHead) {
        laneBusStatsData.rxDropped++;
        return;
    }

    laneBusFrame_t *f = &laneBusBox[tail];
    f->rxLocal  = laneTimerNow();
    f->len      = (uint8_t)laneBusRxLen;
    f->overflow = laneBusRxOverflow;
    memcpy(f->data, laneBusRx, laneBusRxLen);
    __DMB();
    laneBusBoxTail = next;
}

/* The bytes of a write message become the pointer and a mailbox frame. */
static void laneBusTakeWrite(void)
{
    laneBusStatsData.writes++;
    if (laneBusRxOverflow) {
        laneBusStatsData.rxOverflow++;
    }
    laneBusPointer = laneBusRx[0];
    laneBusPostFrame();
    laneBusRxLen      = 0U;
    laneBusRxOverflow = false;
}

static void laneBusFrameEnd(void)
{
    const bool errSeen = laneBusErrSeen;
    laneBusErrSeen   = false;
    laneBusTrafficMs = HAL_GetTick();

    if (LL_I3C_GetMessageDirection(I3C1) == LL_I3C_MESSAGE_DIRECTION_WRITE) {
        laneBusDrainRx();
        if (laneBusRxLen > 0U) {
            laneBusTakeWrite();
            laneBusPrepare();
        }
        laneBusRxLen      = 0U;
        laneBusRxOverflow = false;
        laneBusCccSeen    = false;
        return;
    }

    if (laneBusCccSeen) {
        /* A GET CCC answered by the hardware, not our private read. */
        laneBusCccSeen = false;
        laneBusStatsData.cccFrames++;
        return;
    }

    uint32_t clocked = LL_I3C_GetXferDataCount(I3C1);
    laneBusStatsData.reads++;
    if (errSeen) {
        /* A read cut by a bus error pops nothing (lane_protocol.h): the
         * controller saw an error and keeps nothing of it either. */
        laneBusStatsData.cutReads++;
    } else if (laneBusDone != NULL) {
        laneBusDone(laneBusTxReg, laneBusTxOffered, clocked);
    }

    laneBusDrainRx();
    if (laneBusRxLen > 0U) {
        /* BENCH: no FC at the repeated START, so the pointer write arrived
         * inside this read frame and the read was served from the old
         * pointer. Keep the write (and the pointer for the next read); the
         * controller's STOP turnaround (i3c_ctrl) avoids this case. */
        laneBusStatsData.mergedFrames++;
        laneBusTakeWrite();
    }
    laneBusPrepare();
}

static hal_status_t laneBusIsr(hal_i3c_handle_t *hi3c, uint32_t itMasks)
{
    (void)hi3c;

    if ((itMasks & LL_I3C_MISR_RXFNEMIS) != 0U) {
        laneBusDrainRx();
    }
    if ((itMasks & LL_I3C_MISR_TXFNFMIS) != 0U) {
        laneBusFillTx();
    }
    if ((itMasks & (LL_I3C_MISR_GETMIS | LL_I3C_MISR_STAMIS)) != 0U) {
        LL_I3C_ClearFlag_GET(I3C1);
        LL_I3C_ClearFlag_STA(I3C1);
        laneBusCccSeen = true;
    }
    if ((itMasks & LL_I3C_MISR_FCMIS) != 0U) {
        LL_I3C_ClearFlag_FC(I3C1);
        laneBusFrameEnd();
    }
    if ((itMasks & LL_I3C_MISR_DAUPDMIS) != 0U) {
        LL_I3C_ClearFlag_DAUPD(I3C1);
        if (LL_I3C_IsEnabledOwnDynAddress(I3C1) != 0U) {
            laneBusDynAddr   = LL_I3C_GetOwnDynamicAddress(I3C1);
            laneBusAddressed = true;
            laneBusEvents   |= LANE_BUS_EV_ADDRESS;
            laneBusPointer   = LANE_REG_STATUS;
            laneBusPrepare();
        } else {
            laneBusAddressed = false;
            laneBusEvents   |= LANE_BUS_EV_ADDRESS_LOST;
        }
    }
    if ((itMasks & LL_I3C_MISR_IBIENDMIS) != 0U) {
        LL_I3C_ClearFlag_IBIEND(I3C1);
        laneBusIbiInFlight = false;
        laneBusEvents     |= LANE_BUS_EV_IBI_DONE;
    }
    if ((itMasks & LL_I3C_MISR_INTUPDMIS) != 0U) {
        LL_I3C_ClearFlag_INTUPD(I3C1);   /* ENEC/DISEC: read back via IBIEN */
    }
    if ((itMasks & LL_I3C_MISR_RSTMIS) != 0U) {
        LL_I3C_ClearFlag_RST(I3C1);
        laneBusStatsData.resets++;
    }
    return HAL_OK;
}

laneStatus_e laneBusInit(laneBusBuildFn_t build, laneBusReadDoneFn_t done)
{
    hal_i3c_handle_t *h = mx_i3c1_gethandle();

    laneBusBuild = build;
    laneBusDone  = done;
    laneBusBoxHead = 0U;
    laneBusBoxTail = 0U;
    laneBusRxLen   = 0U;
    memset((void *)&laneBusStatsData, 0, sizeof(laneBusStatsData));

    /* EVENT_FIFO (68 B) and RAW_FIFO (80 B) reads exceed the CubeMX MRL 64. */
    if (HAL_I3C_TGT_SetConfigMaxDataSize(h, LANE_BUS_MRL, LANE_BUS_MWL) != HAL_OK) {
        return LANE_ERR_HAL;
    }

    laneBusAddressed = LL_I3C_IsEnabledOwnDynAddress(I3C1) != 0U;

    uint32_t guard = irqGuardEnter();
    h->p_isr_func = laneBusIsr;
    LL_I3C_EnableIT(I3C1, LANE_BUS_IT_BASE);
    if (!laneBusAddressed) {
        /* mx_i3c1_init() and the MRL call above left the target enabled; it
         * stays off until chain_logic lets it answer ENTDAA. */
        LL_I3C_Disable(I3C1);
    }
    irqGuardExit(guard);

    return LANE_OK;
}

bool laneBusPopFrame(laneBusFrame_t *out)
{
    uint32_t head = laneBusBoxHead;
    if (head == laneBusBoxTail) {
        return false;
    }
    __DMB();
    *out = laneBusBox[head];
    laneBusBoxHead = (head + 1U) % LANE_BUS_MAILBOX;
    return true;
}

uint32_t laneBusTakeEvents(void)
{
    uint32_t guard = irqGuardEnter();
    uint32_t ev    = laneBusEvents;
    laneBusEvents  = 0U;
    irqGuardExit(guard);
    return ev;
}

bool laneBusHasAddress(void)
{
    return laneBusAddressed;
}

uint8_t laneBusAddress(void)
{
    return laneBusAddressed ? laneBusDynAddr : 0U;
}

void laneBusSetTargetEnabled(bool on)
{
    uint32_t guard = irqGuardEnter();
    const bool enabled = LL_I3C_IsEnabled(I3C1) != 0U;

    if (on && !enabled) {
        laneBusRxLen      = 0U;
        laneBusRxOverflow = false;
        laneBusCccSeen    = false;
        laneBusErrSeen    = false;
        LL_I3C_Enable(I3C1);
        laneBusStatsData.enables++;
    } else if (!on && enabled && !laneBusAddressed && LL_I3C_IsEnabledOwnDynAddress(I3C1) == 0U) {
        LL_I3C_Disable(I3C1);
    }
    irqGuardExit(guard);
}

laneStatus_e laneBusRequestHotJoin(void)
{
    if (LL_I3C_IsEnabled(I3C1) == 0U) {
        return LANE_ERR_HOTJOIN;
    }
    if (laneBusAddressed || LL_I3C_IsEnabledOwnDynAddress(I3C1) != 0U) {
        return LANE_ERR_INVALID_STATE;
    }
    if (LL_I3C_IsEnabledHotJoin(I3C1) == 0U) {
        return LANE_ERR_HOTJOIN;
    }
    LL_I3C_TargetHandleMessage(I3C1, LL_I3C_TARGET_MTYPE_HOT_JOIN, 0U);
    laneBusStatsData.hotJoins++;
    return LANE_OK;
}

bool laneBusIbiReady(void)
{
    return laneBusAddressed && !laneBusIbiInFlight && LL_I3C_IsEnabledIBI(I3C1) != 0U;
}

laneStatus_e laneBusRequestIbi(uint8_t payload)
{
    if (!laneBusIbiReady()) {
        return LANE_ERR_IBI;
    }
    laneBusIbiInFlight = true;
    LL_I3C_SetIBIPayload(I3C1, payload);
    LL_I3C_TargetHandleMessage(I3C1, LL_I3C_TARGET_MTYPE_IBI, 1U);
    laneBusStatsData.ibis++;
    return LANE_OK;
}

void laneBusReleaseAddress(void)
{
    uint32_t guard = irqGuardEnter();
    LL_I3C_DisableOwnDynAddress(I3C1);
    laneBusAddressed   = false;
    laneBusIbiInFlight = false;
    irqGuardExit(guard);
}

uint32_t laneBusLastTrafficMs(void)
{
    return laneBusTrafficMs;
}

void laneBusGetStats(laneBusStats_t *out)
{
    uint32_t guard = irqGuardEnter();
    memcpy(out, (const void *)&laneBusStatsData, sizeof(*out));
    irqGuardExit(guard);
}


/* =========================================================================
 *               HAL CALLBACK (I3C1 ERR ISR, priority 2)
 *
 * HAL_I3C_ERR_IRQHandler has already flushed the FIFOs, disabled the
 * transfer interrupts and set p_isr_func back to its own default: restore
 * ours and re-arm the response, so the target FIFO never stalls (guide §7).
 * ========================================================================= */

void HAL_I3C_ErrorCallback(hal_i3c_handle_t *hi3c)
{
    if (hi3c != mx_i3c1_gethandle()) {
        return;
    }

    laneBusStatsData.errors++;
    laneBusStatsData.lastErrorCodes = hi3c->last_error_codes;
    hi3c->last_error_codes = HAL_I3C_ERROR_NONE;

    laneBusRxLen       = 0U;
    laneBusRxOverflow  = false;
    laneBusCccSeen     = false;
    laneBusErrSeen     = true;
    laneBusIbiInFlight = false;
    laneBusEvents     |= LANE_BUS_EV_ERROR;

    hi3c->p_isr_func = laneBusIsr;
    LL_I3C_EnableIT(I3C1, LANE_BUS_IT_BASE);
    if (laneBusAddressed) {
        laneBusPrepare();
    }
}
