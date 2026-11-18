/*
 * The lane's I3C target (I3C1 on PA8/PA9): private transfers of the register
 * map in lane_protocol.h, hot-join, IBI.
 *
 * Owns I3C1 after mx_i3c1_init() configured it (target, ENTDAA payload, IBI
 * with 1 payload byte, hot-join enabled), its EV/ERR interrupts (priority 2)
 * and HAL_I3C_ErrorCallback. No other file may touch I3C1.
 *
 * Why not the HAL transfer API: the HAL target model is "one Transmit_IT or
 * Receive_IT of a known size at a time", and a frame of another size ends in
 * its error path. The register map needs both directions armed at once
 * (a pointer write, then a read of a size the lane chooses). So this module
 * installs its own event routine as the HAL handle's p_isr_func (the
 * generated I3C1_EV_IRQHandler calls it) and drives the FIFOs with LL. The
 * HAL stays in charge of init and of the ERR interrupt; its error path resets
 * p_isr_func, which HAL_I3C_ErrorCallback (here) installs again.
 *
 * Transfers (lane_protocol.h "Transport"):
 *   - write [reg][payload]: bytes collect in the ISR; at frame end (FC) the
 *     first byte becomes the read pointer and the whole frame goes into a
 *     mailbox for the main loop (bus_manager decodes it with lane_regs), with
 *     the TIM2 time it arrived (SYNC_ALIGN needs it). A full mailbox drops
 *     the frame and counts it.
 *   - read: the response for the pointer is built at once in the ISR by
 *     bus_manager's builder and preloaded into the TX FIFO with an exact byte
 *     count (TGTTDR PRELOAD), so the lane ends the read early (T-bit) after
 *     the last byte; no zero padding is needed. At frame end the bytes that
 *     were clocked out (SR.XDCNT) go to the read-done hook, which pops FIFO
 *     records. The response for the same pointer is then built again, so a
 *     controller may also read again without rewriting the pointer.
 *   - The turnaround between the pointer write and the read (Sr + address)
 *     is about 10 µs at 1 MHz: the response is built from snapshots in a few
 *     µs. BENCH: if the first byte is late, the controller can put a STOP
 *     between pointer write and read; the lane behaves the same.
 *   - Reads of a write-only register return nothing (0 bytes preloaded).
 *   - GET/STATUS CCCs are answered by the hardware; a frame end flagged by
 *     them is not taken for a private read (BENCH: confirm no FIFO pop).
 *   - A read during which the ERR interrupt fired pops nothing: the
 *     controller saw the error too and re-reads (it drops repeated seqs).
 *   - ENTDAA: the peripheral answers it in hardware whenever it is enabled
 *     and has no address. The target is therefore disabled (CFGR.EN = 0)
 *     from init until bus_manager enables it to hot-join, and disabled again
 *     when the address goes (lane_protocol.h: no ENTDAA while CHAIN_IN is
 *     low). BENCH: a disabled target must leave SDA/SCL released.
 *
 * Max read length is set to LANE_BUS_MRL at init (CubeMX has 64, too short
 * for RAW_FIFO's 80 bytes and EVENT_FIFO's 68): see the guide's CubeMX list.
 *
 * Low power: an I3C target wakes from Stop only on a reset pattern or a
 * missed start; the lane never enters Stop in v1.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lane_status.h"

#define LANE_BUS_RX_MAX     48U     /* longest write: CONFIG, 1 + 36 bytes  */
#define LANE_BUS_TX_MAX     96U     /* longest read: RAW_FIFO, 80 bytes     */
#define LANE_BUS_MAILBOX    8U
#define LANE_BUS_MRL        96U
#define LANE_BUS_MWL        64U

/* laneBusTakeEvents() bits */
#define LANE_BUS_EV_ADDRESS       0x01U   /* ENTDAA gave us an address           */
#define LANE_BUS_EV_ADDRESS_LOST  0x02U   /* RSTDAA (or our own release)         */
#define LANE_BUS_EV_ERROR         0x04U   /* the ERR interrupt fired             */
#define LANE_BUS_EV_IBI_DONE      0x08U   /* an IBI was accepted by the controller */

typedef struct {
    uint32_t rxLocal;                  /* TIM2 at frame end                  */
    uint8_t  len;
    bool     overflow;                 /* longer than LANE_BUS_RX_MAX        */
    uint8_t  data[LANE_BUS_RX_MAX];
} laneBusFrame_t;

/* Builds the read response for reg into buf (ISR, priority 2); returns the
 * byte count, *offered gets the FIFO records/samples it contains. */
typedef uint32_t (*laneBusBuildFn_t)(uint8_t reg, uint8_t *buf, uint32_t cap, uint8_t *offered);

/* A read of reg ended after clockedOut bytes (ISR, priority 2). */
typedef void (*laneBusReadDoneFn_t)(uint8_t reg, uint8_t offered, uint32_t clockedOut);

typedef struct {
    uint32_t writes;
    uint32_t reads;
    uint32_t rxDropped;      /* mailbox full                                 */
    uint32_t rxOverflow;     /* frame longer than LANE_BUS_RX_MAX            */
    uint32_t errors;
    uint32_t lastErrorCodes; /* HAL last_error_codes (SER bits) of the last error */
    uint32_t hotJoins;       /* requests sent                                */
    uint32_t ibis;           /* requests sent                                */
    uint32_t cccFrames;      /* frame ends skipped as CCC traffic            */
    uint32_t resets;         /* RSTACT / reset pattern seen                  */
    uint32_t cutReads;       /* reads ended after an ERR: nothing popped     */
    uint32_t mergedFrames;   /* BENCH: pointer write seen inside a read frame */
    uint32_t enables;        /* target enabled (to join ENTDAA)              */
} laneBusStats_t;

/* Takes over I3C1 from the generated init: MRL, event routine, interrupts. */
laneStatus_e laneBusInit(laneBusBuildFn_t build, laneBusReadDoneFn_t done);

/* Next received write frame, oldest first (main loop). */
bool laneBusPopFrame(laneBusFrame_t *out);

/* LANE_BUS_EV_* bits since the last call (main loop). */
uint32_t laneBusTakeEvents(void);

bool    laneBusHasAddress(void);
uint8_t laneBusAddress(void);

/* Enables or disables the I3C target (CFGR.EN). An enabled target without
 * an address answers every ENTDAA in hardware, so bus_manager keeps it off
 * until chain_logic says this lane is the one to join. Disabling is ignored
 * while the lane has an address. */
void laneBusSetTargetEnabled(bool on);

/* Asks the controller for an address. LANE_ERR_INVALID_STATE with an
 * address, LANE_ERR_HOTJOIN when the target or hot-join is disabled. */
laneStatus_e laneBusRequestHotJoin(void);

/* The controller enabled IBIs (ENEC) and none is in flight. */
bool laneBusIbiReady(void);

/* Raises an IBI with one payload byte (LANE_IBI_*). */
laneStatus_e laneBusRequestIbi(uint8_t payload);

/* Drops the dynamic address (RELEASE_CHAIN, CHAIN_IN fell). */
void laneBusReleaseAddress(void);

/* HAL tick of the last frame addressed to us. */
uint32_t laneBusLastTrafficMs(void);

void laneBusGetStats(laneBusStats_t *out);
