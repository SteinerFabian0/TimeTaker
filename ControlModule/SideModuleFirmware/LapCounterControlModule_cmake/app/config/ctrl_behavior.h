/*
 * ctrl_behavior.h
 *
 * @brief  The behaviour switches you actually set by hand.
 *
 * Only knobs live here. A define that merely FOLLOWS from one of these — or
 * from RELEASE — lives in the module that acts on it, so this file stays short
 * enough to read before every flash.
 *
 * Values (timings, sizes, rates) live in the module that uses them; this file
 * is only for what the firmware DOES. Anything the web app can set over the
 * host protocol is not a knob either: it is a setting with a compiled default.
 *
 * RELEASE itself is supplied by the build configuration (CTRL_RELEASE in
 * cmake/ctrl_app.cmake, set by the presets) and is tested directly wherever it
 * matters.
 */

#pragma once

// ==========================================================================
// ==========================================================================

#ifdef RELEASE // Set all to 0 to build the true production image.

    /* Play a short blink on both panel LEDs once ctrlInit() is done, so a
     * bench board shows it booted. */
    #define CTRL_ENABLE_INIT_DONE_FLASH          0

#else // DEBUG BUILDS

    /* Play a short blink on both panel LEDs once ctrlInit() is done. */
    #define CTRL_ENABLE_INIT_DONE_FLASH          1

#endif // RELEASE

// ==========================================================================
// ==========================================================================

/* A RELEASE-block switch left non-zero emits a #warning naming it — and still
 * builds, so a production-optimised image stays runnable on the bench. Those
 * checks live at the top of ctrl_master.c, not here, so they print once. Add a
 * switch here, add its check there. */
