/*
 * I3C1 controller: the lane bus master (CONTROL_FIRMWARE_GUIDE §5.1).
 *
 * An asynchronous transfer queue over the C5 HAL. lane_manager is its only
 * user. Callers queue a transfer; i3cCtrlRun() starts one at a time from the
 * main loop; the I3C ISR (prio 2) only advances it and records the outcome;
 * the next i3cCtrlRun() calls the done callback in the main loop. Two queues:
 * PRIORITY (laneManagerSchedule(): start-sequence writes jump ahead, in their
 * own order) and NORMAL (polling, health, config). Depth I3C_CTRL_QUEUE_DEPTH
 * each.
 *
 *   OP_WRITE    private write [reg | payload]
 *   OP_READ     private write [reg], Sr, private read of rdLen (lane
 *               register read, lane_protocol.h). rxLen = bytes the lane
 *               actually sent: the target may end the read early (T-bit)
 *               and the controller's SR.XDCNT counts what arrived.
 *               With I3C_CTRL_READ_STOP_TURNAROUND = 1 the pointer write is
 *               its own frame (STOP) and the read a second one, started from
 *               the first one's completion: the bench fallback if the lane
 *               cannot preload its response within the ~10 µs Sr turnaround
 *               (the lane serves both the same way, lane_bus.h).
 *   OP_CCC      broadcast CCC (RSTDAA, ...) with an optional payload
 *   OP_ENTDAA   ENTDAA giving the address in xfer.addr to the target that
 *               answers. rxLen = targets that answered (0 = nobody, 1 =
 *               normal, >1 = more than one unaddressed lane answered: the
 *               extra ones got addr+1, addr+2, ...; lane_manager logs it).
 *
 * A `prepare` hook runs in the main loop immediately before the transfer
 * starts, on the payload about to go out: SYNC_ALIGN measures its (tick,
 * µs-since-edge) there, so nothing queued in front of it adds latency.
 *
 * Hot-join requests (HJF, acknowledged because CubeMX sets HJACK) are counted
 * in the ISR; lane_manager drains the count. IBIs are latched but, in v1,
 * never acknowledged: no DEVRx entry is configured, so the controller NACKs
 * them and the lanes are polled (§10 Q1: the C5 has 4 DEVRx, six lanes).
 *
 * HAL note (found 2026-11-17): HAL_I3C_PRIVATE_WITH_ARB_RESTART in
 * stm32c5xx_hal_i3c.h is defined as LL_I3C_GENERATE_STOP alone, i.e. message
 * type RELEASE with a STOP after every message, not "private, restart". So
 * the control words of private transfers are built here (MTYPE_PRIVATE,
 * RESTART between messages, STOP after the last) instead of with
 * HAL_I3C_CTRL_BuildTransferCtxPrivate().
 *
 * Errors: an error or a transfer that does not finish in
 * I3C_CTRL_XFER_TIMEOUT_MS is aborted, its done callback gets
 * CTRL_ERR_LANE_ABSENT (address NACK), CTRL_ERR_LANE_NACK (data NACK),
 * CTRL_ERR_TIMEOUT or CTRL_ERR_I3C, and the queue goes on. After
 * I3C_CTRL_RECOVER_AFTER errors in a row with SDA low on an idle bus, the
 * pins are taken back as GPIO and SCL is clocked up to 9 times (the hub's I2C
 * recovery, for I3C pins), then I3C1 is set up again.
 *
 * Owns: I3C1 and its EV/ERR IRQs, and PA8/PA9 between i3cCtrlStart() and
 * i3cCtrlStop() (lane_chain owns them while parked). Defines the HAL I3C
 * weak callbacks.
 * Context: API main loop only; callbacks ISR prio 2.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ctrl_status.h"

#define I3C_CTRL_QUEUE_DEPTH      16U
#define I3C_CTRL_WR_MAX           40U    /* payload after the register byte */
#define I3C_CTRL_XFER_TIMEOUT_MS  5U
#define I3C_CTRL_RECOVER_AFTER    3U
/* BENCH switch: 1 = STOP between the pointer write and the read of OP_READ
 * instead of a repeated START (see OP_READ). */
#define I3C_CTRL_READ_STOP_TURNAROUND  0U

typedef enum {
    I3C_CTRL_OP_WRITE = 0,
    I3C_CTRL_OP_READ,
    I3C_CTRL_OP_CCC,
    I3C_CTRL_OP_ENTDAA,
} i3cCtrlOp_e;

typedef void (*i3cCtrlDoneFn_t)(uint16_t tag, ctrlStatus_e status, uint16_t rxLen);
typedef void (*i3cCtrlPrepareFn_t)(uint8_t *payload, uint8_t len);

typedef struct {
    uint8_t            op;           /* i3cCtrlOp_e */
    uint8_t            addr;         /* target (or the address ENTDAA gives) */
    uint8_t            reg;          /* register pointer, or the CCC code */
    uint8_t            wrLen;        /* payload bytes in wr (after reg) */
    uint8_t            wr[I3C_CTRL_WR_MAX];
    uint8_t           *rd;           /* OP_READ: caller's buffer, alive until done */
    uint16_t           rdLen;
    uint16_t           tag;          /* the caller's own id, handed back to done */
    i3cCtrlDoneFn_t    done;         /* main loop; may be NULL */
    i3cCtrlPrepareFn_t prepare;      /* main loop, right before the start; may be NULL */
} i3cCtrlXfer_t;

typedef struct {
    uint32_t xfers;
    uint32_t errors;
    uint32_t addrNacks;
    uint32_t dataNacks;
    uint32_t timeouts;
    uint32_t earlyEnds;       /* reads the target ended before rdLen */
    uint32_t hotJoins;
    uint32_t ibis;
    uint32_t daaMulti;        /* ENTDAA answered by more than one target */
    uint32_t recoveries;
    uint32_t queueFull;
    uint32_t lastSer;         /* I3C SER of the last error */
} i3cCtrlStats_t;

/* Pins must already be released (laneChainReleaseBusPins()). Runs
 * mx_i3c1_init(), enables hot-join notifications. */
ctrlStatus_e i3cCtrlStart(void);

/* Aborts, drops both queues WITHOUT callbacks (the caller resets its own
 * state), and de-inits I3C1. Pins are left for lane_chain to park. */
void i3cCtrlStop(void);

bool i3cCtrlIsRunning(void);

/* Queues a transfer (copied). CTRL_ERR_BUSY when that queue is full,
 * CTRL_ERR_INVALID_STATE when stopped. */
ctrlStatus_e i3cCtrlSubmit(const i3cCtrlXfer_t *xfer, bool priority);

/* Every loop pass: delivers a finished transfer, times out a stuck one,
 * starts the next. */
void i3cCtrlRun(void);

/* Nothing queued and nothing in flight. */
bool i3cCtrlIsIdle(void);

/* Hot-join requests since the last call. */
uint32_t i3cCtrlTakeHotJoins(void);

const i3cCtrlStats_t *i3cCtrlGetStats(void);
