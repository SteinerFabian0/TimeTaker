# TimeTaker — Bring-up plan

For the first day with the boards. Nothing has run on hardware yet; every
firmware is built and host-tested only. Work top to bottom: each step
assumes the ones before it passed. The procedures themselves live next to
each firmware; this file is the order, the commands, and what to watch.

| Node | Procedures | Status / results go into |
|---|---|---|
| Control STM32 | `ControlModule/SideModuleFirmware/BENCH_TESTS.md` (C0, L, E, S, P, R, B, F) | `CONTROL_FIRMWARE_NOTES.md` "Firmware status", guide §2 |
| Lane STM32 (×6) | `LaneModule/LaneModuleFirmware/BENCH_TESTS.md` (1–14) | `LANE_FIRMWARE_GUIDE.md` §11 |
| nRF52805 bridge | `ControlModule/BleFirmware/BENCH_TESTS.md` (T0–T12) | `BLE_FIRMWARE_GUIDE.md` §14 |
| Host side | `WebApp/tools/README.md`: `tt` bench CLI and its suites | — |

Before the boards come out of the bag: `tools/check_all.sh` must say
`ALL CHECKS PASSED` (host tests of all four nodes, the generator check, and
the three firmware builds). Read the top-10 risks below once.

---

## 1. Flashing the three chips

All three go over SWD with the ST-LINK V3. No bootloader anywhere in v1.

**Control STM32C542** (J201, Tag-Connect):

```
cd ControlModule/SideModuleFirmware/LapCounterControlModule_cmake
B=~/.local/share/stm32cube/bundles
export PATH=$(ls -d $B/cmake/*/bin|tail -1):$(ls -d $B/ninja/*/bin|tail -1):$(ls -d $B/gnu-tools-for-stm32/*/bin|tail -1):$PATH
cmake --preset debug_GCC_main && cmake --build --preset debug_GCC_main
P=$B/programmer/2.23.0/bin/STM32_Programmer_CLI
$P -c port=SWD mode=UR -e all                                                # new chip only
$P -c port=SWD mode=UR -w build/debug_GCC_main/LapCounterControlModule.elf -v -rst
```

Option bytes (new chip, after C0 works; BENCH F-4 first with `--dry-run`):
`python3 tools/optprog.py --dry-run`, then without (needs pyocd and the C5
DFP pack, `docs/RELEASING.md`).

**Lane STM32C542** (TC2030-NL Tag-Connect, SWD + SWO). Normally VS Code
"Debug (ST-LINK V3, SWD + SWO, AP1)" in `LapCounterLaneModule_cmake/`. From
the shell, same PATH as above:

```
cd LaneModule/LaneModuleFirmware/LapCounterLaneModule_cmake
cmake --preset debug_GCC_STM32C542CCU6 && cmake --build --preset debug_GCC_STM32C542CCU6
$P -c port=SWD ap=1 mode=UR -w build/debug_GCC_STM32C542CCU6/LapCounterLaneModule.elf -v -rst
```

`ap=1`: the C542's M33 AHB-AP sits at access port 1 (the lane's
`.vscode/launch.json`, the Yuhka hub). If the control command above does not
connect, add `ap=1` there too: it is the same chip.

**nRF52805** (J501, TC2030; ST-LINK V3 + xPack OpenOCD, no J-Link):

```
cd ControlModule/BleFirmware
tools/build.sh                 # NCS v3.4.2 toolchain, release image (stage B)
tools/flash.sh --recover       # the first time: mass erase (Raytac test image, APPROTECT)
tools/flash.sh                 # every later time; writes UICR.APPROTECT = 0x5A
```

Debug image with RTT: `BUILD=build-debug tools/build.sh -- -DEXTRA_CONF_FILE=debug.conf`,
then `HEX=build-debug/zephyr/zephyr.hex tools/flash.sh` (BLE BENCH T0).

---

## 2. The order

### Step 1 — Control module alone

Ribbon unplugged. The nRF stays as delivered for now **unless** risk 2
below bites (then erase it first: `tools/flash.sh --recover`).

1. Control BENCH **C0-1 … C0-5**: flash and boot flash, USB port
   "TimeTaker" 1209:0001, HELLO / NODE_LIST / STATUS, buttons, SYNC and the
   lane rail on the scope.
2. `node WebApp/tools/tt.ts ports`, then `tt hello`, `tt suite usb`
   (`export PATH=~/.local/bin:$PATH`, run from `WebApp/`).
3. Debug console: `cd WebApp && npm run debug`, http://localhost:5173/debug/
   in Chrome, Connect USB.
4. **F-4** option bytes (BOOT_SEL polarity — risk 3), then **P-1 … P-3**,
   **R-1**, **F-1 … F-3**, **F-5** (fault-inject knob in `ctrl_behavior.h`,
   rebuild).

Done when: `tt suite usb` passes, SYNC is 1.000 kHz ±20 ppm, settings
survive a power cycle, bootIds differ, a forced HardFault reboots with
`RESET_FAULT`.

### Step 2 — One lane

1. On the bench supply first (3.3 V, 100 mA limit), no controller: lane
   BENCH **1 … 7** (power/flash/LED, SWO trace, VCNL4040, sample timing and
   the bench constants, signal at 15 cm, detection with real cars in
   `LANE_BENCH_STANDALONE`, holdoff watch rate). Write the measured
   conversion time and latency trim into `lane_regs.h` /
   `LANE_SAMPLER_LATENCY_TRIM_US`.
2. Lane BENCH **8** (SYNC capture) with the control module's SYNC.
3. Plug it on the ribbon as lane 1, normal debug build: control **L-1**,
   lane BENCH **9 – 10** (enumeration, register map, FIFO reads).
4. Control **E-1**, **E-3**; `tt lane power on`, `tt watch`,
   `tt suite crossings` (one lane).

Done when: JOINED → CONFIGURED, a hand through the beam gives one
EVT_CROSSING with laneSeq +1 and a plausible dwell, raw stream flows.

### Step 3 — Six lanes

Flash all six lanes (same image; the index comes from the chain).

1. Lane BENCH **11** (scheduled LED actions on two lanes, the scope check of
   "dark within a few µs") and **12** (chain).
2. Control **L-2 … L-4** (rail current with six sampling: risk 6),
   `tt suite lanes`.
3. Control **S-1, S-2**, `tt suite start` (ten starts, the armSampling
   check in S-1).
4. Control **E-2** (RESUME by pulling USB), `tt suite resume`.
5. Lane BENCH **13** (robustness) and **14** (six lanes, cars, lights out).

Done when: six lanes join strictly in order, the lights breathe in phase
and all go out together, a lane unplugged mid-run comes back in order, and
the USB pull loses nothing.

### Step 4 — BLE

1. BLE BENCH **T0 – T5** (tools, flash/APPROTECT, supply/LFRC, advertising,
   connect with BLE_IRQ on the scope, the UART link with `link_tool.py`
   while the STM is held in reset).
2. T6, T7 (LED, button) through `link_tool.py`.
3. Control **B-1**, then **B-2** = BLE **T8** (rebuild the STM with
   `CTRL_ENABLE_NRF_LINK_ECHO = 1`, back to 0 afterwards), **T9**
   throughput, **T10** watchdog, **T12** reset lines.
4. Control **B-3** = BLE **T11** (binding with the web app over Web
   Bluetooth), `tt suite binding` over USB for the bind list side.

Done when: the web app runs over BLE after a 2 s button hold, a second
browser is kicked after 60 s, and a 10 KB burst passes the echo without
loss.

### Step 5 — Full race

1. Control **B-4**: a race over USB, pull the cable mid-race, finish on
   BLE; the standings must equal a run without the pull.
2. A real race with the director's defaults (2 attention pulses, 1 s rise,
   1 s step, 0.2–3 s hold): false start, minimum lap, a car stopped on the
   line (BLOCKED), a deslot and replace.
3. Export the race file and keep it: it becomes a regression fixture
   (`WebApp/test/fixtures/races/`, SYSTEM_ARCHITECTURE §11).

---

## 3. Next CubeMX pass (both boards)

Generate into a scratch folder and copy only the changed `generated/` files
and `cmake/components.cmake` (STM32C5_NOTES "CubeMX2 workflow"). After any
GUI Generate: re-add the `include(cmake/<tree>_app.cmake)` line in the root
`CMakeLists.txt` and diff `CMakePresets.json` (the hand-added release
presets get lost). Then `tools/check_all.sh`.

**Control module** (`LapCounterControlModule_cmake`):

1. **EXTI5 (PB5, BLE_IRQ): both edges.** Rising-only today, so `nrf_link`
   polls the pin every pass; with both edges the fall can be latched.
2. **I3C1: `get_last_errors` on** (`USE_HAL_I3C_GET_LAST_ERRORS`).
   `i3c_ctrl.c` reads SER itself today because the HAL clears ERRF first.
3. **RNG: auto-reset and clock error detection** in the RNG config, so
   `mx_rng.c` matches the Yuhka hub; then drop the two calls in
   `rngInit()` (`rng.c`).
4. **CRC: the reversal settings** (REV_IN byte, REV_OUT bit) if the CubeMX2
   CLI accepts them this time; `crc32.c` sets them after `mx_crc_init()`
   today (STM32C5_NOTES "CRC unit").
5. Optional: the HAL flash program/erase switches (`flash_drv` uses the LL;
   only needed if a HAL flash call is ever wanted).

**Lane module** (`LapCounterLaneModule_cmake`):

1. **I3C1 target max read length 64 → at least 96** (EVENT_FIFO 68 B,
   RAW_FIFO 80 B). `lane_bus.c` raises it with
   `HAL_I3C_TGT_SetConfigMaxDataSize` at init until then.
2. Optional: **CRC block** added, so `mx_crc.c` enables its clock
   (`image_crc.c` does it with LL and includes `stm32c5xx_ll_crc.h` itself).
3. Optional: **IWDG** in CubeMX (`fault_guard` starts it with LL). If added,
   keep `USE_HAL_IWDG_MODULE` consistent and do not let `mx_system_init()`
   start it.

Both: no pin moves. Check that the regenerated `mx_pwr_init()` still only
arms WKUP1 (both firmwares disarm it at init: STM32C5_NOTES "WKUP pins").

---

## 4. Top 10 risks, check in this order

1. **SWD on the C542.** The M33 AHB-AP is access port 1; the ST-LINK V3
   adapter needs its 0R jumpers fitted (the Yuhka hub saga). Symptom: no
   connect at all. Try `ap=1`, `mode=UR`, a lower SWD clock.
2. **The nRF factory image driving STM lines.** P0.14 is the STM's NRST and
   P0.12 its BOOT0. A Raytac test image that drives them can hold the STM in
   reset or in the ROM bootloader. If the STM looks dead or boots into DFU
   (`lsusb` 0483:df11), erase the nRF first (`tools/flash.sh --recover`).
3. **Option bytes / BOOT_SEL polarity** (control F-4). The C5 LL/HAL and the
   hub script disagree; our boards have a real BOOT0 pull-down. Dry-run
   first, never copy the hub's setting (STM32C5_NOTES "Option bytes").
4. **Clocks and SYNC.** HSE 24 MHz up, TIM12 SYNC at 1.000 kHz ±20 ppm,
   SYNC off before the rail falls (C0-5). Everything is timed on this axis.
5. **TinyUSB on the C5** (C0-2): first run of the `stm32_fsdev` port on this
   chip, CK48 = PSI/3, descriptors "TimeTaker" 1209:0001. Linux: `dialout`.
6. **Lane rail with six lanes sampling:** TPS22917 ramp (~16 ms), LDO heat,
   PTC margin (L-2, HARDWARE_REFERENCE §2.3). Six 200 mA IR pulses are
   phase-staggered by 1/6 period; check on the rail that they never overlap.
7. **I3C at register level, lane side** (LANE guide §11 "Unverified" 1):
   FC at the Sr, XDCNT = bytes really clocked out, PRELOAD early end, max
   read length (CubeMX item above), hot-join after RSTDAA, ENTDAA gated by
   CHAIN_IN with six identical provisioned IDs (the 60 ms rejoin hold vs the
   CHAIN cascade). Symptoms in control L-1 "If not".
8. **I3C controller on the C5 HAL:** the hand-built private-transfer control
   words (`HAL_I3C_PRIVATE_WITH_ARB_RESTART` bug), the hot-join → ENTDAA
   flow, IBIs NACKed (4 DEVR slots, v1 polls). DBG "DAA NOBODY" / "DAA
   MULTI", `i3cCtrlStats.lastSer`.
9. **VCNL4040 and detection timing:** register bit positions, conversion
   time, reachable sample rate, levels at 15 cm, saturation; then the
   latency trim, and inter-lane skew of scheduled actions (SYNC_ALIGN within
   period/2). Lane BENCH 3–5, 11.
10. **nRF52805 bring-up:** APPROTECT/UICR flow with OpenOCD, LDO + LFRC
    (no 32 kHz crystal), UARTE at 460800 (baud error on both chips), RAM
    headroom (2.3 KB free in release), stack sizes (debug image's thread
    analyzer), credits under load. BLE BENCH T1, T2, T5, T9.

---

## 5. Fixed in the seam review before bring-up (2026-11-18)

What changed in the code right before this plan, so a surprise on the bench
can be traced (details: SYSTEM_ARCHITECTURE §13):

- Control: seq checking after a lane FIFO overflow, the start-sequence
  read-back retried (2 ms × 10), armSampling sets the table mode at T0, BLE
  session reset on every disconnect report, coordinator decisions in
  `ctrl_policy` (host-tested).
- Lane: STATUS `schedulePending` live; the sample instant is the ISR time.
- Web app: lanes listed UPDATING are `joined`; simulator CLEARED / holdoff
  anchor / NODE_LIST as the firmware.
