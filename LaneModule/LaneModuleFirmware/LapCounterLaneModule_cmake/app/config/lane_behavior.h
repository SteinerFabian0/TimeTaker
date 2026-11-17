/*
 * lane_behavior.h
 *
 * @brief  The behaviour switches you actually set by hand.
 *
 * Only knobs live here. A define that merely FOLLOWS from one of these — or
 * from RELEASE — lives in the module that acts on it, so this file stays short
 * enough to read before every flash.
 *
 * Values (timings, sizes, rates) live in the module that uses them; this file
 * is only for what the firmware DOES. Anything the mainboard can set over the
 * bus is not a knob either: it is a CONFIG register with a compiled default.
 *
 * RELEASE itself is supplied by the build configuration (LANE_RELEASE in
 * cmake/lane_app.cmake, set by the presets) and is tested directly wherever it
 * matters.
 */

#pragma once

// ==========================================================================
// ==========================================================================

#ifdef RELEASE // Set all to 0 to build the true production image.

    /* Keep SWD alive through Sleep/Stop1 (sys_power, bring-up step 9). */
    #define LANE_DEBUG_DURING_LOW_POWER          0

    /* Blink the LED a few times once laneInit() is done, so a bench board
     * shows it booted. Off in production: six lanes flashing on every
     * mainboard reset would look like a start sequence. */
    #define LANE_ENABLE_INIT_DONE_FLASH          0

#else // DEBUG BUILDS

    /* Keep SWD alive through Sleep/Stop1 (sys_power, bring-up step 9). */
    #define LANE_DEBUG_DURING_LOW_POWER          1

    /* Blink the LED a few times once laneInit() is done. */
    #define LANE_ENABLE_INIT_DONE_FLASH          1

#endif // RELEASE

// ==========================================================================
// ==========================================================================

/* A RELEASE-block switch left non-zero emits a #warning naming it — and still
 * builds, so a production-optimised image stays runnable on the bench. Those
 * checks live at the top of lane_master.c, not here, so they print once. Add a
 * switch here, add its check there. */
