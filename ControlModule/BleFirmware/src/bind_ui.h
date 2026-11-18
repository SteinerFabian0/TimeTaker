/*
 * Bind button and BLE LED. BLE_FIRMWARE_GUIDE.md §7.1, §7.2.
 *
 * A 10 ms k_timer samples SW501 (ISR context) through the pure classifier
 * (button_classifier.c) and, every 50 ms, wakes the main loop for the LED.
 * The main loop renders the LED from the pure table (led_table.c) with what
 * it knows (link state) and what the STM said (UI_STATE, CUE).
 *
 * The nRF only classifies and reports; pairing and clearing are the STM's
 * decisions (control guide §5.4).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "button_classifier.h"
#include "led_table.h"

int bindUiInit(void (*wake)(void));

/* Main loop: advances the LED and returns a press released since the last
 * call (BTN_EVT_NONE if none). A short press also plays the local
 * one-blink cue. */
btnEvent_e bindUiProcess(void);

/* From the STM's UI_STATE. */
void bindUiSetUiState(uint8_t session, bool pairWindow);
/* LINK_DOWN resets the session locally; pairWindow stays the STM's. */
void bindUiResetSession(void);

/* From the STM's CUE. */
void bindUiCue(ledCue_e cue);

/* From the link state. */
void bindUiSetLink(bool connected, bool advertiseOff);
