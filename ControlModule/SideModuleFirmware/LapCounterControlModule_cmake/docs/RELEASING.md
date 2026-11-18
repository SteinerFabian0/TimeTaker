# Releasing a control module image

Every image names its source: `CTRL_FW_VERSION` (`cmake/version.cmake`) plus
the git revision, `0.1.0+abc1234` (`-dirty` from uncommitted changes). NODE_LIST
reports the numbers, the string is in the `.bin`. NODE_LIST's CTRL `imageCrc`
is 0 (unknown): there is no image header since the bootloader went.

## Images and presets

v1 has **no bootloader and no field update**: every board is flashed over
SWD / Tag-Connect. The app is linked at 0x0800_0000 (240 KB); the last two
8 KB pages (0x0803_C000) hold the settings. Flash map: `app/modules/flash_drv.h`.
The bootloader layout (`ctrl_boot`, app at 0x0800_8000, image header, UPDATE_*)
is at git tag `archive/bootloader-update`.

| Preset | Use |
|---|---|
| `debug_GCC_main` (default) | Bring-up and bench: -Og, debug knobs of `ctrl_behavior.h` |
| `release_GCC_main` | Production image: -O2, `RELEASE` defined |

Each build writes `LapCounterControlModule.elf` and `LapCounterControlModule.bin`.

```
cmake --preset release_GCC_main && cmake --build --preset release_GCC_main
P=~/.local/share/stm32cube/bundles/programmer/2.23.0/bin/STM32_Programmer_CLI
$P -c port=SWD mode=UR -e all                                         # new chip
$P -c port=SWD mode=UR -w build/release_GCC_main/LapCounterControlModule.elf -v -rst
```

The settings pages (0x0803_C000) survive `-w`; `-e all` clears them (the
board then boots with defaults and reports EVT_FAULT{SETTINGS} once).

## Steps

1. `test/run_all.sh` passes (it also checks the generated protocol files and
   both shared headers against their other trees).
2. Commit everything; the release build warns about a dirty tree.
3. At a release point: bump `CTRL_FW_VERSION`, commit, tag `ctrl-v0.2.0`.
4. Build `release_GCC_main`; it must build with 0 warnings (the dirty-tree
   `#warning` aside).
5. New chip: option bytes first (`tools/optprog.py`; needs pyocd and the C5
   DFP pack, `C5_PACK=/path/to/STMicroelectronics.stm32c5xx_dfp.*.pack`),
   then a full power-on reset.
6. Flash; note which unit got which version string.

## After a CubeMX2 regeneration

- Root `CMakeLists.txt` is overwritten: re-add `include(cmake/ctrl_app.cmake)`.
  Everything else (TinyUSB, `-Werror` on `app/`, version stamp, the `.bin`)
  lives in `cmake/ctrl_app.cmake`.
- `CMakePresets.json` may be rewritten: re-add `release_GCC_main` (copy from
  git).
- The linker script `user_modifiable/Device/STM32C542CCU6/stm32c542xc_flash.ld`
  must keep: ROM length 0x3C000 (the settings pages stay out of the image),
  `.noinit`, and the 4 KB stack.
- I3C1, RNG, IWDG, CRC must stay "generated only"; `stm32c5xx_hal_conf.h` keeps
  the HAL modules on. Initial GPIO levels: PA5/PA6/PA10 low, PB0/PB1 high.
- `mx_usb_drd_fs.c` stays excluded (`HEADER_FILE_ONLY`), `usb_cdc.c` supplies
  `mx_usb_drd_fs_device_init()`.
