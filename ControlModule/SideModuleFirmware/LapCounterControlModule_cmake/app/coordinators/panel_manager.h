/*
 * Panel manager: what the box shows and what its buttons mean.
 *
 * LED_STATUS (D601) shows the host link:
 *   short blip every 2 s   no host (USB not enumerated)
 *   1 Hz blink             USB up, no session yet (web app not connected)
 *   steady                 a web app has an open session
 * LED_ACT (D602) flashes on host traffic (frames in, events out).
 *
 * Buttons: every classified press of BTN1..BTN3 becomes an EVT_BUTTON. What a
 * press MEANS is the web app's decision, for example "BTN1 = start sequence"
 * (SYSTEM_ARCHITECTURE §1). Local fallbacks that must work without a host,
 * like BTN2 long = abort the start, arrive with sequence_manager.
 *
 * Owns: the policy over indicator_led and button_handler.
 * Context: main loop only.
 */

#pragma once

#include "ctrl_status.h"

/* ledsFreeAtMs: HAL_GetTick() value from which the panel may drive the LEDs,
 * so a boot flash started by ctrl_master plays out first. Buttons work at
 * once. */
ctrlStatus_e panelManagerInit(uint32_t ledsFreeAtMs);

ctrlStatus_e panelManagerRun(void);
