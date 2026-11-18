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

    /* DBG_LOG stream lines (lane joins, I3C errors, BLE, binds, settings). */
    #define CTRL_ENABLE_DBG_LOG                  0

    /* Echo every nRF DATA frame back instead of feeding the BLE session. */
    #define CTRL_ENABLE_NRF_LINK_ECHO            0

    /* BTN1 very long = HardFault, BTN3 very long = hang (watchdog test). */
    #define CTRL_ENABLE_FAULT_INJECT             0

    /* Power the lanes at boot without a host. */
    #define CTRL_ENABLE_LANE_AUTO_POWER          0

    /* Never start the IWDG. */
    #define CTRL_DISABLE_WATCHDOG                0

    /* Busy-loop instead of WFI in the idle tail. */
    #define CTRL_DISABLE_IDLE_WFI                0

#else // DEBUG BUILDS

    /* Play a short blink on both panel LEDs once ctrlInit() is done. */
    #define CTRL_ENABLE_INIT_DONE_FLASH          1

    /* DBG_LOG stream lines to every open session: "LANE 3 JOINED addr=0x13",
     * "I3C ERR ...", "BLE HELLO ...", "BIND ...", "SET ...". The bench client
     * and the debug console print them. */
    #define CTRL_ENABLE_DBG_LOG                  1

    /* BLE bench test (BENCH_TESTS.md B-3): every DATA frame from the nRF is
     * sent straight back as DATA (credits respected) and the BLE session is
     * not run. Set to 1, flash, write bytes from the nRF Connect app, see
     * them come back. */
    #define CTRL_ENABLE_NRF_LINK_ECHO            0

    /* Watchdog / fault bench test (BENCH_TESTS.md F-1/F-2): BTN1 held 5 s
     * executes from an invalid address (HardFault -> breadcrumb -> reset ->
     * EVT_BOOT RESET_FAULT); BTN3 held 5 s hangs the loop (IWDG reset after
     * about 2 s -> RESET_WATCHDOG). Off by default: a held button must never
     * reboot a timing box by accident. */
    #define CTRL_ENABLE_FAULT_INJECT             0

    /* Switch the lane rail on at boot, without a host, so enumeration can be
     * watched on the scope with nothing but USB power (BENCH_TESTS.md L-1). */
    #define CTRL_ENABLE_LANE_AUTO_POWER          0

    /* Do not start the IWDG (stepping through init with a probe that does
     * not freeze it). The IWDG is frozen on a debug halt anyway. */
    #define CTRL_DISABLE_WATCHDOG                0

    /* Spin instead of WFI in sysPowerIdle(), if a probe loses the core in
     * Sleep on this part. Costs nothing but power. */
    #define CTRL_DISABLE_IDLE_WFI                0

#endif // RELEASE

// ==========================================================================
// ==========================================================================

/* A RELEASE-block switch left non-zero emits a #warning naming it — and still
 * builds, so a production-optimised image stays runnable on the bench. Those
 * checks live at the top of ctrl_master.c, not here, so they print once. Add a
 * switch here, add its check there. */
