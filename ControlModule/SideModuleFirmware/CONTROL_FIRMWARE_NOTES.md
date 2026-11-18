# Control Module Firmware — CubeMX notes

These notes hold the CubeMX facts and the firmware status log. **What to
build next, and how, is in `CONTROL_FIRMWARE_GUIDE.md`** next to this file
(task cards in its §9).

Project: `LapCounterControlModule.ioc2` (STM32CubeMX2), set up 2026-11-16 from
`../SideModulePCB/ControlModule.kicad_sch`. The generated code lives in
`LapCounterControlModule_cmake/` and builds with 0 warnings. Last CubeMX pass:
2026-11-17 (see "CubeMX pass 2026-11-17" below).

## MCU and clocks

| Item | Value |
|---|---|
| MCU | STM32C542CCU6 (schematic symbol says STM32C531CCU6: same die and pinout) |
| HSE | 24 MHz crystal Y201 on PH0/PH1 (18 pF), `HAL_RCC_HSE_ON` (crystal, not bypass). CSS off. |
| SYSCLK | PSI fed from HSE (24 MHz ref) → 144 MHz, flash latency 4 |
| AHB/APB1/2/3 | 144 MHz, timers 144 MHz |
| USB | 48 MHz from PSI/3, so crystal accurate and no CRS needed |

## Pin map (labels = schematic nets)

| Pin | Label | Config |
|---|---|---|
| PA8 / PA9 | BUS_SDA / BUS_SCL | I3C1 controller (AF2). 2.2k SDA pull-up R306 to LANE_3V3 |
| PA10 | CHAIN_OUT | GPIO out push-pull, init low |
| PB14 | BUS_INT | Input, no pull (10k R307 to LANE_3V3), EXTI14 falling |
| PB15 | SYNC | TIM12_CH2 (AF2), 1 kHz PWM, 50 % |
| PA6 | LANE_PWR_EN | GPIO out, init low (lane power off) |
| PA5 | EXT_PWR_EN | GPIO out, init low |
| PA4 | EXT_PWR_FAULT | Input pull-up, EXTI4 falling (active-low assumed) |
| PB12 | USB_FAULT | TPD4S201 FLT* (open-drain, active-low): input pull-up, EXTI12 falling |
| PA11 / PA12 | USB_DN / USB_DP | USB_DRD_FS device (PCD) |
| PB6 / PB7 | UART1_TX / UART1_RX | USART1 to nRF52 (AF7). PB7 (RX) has the internal **pull-up**, so an nRF in reset (pins floating) reads as an idle line |
| PB5 | BLE_IRQ | Input pull-down, EXTI5 rising (nRF → STM). Meaning: **high while a BLE central is connected** (SYSTEM_ARCHITECTURE §9.4) |
| PB4 / PA15 | BLE_GPIO1 / BLE_GPIO2 | Inputs. PB4 = **BOOT_REQ** (nRF → STM, **active low**): internal **pull-up**, the same as the future control bootloader uses, so an nRF in reset reads as "no request". PA15 is spare: pull-down. |
| PA2 / PA3 | UART2_TX / UART2_RX | USART2 on J302 pins 5/4 (AF7) |
| PA0 / PA1 | UART4_TX / UART4_RX | UART4 on J302 pins 8/7 (AF8) |
| PB10 / PB2 / PA7 | SPI_SCK / SPI_MOSI / SPI_MISO | SPI2 on J303 (AF5 / AF7 / AF6) |
| PB1 / PB0 | SPI_CS1 / SPI_CS2 | GPIO out, init high (deselected), active low |
| PB8 | LED_ACT | TIM1_CH1 (AF1) PWM |
| PB13 | LED_STATUS | TIM8_CH2 (AF2) PWM |
| PC15 / PC13 / PE2 | BTN1 / BTN2 / BTN3 | Input pull-up, EXTI falling (no external pull-up) |
| PH0 / PH1 | HSE_IN / HSE_OUT | Crystal |
| PA13 / PA14 / PB3 | SWDIO / SWCLK / SWO | Debug: serial wire + async SWO trace |
| PC14, PB9 | — | Not connected, excluded |

NRST and BOOT0 can also be driven by the nRF52 (P0.14 / P0.12). They need no CubeMX setup.

## CubeMX configuration

| Block | Setting | Init |
|---|---|---|
| I3C1 | Controller, pure I3C bus, 1 MHz push-pull (conservative for 6 chained boards), hot-join on, EV/ERR IRQ | **generated only** (the lane bus code calls `mx_i3c1_init()` once the rail has settled) |
| TIM12 | SYNC master timebase: PSC 143 (1 MHz), ARR 999 → 1 kHz, CH2 PWM1 pulse 500 (rising edge at update), update IRQ | called |
| TIM1 / TIM8 | LED PWM 1 kHz: PSC 143, ARR 999, PWM1, pulse 0 (dark), preload on | called |
| USART1 | BLE link 460800 8N1, FIFO on, IRQ | called |
| USART2 | J302 lane transformer 115200 8N1, IRQ | **generated only** |
| UART4 | J302 Arduino 115200 8N1, IRQ | **generated only** |
| SPI2 | Master, full duplex, mode 0, 8 bit, 4.5 Mbit/s, software CS, IRQ | **generated only** |
| USB_DRD_FS | Device (PCD), FS, IRQ. No class stack (USBX/TinyUSB) chosen yet | called |
| MPU / ICACHE | CubeMX defaults | called |
| RNG | NIST candidate config, kernel clock CK48 = PSI/3 = 48 MHz (shared with USB), no IRQ | **generated only** |
| IWDG | LSI 32 kHz, `start_function` off, calculator max 2000 ms (the generated init does **not** start it) | **generated only** |
| CRC | Defaults: CRC-32 poly 0x04C11DB7, init 0xFFFFFFFF, no input/output reversal | **generated only** |

The firmware calls the "generated only" blocks when it needs them (`mx_spi2_init()`,
`mx_usart2_uart_init()`, `mx_uart4_uart_init()`, `mx_i3c1_init()`, `mx_rng_init()`,
`mx_iwdg_init()`, `mx_crc_init()`).

- **IWDG:** `mx_iwdg_init()` only initialises the handle. `fault_guard` starts it
  with `HAL_IWDG_Start(h, 0, 2000, 0)` (times in ms: `USE_HAL_IWDG_TIME_UNIT`
  defaults to ms) and refreshes it with `HAL_IWDG_Refresh()`.
- **CRC:** the CubeMX2 CLI refused the input/output reversal settings (empty
  error). For the zlib/IEEE CRC-32 (reflected), call `HAL_CRC_SetConfig()` with
  input reverse BYTE and output reverse BIT after `mx_crc_init()`, and XOR the
  result with 0xFFFFFFFF. Or keep the software CRC32.
- **RNG:** `mx_rng_init()` sets the CK48 kernel source to PSI/3 again. That is
  the same source `usb_cdc` sets, so calling it after USB init is harmless.

## Interrupt priorities (4 bits preemption, 0 sub)

| Prio | IRQ | Why |
|---|---|---|
| 0 | EXTI4 (EXT_PWR_FAULT), EXTI12 (USB_FAULT) | Power faults first |
| 1 | TIM12 (SYNC tick) | Master time; must never miss a period |
| 2 | I3C1 EV/ERR | Lane bus |
| 3 | EXTI14 (BUS_INT) | Lane events |
| 4 | USB | Host link |
| 5 | USART1 | BLE |
| 6 | EXTI5 (BLE_IRQ) | |
| 7 | USART2, UART4 | Header UARTs |
| 8 | SPI2 | Header SPI |
| 9 | EXTI2/13/15 (buttons) | |
| 15 | SysTick | HAL timebase |

## Things the firmware must know

- **Master time:** use `tick × 1000 + TIM12->CNT` µs, with `tick` counted in the
  TIM12 update IRQ. The SYNC rising edge is exactly the update event. This matches
  the lane guide (§3.3: about 1 kHz SYNC, lanes capture rising edges).
- **Lanes unpowered (LANE_PWR_EN low):** the SDA and BUS_INT pull-ups go to
  LANE_3V3, which the TPS22917 QOD discharges to 0 V. So BUS_SDA and BUS_INT read
  low and EXTI14 can fire at boot or power-down. Ignore BUS_INT and keep I3C idle
  until lane power is up and settled.
- **EXTI at init:** the generated code enables every EXTI IRQ in
  `mx_gpio_default_init()`.
- **TIM1/TIM8 are advanced timers:** PWM start needs the main output enable (HAL
  start does this). The pulse starts at 0, so the LEDs are dark.
- **USB:** CubeMX configures only the PCD driver. The firmware runs
  **TinyUSB CDC-ACM** instead and excludes the generated `mx_usb_drd_fs.c`
  (see Firmware status below).
- **Firmware update:** the ROM bootloader's USART1 is PA9/PA10 (the lane bus),
  not PB6/PB7, so the nRF cannot reach it. BLE/USB updates go through our own
  bootloader (SYSTEM_ARCHITECTURE §10.1). Its entry strap is BOOT_REQ = PB4,
  active low. PB4's NJTRST pull-up at reset means an nRF in reset reads as "no
  request". Since the 2026-11-17 pass the app's CubeMX config has the pull-up
  too, so the app and the bootloader agree.
- **Host protocol:** `app/config/host_protocol.h` is generated from
  `../../protocol/host_protocol.toml` (`protocol/gen_protocol.py`). Never edit
  it by hand.
- **Open:** low-power modes, HSE CSS, I3C speed increase after bench tests.
  IWDG is configured; `fault_guard` (task C6) starts it.

## Firmware status

Updated 2026-11-18 (task cards C1–C7, no board). Everything in
`CONTROL_FIRMWARE_GUIDE.md` §9 C1–C7 that can be written without hardware is
written, builds and is host-tested; **nothing is flashed or bench-tested**.
The bench procedures are in `BENCH_TESTS.md`. The older C0 checklist below
("On the bench") is now BENCH_TESTS.md C0.

**Sizes (2026-11-18):** debug app 61.4 KB flash / 43.9 KB RAM (24 KB event
log, 4 KB stack, 3 KB nRF rings); release app 60.7 KB / 43.9 KB; `ctrl_boot`
29.4 KB flash of 32 KB / 15.8 KB RAM.

**Flash layout** (`app/modules/image_header.h`): ctrl_boot 0x0800_0000 (32 KB,
4 pages — 24 KB did not fit), app 0x0800_8000 (208 KB, header at +0x400),
settings 0x0803_C000 (2 pages, bank 2). The default `debug_GCC_main` /
`release_GCC_main` presets still link a standalone app at 0x0800_0000;
`debug_GCC_boot` / `release_GCC_boot` link it behind the bootloader.

**New modules:** `i3c_ctrl`, `nrf_link`, `settings_store`, `flash_drv`
(LL: the HAL flash program/erase are disabled in hal_conf), `rng`,
`fault_guard`, `sys_power`, `exti_latch` (owns HAL_EXTI_TriggerCallback);
pure and host-tested: `lane_logic`, `seq_planner`, `bind_list`,
`settings_record`, `crc32`, `nrf_link_proto`, `byte_ring`, `stream_budget`,
`image_header`, `update_writer`, plus `log_line`, `boot_shared.h`.
Coordinators: `lane_manager` (rewritten), `sequence_manager` (new),
`host_manager` (BLE session, bind policy, every 0.2 request), `panel_manager`
(BTN2 long = abort), `ctrl_master` (init order, watchdog, fault inputs).
`boot/boot_main.c` is the bootloader; `tools/image_stamp.py` stamps the
header after every link; `tools/optprog.py` programs the option bytes.

**Findings:** `HAL_I3C_PRIVATE_WITH_ARB_RESTART` is miscoded in the C5 HAL
(see `i3c_ctrl.h`); the C5 LL/HAL say BOOT_SEL = 1 means "BOOT0 from the
pin", the opposite of the hub script's docstring (BENCH_TESTS.md F-4).

### Build

```
cd LapCounterControlModule_cmake
export PATH=~/.local/share/stm32cube/bundles/{cmake,ninja,gnu-tools-for-stm32}/<version>/bin:$PATH
cmake --preset debug_GCC_main && cmake --build --preset debug_GCC_main    # or release_GCC_main
test/run_all.sh                                                         # host tests
```

**Size:** see "Firmware status" above.

**Flash** with STM32CubeProgrammer, which comes as an STM32Cube bundle and knows
the C5. The xPack OpenOCD in `~/.local/bin` has no C5 target. The ST-LINK V3
connects to J201 (Tag-Connect):

```
P=~/.local/share/stm32cube/bundles/programmer/2.23.0/bin/STM32_Programmer_CLI
$P -c port=SWD mode=UR -w build/debug_GCC_main/LapCounterControlModule.elf -v -rst
```

The udev rules for the ST-LINK V3 and the DFU bootloader are installed
(`/etc/udev/rules.d/49-stlinkv3.rules`, `49-stm32-dfu.rules`), and the user is in
`dialout`.

### Structure

`main.c` calls `ctrlInit()` / `ctrlRun()` (`app/coordinators/ctrl_master.c`).
That is the hub's and the lane's pattern.

**Coordinators:**

| File | Job |
|---|---|
| `ctrl_master` | Init order and the error policy |
| `host_manager` | Event log, USB session, requests: NODE_LIST, LANE_POWER, REBOOT |
| `lane_manager` | Lane rail + SYNC sequencing |
| `panel_manager` | Status/activity LEDs, buttons → EVT_BUTTON |

**Modules:**

| Group | Files |
|---|---|
| Hardware | `sys_time` (TIM12), `indicator_led` (TIM8/TIM1), `button_handler`, `lane_chain`, `usb_cdc` (TinyUSB), `device_id` |
| Pure, host-tested | `frame_codec`, `event_log`, `host_session` |
| Shared | `irq_guard`, `led_pattern` |

### Decisions taken in this pass

- **USB:**
  - **TinyUSB 0.21.0** (`../third_party/tinyusb`, supports the C5 upstream).
    CDC-ACM, product "TimeTaker", serial number = MCU UID.
  - **VID/PID** come from `[usb]` in `protocol/host_protocol.toml`, so the
    firmware and the web app share one source. Today they are the pid.codes
    test pair 1209:0001, fine for development; allocate a real pair before
    publishing.
  - **No CubeMX change was needed to hand USB to TinyUSB.** CubeMX2 always
    generates `mx_usb_drd_fs.c` with a HAL PCD init *and*
    `USB_DRD_FS_IRQHandler`. `cmake/ctrl_app.cmake` marks that file
    `HEADER_FILE_ONLY`, and `usb_cdc.c` supplies `mx_usb_drd_fs_device_init()`
    (USB clock, CK48 = PSI/3, NVIC priority 4) plus TinyUSB's IRQ handler.
    This survives regeneration.
- **System time:**
  - TIM12 counts from `ctrlInit()` on: `sysUs = ticks × 1000 + CNT`, with the
    pending-UIF race handled.
  - **SYNC output is off until the lanes are powered.** `lane_manager`
    switches the rail first and enables SYNC 30 ms later. Power-off parks
    signals first.
  - `sysTimeNowUs()` must not be called from a priority-0 ISR (see
    `sys_time.h`).
- **Lane pins at boot:** `lane_chain` parks PA8/PA9 (I3C) as analog and holds
  CHAIN_OUT low as the first step of `ctrlInit()`.
- **Event log:** 1024 events in RAM. EVT_BOOT is evSeq 0. Every session
  pumps from its own cursor; RESUME replays (SYSTEM_ARCHITECTURE §7.4).
- **bootId:** a `.noinit` boot counter hashed with the UID. A `.noinit`
  section was added to the linker script, and fault_guard's breadcrumb will
  use it too.
- **Stack** raised from 1 KB to 4 KB in the linker script: the request path
  uses about 550 B, and up to four ISR levels nest on top.
- **Protocol:** `LANE_POWER` (0x21) and status codes `ERR_TIMER` /
  `ERR_USB` were added to the TOML.

### CubeMX pass 2026-11-17 (CONTROL_FIRMWARE_GUIDE §8)

Applied through the CubeMX2 CLI. No pin moved.

| Change | Effect in the generated code |
|---|---|
| I3C1 init → generated only | `mx_system_init()` no longer calls `mx_i3c1_init()`. PA8/PA9 stay in their reset state (analog) until `laneChainInit()` parks them, so SCL is never driven at boot. This closes the gap noted in `lane_chain.h`. |
| PB4 (BOOT_REQ) pull-down → pull-up | Initialised together with PB12 (pull-up, EXTI12); PB5 BLE_IRQ keeps its pull-down and EXTI5 rising |
| PB7 (USART1 RX) pull-up | `mx_usart1.c` now initialises TX (no pull) and RX (pull-up) separately |
| RNG, IWDG, CRC enabled, generated only | New `mx_rng.c`, `mx_iwdg.c`, `mx_crc.c`; HAL modules switched on in `stm32c5xx_hal_conf.h`; components added to `cmake/components.cmake` |

**How it was regenerated** (no GUI Generate): the project was generated into a
scratch folder (`include-packs-from-local`, as the GUI does), compared with this
tree, and only the changed files were copied: `generated/hal/*` and
`cmake/components.cmake`. Their digests were merged into
`.settings/exported-files.json`. `main.c`, the linker script, the root
`CMakeLists.txt` (the `include(cmake/ctrl_app.cmake)` line is still there),
`CMakePresets.json` (it holds the hand-added `release_GCC_main` preset) and
`stm32c5xx_dfp/` were not touched.

**Check after a GUI Generate:** it rewrites the root `CMakeLists.txt` (re-add
`include(cmake/ctrl_app.cmake)`), and it may also rewrite `CMakePresets.json`,
which would drop the `release_GCC_main` preset. Diff both files afterwards.

**Initial GPIO levels after the pass** (from `mx_gpio_default.c`, checked):
PA5 EXT_PWR_EN, PA6 LANE_PWR_EN and PA10 CHAIN_OUT are outputs, init low.
PB0/PB1 SPI_CS are outputs, init high. PA4, PB12, PB4 and the buttons
(PC13/PC15/PE2) are inputs with pull-up. PB5 and PA15 are inputs with
pull-down. PB14 BUS_INT is an input with no pull. PA8/PA9 are not touched
(reset state: analog).

### Open

- Everything on the bench (BENCH_TESTS.md).
- CubeMX items for the next pass: EXTI5 (BLE_IRQ) both edges (polled today);
  I3C1 `get_last_errors` on (error causes are read from SER after the HAL
  cleared ERRF); optionally the HAL flash program/erase switches (flash_drv
  uses the LL instead).

### On the bench (task C0, still to do: needs a board)

Lane ribbon **unplugged**. ST-LINK V3 on J201, USB-C to the PC.

1. **Flash** (command under "Build"). Then `-rst`, or press SW605.
   - *Expect:* both panel LEDs flash at boot (`ctrl_master`'s boot flash), then
     LED_STATUS (D601) shows the link state (step 5).
2. **Serial port:** `ls -l /dev/ttyACM*` and `udevadm info /dev/ttyACM0 | grep -E
   'ID_MODEL=|ID_VENDOR_ID|ID_MODEL_ID|ID_SERIAL_SHORT'`.
   - *Expect:* model `TimeTaker`, 1209:0001, serial = the 24 hex digits of the UID.
   - *If it is missing:* `dmesg | tail` for USB descriptor errors. USB_FAULT
     (PB12) low means the TPD4S201 has cut the port.
3. **Debug console:** run `cd WebApp && npm run debug` (or the `webapp-debug`
   entry in `.claude/launch.json`) and open http://localhost:5173 in Chrome.
   Choose Web Serial; the port picker shows "TimeTaker (ttyACM0)".
   - *Expect HELLO:* protocol version, firmware 0.1.0, a bootId, log head ≥ 1.
   - *Expect EVT_BOOT:* evSeq 0.
   - *Expect NODE_LIST:* the CTRL node.
   - *Expect EVT_BUTTON:* one per press of BTN1 (SW601), BTN2 (SW602) and
     BTN3 (SW603), short and long.
4. **SYNC:** scope CH1 on PB15 / J301 pin 7 (GND on pin 8), and CH2 on
   LANE_3V3 (J301 pin 1, or D403's anode). Trigger on CH2 rising. Send
   LANE_POWER on from the console.
   - *Expect:* LANE_3V3 ramps up in about 16 ms (TPS22917, CT 10 nF).
   - *Expect:* SYNC starts about 30 ms after the rail switches on
     (`LANE_MANAGER_SETTLE_MS`), as a 1.000 kHz square wave, 50 % duty,
     0 to 3.3 V. Before that, SYNC is low or Hi-Z: `sysTimeSetSyncOutput()`
     only clears CC2E, and the RM does not say for certain which of the two
     TIM12 gives. Either one meets HARDWARE_REFERENCE §2.7. With a 10 k
     pull-down on the probe, a Hi-Z pin reads 0 V too. Note which one you see.
   - *Expect:* the frequency is within ±20 ppm of 1 kHz (the crystal), if the
     scope's counter is good enough.
   - *Expect on LANE_POWER off:* SYNC stops (no 3.3 V level held) **before**
     the rail starts to fall.
   - *Expect:* CHAIN_OUT (J301 pin 2), SCL (pin 3) and SDA (pin 5) stay low
     the whole time. SDA follows the 2.2 k pull-up to LANE_3V3 once the rail
     is up, because I3C is not running yet.
5. **Panel LEDs:** LED_STATUS (D601) gives a short blip every 2 s without USB
   enumerated, blinks at 1 Hz with USB up and no session, and is steady while
   the console is connected. LED_ACT (D602) flashes on traffic.
6. **Lane rail current** (HARDWARE_REFERENCE §7), if a lane is at hand: not
   part of C0. Measure it in C1.
