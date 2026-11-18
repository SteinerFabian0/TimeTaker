# Releasing a control module image

Every image names its source: `CTRL_FW_VERSION` (`cmake/version.cmake`) plus
the git revision, `0.1.0+abc1234` (`-dirty` from uncommitted changes). NODE_LIST
reports the numbers, the string is in the `.bin`. The image header at +0x400
carries the version, the revision, the size and the image CRC
(`tools/image_stamp.py` fills them after every link).

## Images and presets

| Preset | App at | Use |
|---|---|---|
| `debug_GCC_main` (default) | 0x0800_0000 | Bring-up: standalone, no bootloader needed |
| `release_GCC_main` | 0x0800_0000 | Standalone production image |
| `debug_GCC_boot` / `release_GCC_boot` | 0x0800_8000 | App behind `ctrl_boot`, updatable over USB/BLE |

Every preset also builds `ctrl_boot.elf` / `ctrl_boot.bin` (always -Os,
always at 0x0800_0000, 32 KB). Flash map: `app/modules/image_header.h`.

```
cmake --preset release_GCC_boot && cmake --build --preset release_GCC_boot
P=~/.local/share/stm32cube/bundles/programmer/2.23.0/bin/STM32_Programmer_CLI
$P -c port=SWD mode=UR -e all                                         # new chip
$P -c port=SWD mode=UR -w build/release_GCC_boot/ctrl_boot.elf -v
$P -c port=SWD mode=UR -w build/release_GCC_boot/LapCounterControlModule.elf -v -rst
```

Flash the **stamped** ELF or the `.bin` (at 0x0800_8000), never an older
unstamped copy: the bootloader refuses an image whose header CRC does not
match and stays in update mode (which is safe, just surprising).

Over USB/BLE (web app updater) send only the `_boot` preset's `.bin`. The
header's `loadAddr` (stamped from the ELF's `.vectors`) must be 0x0800_8000:
the updater and the bootloader both refuse the standalone `.bin`, whose
header and CRC are otherwise valid. `test/host_update` checks the built
`debug_GCC_boot` `.bin` against the bootloader's own check.

Switching a board from the standalone image to the bootloader layout: erase
the chip first (the standalone image's tail would sit in the bootloader pages).
The settings pages (0x0803_C000) survive `-w`; `-e all` clears them.

## Steps

1. `test/run_all.sh` passes (it also checks the generated protocol files and
   both shared headers against their other trees).
2. Commit everything; the release build warns about a dirty tree.
3. At a release point: bump `CTRL_FW_VERSION`, commit, tag `ctrl-v0.2.0`.
4. Build the preset; the stamp line prints size and both CRCs.
5. New chip: option bytes first (`tools/optprog.py`; needs pyocd and the C5
   DFP pack, `C5_PACK=/path/to/STMicroelectronics.stm32c5xx_dfp.*.pack`),
   then a full power-on reset.
6. Flash; note which unit got which version string.

## After a CubeMX2 regeneration

- Root `CMakeLists.txt` is overwritten: re-add `include(cmake/ctrl_app.cmake)`.
  Everything else (TinyUSB, `-Werror` on `app/`, version stamp, image stamp,
  `ctrl_boot`, the flash layouts) lives in `cmake/ctrl_app.cmake`.
- `CMakePresets.json` may be rewritten: re-add `release_GCC_main`,
  `debug_GCC_boot`, `release_GCC_boot` (copy from git).
- The linker script `user_modifiable/Device/STM32C542CCU6/stm32c542xc_flash.ld`
  must keep: `INCLUDE ctrl_memory.ld` instead of its MEMORY block, the
  `.image_header` section at `ORIGIN(ROM) + 0x400`, `.boot_shared` first in RAM,
  `.noinit`, and the 4 KB stack.
- I3C1, RNG, IWDG, CRC must stay "generated only"; `stm32c5xx_hal_conf.h` keeps
  the HAL modules on. Initial GPIO levels: PA5/PA6/PA10 low, PB0/PB1 high.
- `mx_usb_drd_fs.c` stays excluded (`HEADER_FILE_ONLY`), `usb_cdc.c` supplies
  `mx_usb_drd_fs_device_init()`.
