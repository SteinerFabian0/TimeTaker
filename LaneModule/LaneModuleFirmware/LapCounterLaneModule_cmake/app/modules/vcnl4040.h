/*
 * VCNL4040 proximity sensor driver (U2, I2C1 on PH0/PH1, 7-bit address 0x60).
 *
 * Owns I2C1, its pins during bus recovery, and the HAL I2C callbacks
 * (HAL_I2C_MASTER_MemTxCpltCallback, ..._MemRxCpltCallback,
 * HAL_I2C_ErrorCallback): no other file may define them.
 *
 * Interrupt-driven, never blocking (guide §3.4): every transfer is a HAL _IT
 * call; I2C1 EV/ERR run at priority 3.
 *   - Housekeeping (probe, configure, shut down, recover) is a script of
 *     register transfers that vcnl4040Run() advances from the main loop.
 *     sense_manager states what it wants (vcnl4040Activate / ..Shutdown);
 *     the driver gets there when the bus is free.
 *   - Sampling (vcnl4040Trigger, vcnl4040ReadPs) is started by lane_sampler
 *     from the TIM2 ISR and completes in the I2C ISR through a callback. Only
 *     allowed while ACTIVE and no script runs; sense_manager stops the
 *     sampler before it changes settings.
 *
 * Force mode (PS_AF = 1): the sensor converts once per PS_TRIG write, so the
 * MCU decides the sample instant. PS_INT is off: SENS_INT (EXTI0) stays high
 * and is only counted (a watch mode with the sensor's own threshold INT is a
 * later extension, guide §3.8).
 *
 * Register map (datasheet; BENCH: verify the bit positions marked below on
 * the first board with procedure 3 of BENCH_TESTS.md):
 *   0x00 ALS_CONF  L: ALS_SD bit 0                       (ALS kept shut down)
 *   0x03 PS_CONF1  L: PS_DUTY 7:6, PS_PERS 5:4, PS_IT 3:1, PS_SD 0
 *        PS_CONF2  H: PS_HD bit 3 (16-bit output), PS_INT 1:0 (off)
 *   0x04 PS_CONF3  L: PS_MPS 6:5, PS_SMART_PERS 4, PS_AF 3, PS_TRIG 2
 *        PS_MS     H: WHITE_EN 7 (1 = white off), PS_MS 6, LED_I 2:0
 *   0x08 PS_DATA   16 bit, little-endian
 *   0x0C ID        0x0186
 *
 * Recovery ladder (guide §7): a failed housekeeping transfer is retried
 * VCNL4040_RETRIES times, then the bus is cleared (up to 9 SCL pulses with
 * I2C1 deinitialised, a STOP) and I2C1 re-initialised, and the script starts
 * over from the ID probe. VCNL4040_SAMPLE_ERRORS_MAX failed samples in a row
 * also trigger that recovery (sense_manager asks). Repeated failure = FAULT,
 * re-probed every VCNL4040_REPROBE_MS; STATUS shows SENSOR_OK clear.
 *
 * Low power: the sensor is in shutdown whenever sampling is OFF (PS_SD and
 * ALS_SD), which is IDLE sleep. I2C1 needs PSIK after Stop1 (hub lesson).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lane_status.h"

#define VCNL4040_I2C_ADDR          0x60U
#define VCNL4040_ID                0x0186U
#define VCNL4040_RETRIES           3U
#define VCNL4040_REPROBE_MS        1000U
#define VCNL4040_SAMPLE_ERRORS_MAX 20U
/* 16-bit output (PS_HD). */
#define VCNL4040_FULL_SCALE        65535U

typedef enum {
    VCNL4040_STATE_RESET = 0,     /* nothing known yet                        */
    VCNL4040_STATE_BUSY,          /* a housekeeping script is running         */
    VCNL4040_STATE_SHUTDOWN,      /* ID ok, PS and ALS shut down              */
    VCNL4040_STATE_ACTIVE,        /* PS on in force mode: sampling allowed    */
    VCNL4040_STATE_FAULT,         /* not answering / wrong ID; re-probing     */
} vcnl4040State_e;

typedef struct {
    uint8_t irCurrent;            /* LED_I code 0..7 (50..200 mA)             */
    uint8_t psIntegration;        /* PS_IT code 0..7 (1T..8T)                 */
    uint8_t psMultiPulse;         /* PS_MPS code 0..3 (1..8 pulses)           */
} vcnl4040Settings_t;

/* Sample-op completion, from the I2C ISR. value is PS_DATA for a read. */
typedef void (*vcnl4040DoneFn_t)(laneStatus_e status, uint16_t value);

/* Starts the ID probe (async). Assumes mx_i2c1_i2c_init() ran. */
laneStatus_e vcnl4040Init(void);

/* Advances housekeeping. Main loop, every pass. Returns an error when a
 * script gave up (the caller reports it); recovery carries on regardless. */
laneStatus_e vcnl4040Run(uint32_t nowMs);

/* Wanted: PS on in force mode with these settings. Takes effect when the bus
 * is free; vcnl4040State() says when it is ACTIVE. */
laneStatus_e vcnl4040Activate(const vcnl4040Settings_t *s);

/* Wanted: shut down (sampling OFF / sleep). */
void vcnl4040Shutdown(void);

/* Clears the bus, re-inits I2C1, re-probes and restores the wanted state. */
void vcnl4040RequestRecovery(void);

vcnl4040State_e vcnl4040State(void);

/* ACTIVE with exactly the settings last passed to vcnl4040Activate(): the
 * sampler may run. */
bool vcnl4040Settled(void);

/* The ID matched on the last probe and the sensor is not in FAULT. */
bool vcnl4040IsOk(void);

/* ISR-safe. Writes PS_TRIG; done(status, 0) when the write completed (the
 * conversion is running). LANE_ERR_SENSOR_NOT_READY unless ACTIVE and idle,
 * LANE_ERR_I2C_BUSY while another transfer runs. */
laneStatus_e vcnl4040Trigger(vcnl4040DoneFn_t done);

/* ISR-safe. Reads PS_DATA; done(status, value). Same refusals. */
laneStatus_e vcnl4040ReadPs(vcnl4040DoneFn_t done);

/* EXTI0 (SENS_INT) edge; counted only. */
void vcnl4040OnInt(void);

/* Diagnostics. */
uint32_t vcnl4040ErrorCount(void);
uint32_t vcnl4040RecoveryCount(void);
uint32_t vcnl4040IntCount(void);
uint16_t vcnl4040LastId(void);

/* Failed sample ops in a row; sense_manager recovers past
 * VCNL4040_SAMPLE_ERRORS_MAX. */
uint32_t vcnl4040SampleErrorRun(void);
