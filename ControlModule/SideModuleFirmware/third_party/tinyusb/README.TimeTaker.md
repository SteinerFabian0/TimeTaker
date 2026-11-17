# TinyUSB (vendored)

**Version:** TinyUSB **0.21.0**, git `dae3f9a366bfcddbf9dcf1b48d7500286a849539`
(2026-06-30), from https://github.com/hathach/tinyusb. MIT license, see
`LICENSE`.

**What is copied:** `src/`, with only the one port this board uses,
`portable/st/stm32_fsdev`. 0.21.0 supports the STM32C5 (`OPT_MCU_STM32C5`)
and ships a NUCLEO-C542 board port. Do not edit the files here. Configuration
lives in the app's `tusb_config.h`, and the glue (descriptors, the USB IRQ
handler, clocks) lives in `usb_cdc.c`.

**Users:**
- the control app (`../LapCounterControlModule_cmake`);
- later, the control bootloader. That is why it sits beside the projects, not
  inside one.

**To upgrade:**
1. Replace `src/` with the new release, again keeping only
   `portable/st/stm32_fsdev`.
2. Update the version above.
3. Rebuild both presets and run a USB enumeration test on the board.
