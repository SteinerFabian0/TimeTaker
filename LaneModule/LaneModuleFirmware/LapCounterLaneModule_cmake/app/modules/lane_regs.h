/*
 * Register-map rules of the lane bus: decoding and validating every write
 * from the control module (lane_protocol.h), the compiled CONFIG defaults,
 * and the sensor timing that bounds the sample period.
 *
 * Pure logic: no HAL, no globals, host-tested (test/host_regs). bus_manager
 * owns one laneRegs_t and calls it from the main loop on each received
 * frame; lane_bus never decodes anything in its ISR beyond the pointer byte.
 *
 * Rules (lane_protocol.h "Transport", guide §5):
 *   - a frame is [reg u8] [payload]; a frame of just [reg] sets the read
 *     pointer and is valid for every known register;
 *   - a payload must be exactly the register's struct; anything else is
 *     rejected as a whole (LANE_ERR_REG_LENGTH), never half-applied;
 *   - a payload to a read-only register is LANE_ERR_REG_READ_ONLY, an
 *     unknown register LANE_ERR_REG_UNKNOWN;
 *   - every field is range-checked (LANE_ERR_REG_VALUE). CONFIG is checked
 *     as a whole, including the period against the sensor's conversion time,
 *     and applied by the caller only after it passed: CONFIG is atomic;
 *   - COMMAND ENTER_BOOTLOADER is reserved and rejected by this APP image,
 *     as are sleep levels beyond IDLE (LANE_ERR_CMD_UNSUPPORTED);
 *   - every rejection counts in badWrites (saturating) and is remembered as
 *     the last reject; the caller also counts rejections it decides later
 *     (queue full, SYNC not locked) through laneRegsReject().
 *
 * Reserved fields are ignored, not checked: the control module zeroes them,
 * and a later minor version may give them meaning.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lane_status.h"
#include "lane_protocol.h"

/* ---------------------------------------------------------------------------
 * VCNL4040 timing in force mode. BENCH ITEMS: compiled defaults until the
 * bench measures them (BENCH_TESTS.md procedure 4). One trigger-to-data cycle
 * is: trigger write on I2C, conversion, data read on I2C, margin.
 * ------------------------------------------------------------------------- */
#define LANE_REGS_VCNL_T_US              125U   /* BENCH: 1T PS integration time        */
#define LANE_REGS_VCNL_OVERHEAD_US       100U   /* BENCH: per-conversion overhead        */
#define LANE_REGS_I2C_TRIGGER_US         90U    /* 4 bytes at 400 kHz, measured on bench */
#define LANE_REGS_I2C_READ_US            130U   /* 5 bytes + restart at 400 kHz          */
#define LANE_REGS_SAMPLE_MARGIN_US       100U

/* CONFIG ranges: LANE_CFG_* in lane_protocol.h (the controller checks the
 * same). The sensor-cycle floor of the sample period stays here
 * (laneRegsMinSamplePeriodUs). COMMAND SLEEP levels: LANE_SLEEP_* there too;
 * this image implements AWAKE and IDLE and rejects NAP and STANDBY. */

/* One decoded write. payloadLen 0 = pointer only. */
typedef struct {
    uint8_t reg;
    uint8_t payloadLen;
    union {
        laneIndex_t     laneIndex;
        laneConfig_t    config;
        laneLed_t       led;
        laneSchedule_t  schedule;
        laneCommand_t   command;
        laneSyncAlign_t syncAlign;
    } u;
} laneRegsWrite_t;

typedef struct {
    uint16_t     badWrites;    /* saturating */
    laneStatus_e lastReject;
} laneRegs_t;

void laneRegsInit(laneRegs_t *r);

/*
 * Decodes and validates one received frame. LANE_OK fills *out; any error
 * leaves *out unspecified and counts the rejection. A zero-length frame is
 * ignored (LANE_ERR_REG_LENGTH, not counted): it carries nothing to reject.
 */
laneStatus_e laneRegsDecode(laneRegs_t *r, const uint8_t *frame, uint32_t len,
                            laneRegsWrite_t *out);

/* Counts a rejection the caller decided after decoding. */
void laneRegsReject(laneRegs_t *r, laneStatus_e why);

uint16_t laneRegsBadWrites(const laneRegs_t *r);
void     laneRegsClearCounters(laneRegs_t *r);

/* Whether reg is in laneReg_e. */
bool laneRegsIsKnown(uint8_t reg);

/* Payload size of a write to reg; 0 for a read-only or unknown register. */
uint32_t laneRegsWriteSize(uint8_t reg);

/* The compiled CONFIG defaults (same values as the control module's table). */
void laneRegsDefaultConfig(laneConfig_t *c);

/* Validates a whole CONFIG (the rules above). */
laneStatus_e laneRegsValidateConfig(const laneConfig_t *c);

/* Sensor conversion time for the PS_IT / PS_MPS codes (bench constants). */
uint32_t laneRegsConversionUs(uint8_t psIntegration, uint8_t psMultiPulse);

/* Shortest sample period that fits trigger, conversion, read and margin. */
uint32_t laneRegsMinSamplePeriodUs(uint8_t psIntegration, uint8_t psMultiPulse);
