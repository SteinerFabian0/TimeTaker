/**
 * @file sense_manager.h
 * @brief Coordinator for sensing: lane_sampler -> vcnl4040 -> lane_detect ->
 *        event_fifo, plus holdoff, the raw stream, calibration and the
 *        single sample (SYSTEM_ARCHITECTURE §5, guide §3.4-3.6).
 *
 * Owns the active CONFIG, the sampling mode, the detector, the event and raw
 * FIFOs and the SIGNAL snapshot. Called from the main loop; the FIFO read
 * hooks are the only entry points used by the I3C ISR (through bus_manager).
 *
 * Rules:
 *   - Mode OFF puts the sensor in shutdown; ON and RAW activate it in force
 *     mode and run the sampler on the CONFIG schedule. RAW also streams every
 *     rawDecimation-th sample into RAW_FIFO (mainboard µs).
 *   - A CONFIG is applied as a whole: sampler stopped, sensor reconfigured
 *     if its codes changed, detector updated (baseline kept), sampler
 *     restarted. The holdoff watch rate (holdoffSamplePeriodUs) is used while
 *     the detector says the lane is in holdoff.
 *   - Events get their sync flags here: tEntry is mapped to the mainboard
 *     axis with sync_time, dwell is mapped as exit − entry in mainboard µs.
 *   - A sensor that fails VCNL4040_SAMPLE_ERRORS_MAX samples in a row is
 *     recovered (bus clear, re-probe); one SENSOR_FAULT event marks it, and
 *     sampling resumes by itself when the sensor is back.
 *   - Parked (boot-loop guard): every mode request is refused.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lane_status.h"
#include "lane_protocol.h"
#include "event_fifo.h"

laneStatus_e senseManagerInit(void);

/* Main loop, every pass. */
laneStatus_e senseManagerRun(uint32_t nowMs);

/* CONFIG write, already validated by lane_regs. */
laneStatus_e senseManagerApplyConfig(const laneConfig_t *cfg);

/* The CONFIG in use (compiled defaults until one is written). */
void senseManagerGetConfig(laneConfig_t *out);

bool senseManagerConfigWritten(void);

/* Forgets the written CONFIG (address lost): back to defaults, OFF. */
void senseManagerResetConfig(void);

laneStatus_e senseManagerSetMode(laneSamplingMode_e mode);
laneSamplingMode_e senseManagerMode(void);

/* COMMAND CALIBRATE: relearn the baseline (the lane must be free). */
laneStatus_e senseManagerCalibrate(void);

/* COMMAND SET_REFERENCE: value != 0 seeds baseline and reference (no
 * learning, calibrated at once), 0 forgets and relearns. */
laneStatus_e senseManagerSetReference(uint16_t value);

/* COMMAND SINGLE_SAMPLE: one sample into SIGNAL, also while OFF. */
laneStatus_e senseManagerSingleSample(void);

/* Refuse everything from now on (parked). */
void senseManagerPark(void);

/* STATUS flag bits this coordinator knows (SENSOR_OK, CALIBRATED, ...). */
uint16_t senseManagerStatusFlags(void);

/* SIGNAL snapshot (ISR-safe copy), with rawMin/rawMax of the window since the
 * last delivered SIGNAL read (lane_detect.h, laneRawWindow_t). The window is
 * not reset here: the response may be built more than once per read. */
void senseManagerGetSignal(laneSignal_t *out);

/* The read-done hook saw a full SIGNAL response clocked out: the window it
 * carried is spent. I3C ISR. */
void senseManagerSignalDelivered(void);

/* Event FIFO and raw FIFO access for the I3C read path (bus_manager). Call
 * with interrupts at I3C priority or under irq_guard. */
eventFifo_t *senseManagerEventFifo(void);
rawFifo_t   *senseManagerRawFifo(void);

/* COMMAND CLEAR_COUNTERS. */
void senseManagerClearCounters(void);

/* Pushes one event with sync flags (also used for SENSOR_FAULT). */
void senseManagerPushEvent(uint8_t type, uint8_t flags, uint32_t tEntryLocal,
                           uint32_t dwellUs, uint16_t depth);
