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

    /* Text trace over SWO/ITM port 0 (PB3): boot info, state changes,
     * events, and on request the signal (lane_trace.h, laneTraceLevel). */
    #define LANE_ENABLE_SWO_TRACE                0

    /* Drive TP6 (PB4, the unused SPI CS pad) low for each sample, trigger
     * to data, so a scope sees the sample rate and the sensor latency. */
    #define LANE_ENABLE_SCOPE_MARKER             0

    /* laneBenchRequest (Live Watch) provokes faults and actions for the
     * bench procedures: HardFault, hang, fatal, single sample, ... */
    #define LANE_ENABLE_BENCH_HOOKS              0

    /* Sample and detect at boot with the compiled default CONFIG, on local
     * time, without a controller; the LED flashes on every crossing.
     * BENCH_TESTS.md procedures 5-7. Never in a chain. */
    #define LANE_BENCH_STANDALONE                0

    /* Do not start the IWDG (fault_guard). Only to single-step the boot. */
    #define LANE_DISABLE_WATCHDOG                0

    /* Do not put the LED and sensor to safe defaults when the controller
     * goes quiet (bus_manager). For a bench controller that is halted in
     * a debugger between transfers. */
    #define LANE_DISABLE_BUS_WATCHDOG            0

#else // DEBUG BUILDS

    /* Keep SWD alive through Sleep/Stop1 (sys_power, bring-up step 9). */
    #define LANE_DEBUG_DURING_LOW_POWER          1

    /* Blink the LED a few times once laneInit() is done. */
    #define LANE_ENABLE_INIT_DONE_FLASH          1

    /* Text trace over SWO/ITM port 0 (PB3). */
    #define LANE_ENABLE_SWO_TRACE                1

    /* TP6 (PB4) low for each sample, trigger to data. */
    #define LANE_ENABLE_SCOPE_MARKER             1

    /* laneBenchRequest (Live Watch) for the bench procedures. */
    #define LANE_ENABLE_BENCH_HOOKS              1

    /* Sample and detect without a controller (bench only). */
    #define LANE_BENCH_STANDALONE                0

    /* Do not start the IWDG. */
    #define LANE_DISABLE_WATCHDOG                0

    /* Keep LED and sensor as they are when the controller goes quiet. */
    #define LANE_DISABLE_BUS_WATCHDOG            0

#endif // RELEASE

// ==========================================================================
// ==========================================================================

/* A RELEASE-block switch left non-zero emits a #warning naming it — and still
 * builds, so a production-optimised image stays runnable on the bench. Those
 * checks live at the top of lane_master.c, not here, so they print once. Add a
 * switch here, add its check there. */
