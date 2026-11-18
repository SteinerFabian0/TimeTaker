/*
 * True random numbers from the C5's TRNG (RNG peripheral, NIST SP800-90B
 * entropy source; datasheet DS15125 §3.19). This answers CONTROL_FIRMWARE_GUIDE
 * §10 Q5: the C542 has the RNG, CubeMX generates mx_rng_init() (NIST
 * candidate config, CK48 = PSI/3 kernel clock shared with USB).
 *
 * Two users and nothing else (SYSTEM_ARCHITECTURE §6.1): the bootId at boot
 * (device_id) and the start-sequence hold (sequence_manager, seq_planner).
 *
 * If the RNG fails (seed or clock error) rngGet() still returns a word: the
 * last good one mixed with SysTick and the TIM12 counter, flagged with
 * CTRL_ERR_HAL, so a start sequence never stalls on it. The hold is then
 * "only" unpredictable to a human, which is what it is for.
 *
 * Owns: the RNG peripheral. Context: main loop only.
 */

#pragma once

#include <stdint.h>

#include "ctrl_status.h"

/* Calls mx_rng_init(). */
ctrlStatus_e rngInit(void);

/* One 32-bit random word; CTRL_ERR_HAL if it had to fall back. */
ctrlStatus_e rngGet(uint32_t *word);
