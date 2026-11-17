# Building and releasing the lane firmware

## Build

The toolchain bundled with STM32Cube, nothing from the system:

```
B=~/.local/share/stm32cube/bundles
export PATH=$B/cmake/4.4.0+st.1/bin:$B/ninja/1.13.2+st.1/bin:$B/gnu-tools-for-stm32/14.3.1+st.2/bin:$PATH
cmake --preset debug_GCC_STM32C542CCU6   && cmake --build --preset debug_GCC_STM32C542CCU6
cmake --preset release_GCC_STM32C542CCU6 && cmake --build --preset release_GCC_STM32C542CCU6
```

Both presets use the build type `debug_GCC_STM32C542CCU6`, because every
ST-generated `cmake/*.cmake` file is gated on it; `LANE_RELEASE` (in
`cmake/lane_app.cmake`) adds `-O2` and `RELEASE` on top. A release build warns
for every `lane_behavior.h` switch left in its debug position.

## Flash and debug (VS Code)

Open this folder (`LapCounterLaneModule_cmake/`) as the VS Code workspace.
`.vscode/` is the Yuhka hub setup (cortex-debug + ST-LINK_gdbserver from the
STM32Cube bundles, SWD on access port 1, 24 MHz), adapted to the lane:

- **Debug (ST-LINK V3, SWD + SWO, AP1):** builds the debug preset, flashes it
  over the Tag-Connect, runs to `main`. SWO off, as on the hub.
- **Attach (ST-LINK V3, SWD + SWO, AP1):** no flash, no reset; SWO on
  (2 MHz, CPU 144 MHz, ITM port 0 as console).

If the folder moves, delete `build/` and configure again: CMake caches the
absolute source path.

## Release

1. `test/run_all.sh` passes.
2. Bump `LANE_FW_VERSION` in `cmake/version.cmake`, commit, tag.
3. Build the release preset from the clean tree. `LANE_FW_TREE_DIRTY` must be 0
   (no "uncommitted changes" warning); a tree outside git always counts as
   dirty and stamps `+nogit`.

## After a CubeMX2 regeneration

- **Root `CMakeLists.txt` is overwritten** (the old one is kept as
  `CMakeLists.txt_bckp`). Everything the lane adds lives in
  `cmake/lane_app.cmake`, so only one line has to come back, after
  `include(cmake/components.cmake)`:
  `include(cmake/lane_app.cmake)`.
- **`CMakePresets.json` is overwritten**: re-add `LANE_RELEASE` to the debug
  preset and the `release_GCC_STM32C542CCU6` configure and build presets.
- **`main.c`** is user code and survives, but check it still calls
  `laneInit()` and `laneRun()`.
- Re-check the initial GPIO levels in `generated/hal/mx_gpio_default.h`:
  CHAIN_OUT low, BUS_INT released (high), CS high.
- `mx_system.c` must still init TIM1 and TIM2 (the LED and timebase adopt their
  CubeMX setup) and must not call `mx_spi1_init()`.
