# TimeTaker — Hardware Reference

The gist of the hardware in one place. Read this before you open a schematic.
Open the schematic only to check a detail.

Written 2026-11-16 from the KiCad netlists (exported with `kicad-cli`), the
schematic notes, the CubeMX2 projects and the STM32C531 datasheet. If this file
and a schematic disagree, the schematic wins. Fix this file when that happens.

**Sources of truth**

| What | Where |
|---|---|
| Control module schematic and PCB | `ControlModule/SideModulePCB/ControlModule.kicad_sch` (sheets: stm32c542, power, connectors, ble, ux) |
| Lane module schematic and PCB | `LaneModule/LaneModulePCB/LaneModule.kicad_sch` (sheets: stm32c542, vcnl4040, power_connectors) |
| Control CubeMX config | `ControlModule/SideModuleFirmware/LapCounterControlModule.ioc2` + `CONTROL_FIRMWARE_NOTES.md` |
| Lane CubeMX config and firmware brief | `LaneModule/LaneModuleFirmware/LapCounterLaneModule.ioc2` + `LANE_FIRMWARE_GUIDE.md` |
| System architecture (who processes what, protocols) | `SYSTEM_ARCHITECTURE.md` |
| MCU datasheet (same die as C542) | `~/Documents/Yuhka_Docs/stm32c531cc.pdf` |
| Original concept doc (German, predates the hardware) | claude.ai/artifact/7516F3ijtD7JXM8eD558Xi, "Rundenzähler & Zeitnahme – Märklin Sprint 6-spurig" |

---

## 1. System at a glance

```
                       USB-C (CDC)                      UART 460800
 Laptop / tablet  ◄──────────────────►  ┌───────────────────────────────┐
 web app (Chrome)                       │ CONTROL MODULE ("SideModule") │
        ▲                               │  STM32C542 @144 MHz, 24 MHz   │
        │ BLE (Web Bluetooth)           │  crystal = system time master │
        └──────────────────────────►    │  nRF52805 BLE module (U501)   │
                                        └──┬───────────┬────────────┬───┘
                     J301 lane bus (2x5)   │           │ J302       │ J303
                     3V3, I3C, SYNC,       │           │ UART/power │ SPI
                     CHAIN, BUS_INT        ▼           ▼ (future)   ▼ (spare)
          ┌────────┐  ┌────────┐       ┌────────┐   power board, Arduino+HDMI
          │ Lane 1 ├──┤ Lane 2 ├─ … ──┤ Lane 6 │
          └────────┘  └────────┘       └────────┘
          6 identical boards, plugged edge to edge. Each one has an STM32C542,
          a VCNL4040 looking down at retro foil, and a red start-light LED.
```

- **Track:** Märklin Sprint, 6 lanes, over 50 m, 12 V, old transformers with
  hand controllers. Timing goal: at least 1/100 s per lap. All 6 cars at once.
- **Detection:** each lane has a VCNL4040 about 15 cm above a retroreflective
  foil patch. A passing car blocks the reflection. A car spends about 15–30 ms
  under the sensor. The goal is at least 500 Hz sampling per lane.
- **Start light:** one red 5 mm LED per lane board, used F1 style.
- **Nothing touches the track power in v1.** Power electronics come later on
  J302 (§5).

---

## 2. Control module (SideModule PCB, 4 layers)

### 2.1 MCU and clocks

| Item | Value |
|---|---|
| MCU | U201. The schematic symbol says STM32C531CCU6; CubeMX targets the **STM32C542CCU6** (same die and pinout). UFQFPN48. |
| Memory | 256 KB flash, 64 KB SRAM (linker script `stm32c542xc_flash.ld`) |
| Clock | **24 MHz crystal Y201** (±20 ppm, CL 12 pF, 18 pF load caps) → PSI → SYSCLK 144 MHz. USB 48 MHz from PSI/3, so no CRS is needed. |
| Time master | TIM12 runs at 1 MHz from the crystal. Its CH2 drives SYNC (1 kHz). System time = `tick × 1000 + TIM12->CNT` µs. |
| Debug | J201 Tag-Connect TC2030-NL: SWDIO PA13, SWCLK PA14, SWO PB3, NRST |
| BOOT0 | PH2-BOOT0 (pin 44): 10 k pull-down (R201). SW604 BOOT pulls it high through 1 k (R604). The nRF can also drive it (P0.12). |
| Reset | NRST: SW605 to GND, J201, and the nRF P0.14 (**open-drain use only**) |

### 2.2 STM32 pin map (complete)

| Pin | Port | Net | Function / config |
|---|---|---|---|
| 1 | PE2 | BTN3 | SW603 to GND. Input with internal pull-up, EXTI falling. |
| 2 | PC13 | BTN2 | SW602 to GND, same. RTC-domain pin: low speed only. |
| 3 | PC14 | — | Not connected (spare) |
| 4 | PC15 | BTN1 | SW601 to GND, same. RTC-domain pin. |
| 5/6 | PH0/PH1 | HSE_IN/OUT | 24 MHz crystal |
| 10 | PA0 | UART4_TX | J302 pin 8 (Arduino link), AF8 |
| 11 | PA1 | UART4_RX | J302 pin 7. **Not 5 V tolerant.** |
| 12 | PA2 | UART2_TX | J302 pin 5 (power board link), AF7 |
| 13 | PA3 | UART2_RX | J302 pin 4 |
| 14 | PA4 | EXT_PWR_FAULT | J302 pin 2. Input with pull-up, EXTI falling (assumed active low) |
| 15 | PA5 | EXT_PWR_EN | J302 pin 1. Output, init low |
| 16 | PA6 | LANE_PWR_EN | U403 ON. Output, init low. R403 100 k pull-down, so lanes are off by default. |
| 17 | PA7 | SPI_MISO | J303 pin 8 (SPI2) |
| 18 | PB0 | SPI_CS2 | J303 pin 7, GPIO, idle high |
| 19 | PB1 | SPI_CS1 | J303 pin 6, GPIO, idle high |
| 20 | PB2 | SPI_MOSI | J303 pin 5 |
| 21 | PB10 | SPI_SCK | J303 pin 3 |
| 25 | PB12 | USB_FAULT | TPD4S201 FLT* (open drain, active low). Needs the MCU pull-up. |
| 26 | PB13 | LED_STATUS | D601 via 1 k, TIM8_CH2 PWM |
| 27 | PB14 | BUS_INT | J301 pin 9 via 47 Ω. 10 k pull-up to LANE_3V3. Input, EXTI falling. |
| 28 | PB15 | SYNC | J301 pin 7 via 47 Ω. TIM12_CH2, 1 kHz, 50 % |
| 29 | PA8 | BUS_SDA | J301 pin 5 via 47 Ω. I3C1_SDA. 2.2 k pull-up to LANE_3V3 for the open-drain phase. |
| 30 | PA9 | BUS_SCL | J301 pin 3 via 47 Ω. I3C1_SCL, push-pull |
| 31 | PA10 | CHAIN_OUT | J301 pin 2 via 47 Ω. Output, init low. Enumeration line to lane 1. |
| 32/33 | PA11/PA12 | USB_DN/DP | USB FS device. Routed through the TPD4S201 SBU channels for OVP and ESD (same trick as Yuhka V1). |
| 34/37/39 | PA13/PA14/PB3 | SWD | Debug |
| 38 | PA15 | BLE_GPIO2 | nRF P0.01. JTDI: pulled up after reset. Spare. |
| 40 | PB4 | BLE_GPIO1 | nRF P0.04. NJTRST: pulled up after reset. **BOOT_REQ**, active low, read by the future control bootloader at reset (SYSTEM_ARCHITECTURE §9.4). |
| 41 | PB5 | BLE_IRQ | nRF P0.05 → STM. Input with pull-down, EXTI rising. High while a BLE central is connected. |
| 42 | PB6 | UART1_TX | → nRF P0.16 (RX). USART1, 460800 8N1 |
| 43 | PB7 | UART1_RX | ← nRF P0.18 (TX) |
| 44 | PH2 | BOOT0 | See §2.1 |
| 45 | PB8 | LED_ACT | D602 via 1 k, TIM1_CH1 PWM |
| 46 | PB9 | — | Not connected (spare, no timer) |

Spare pins: **PC14, PB9**, and BLE_GPIO2 (PA15).

### 2.3 Power tree

```
USB-C J401 (USB4105, 5.1 k Rd on CC: 5 V sink)
  │ VBUS
  ├─ U401 TPD4S201: OVP (short to VBUS up to 24 V) + ESD on CC1/CC2/SBU (= D+/D-), FLT* → PB12
  ▼
F401 PTC 0.75 A hold / 1.5 A trip
  │ VBUS_F ── J1 pin 2 (2-pin header, pin 1 GND) ── D401 TPD1E10B06 TVS
  ▼
U402 LDL1117S33R LDO, 1.2 A ──► DVDD_3V3: STM32, nRF52805, J303 pin 1, PWR LED D402 (2 k, always on)
  ▼
U403 TPS22917 load switch (ON = LANE_PWR_EN/PA6, default off via R403 100 k)
  CT 10 nF → about 16 ms ramp. QOD with a 1 k external resistor (R404). Always-on reverse blocking.
  ▼
LANE_3V3 ──► J301 pin 1 → all six lanes (about 600 µF of lane capacitance in total)
             LANE LED D403 (2 k, always on), I3C SDA and BUS_INT pull-ups
```

- **Why a separate lane rail:** it keeps the USB inrush small, lets the web app
  power-cycle all lanes, and stops a lane short from browning out the
  controller and the master clock.
- **J1** carries the fused 5 V node (VBUS_F) and GND. Its use is not decided.
  There is no diode between J1 and USB VBUS. A supply fed into J1 while USB is
  plugged in back-feeds the laptop's VBUS through F401. Do not use J1 as an
  input without adding a diode or a power mux.
- **Supply budget:** a 0.75 A PTC feeds a 1.2 A LDO with a 1.7 V drop
  (about 0.5 W at 300 mA in SOT-223). The real current of six lanes is
  **not measured yet**. Measure it at bring-up.

### 2.4 Connectors

**J301 lane bus OUT** (2×5 2.54 mm header). It runs a straight IDC ribbon to
lane 1 J3, pin n to pin n. Every signal has a 47 Ω series resistor. All
pull-ups go to LANE_3V3, so nothing back-powers the lanes.

| Pin | Signal | Pin | Signal |
|---|---|---|---|
| 1 | LANE_3V3 | 2 | CHAIN (PA10 → lane 1 CHAIN_IN) |
| 3 | BUS_SCL (PA9) | 4 | GND |
| 5 | BUS_SDA (PA8), 2.2 k pull-up | 6 | GND |
| 7 | SYNC (PB15) | 8 | GND |
| 9 | BUS_INT (PB14), 10 k pull-up | 10 | GND |

**J302 UART_Ext** (1×8). 3.3 V logic only, no supply pin. Names are from the
board side.

| Pin | Signal | Intended use |
|---|---|---|
| 1 | EXT_PWR_EN (PA5) | Future custom lane power board ("lane transformer") |
| 2 | EXT_PWR_FAULT (PA4) | Same |
| 3 | GND | |
| 4 | UART2_RX (PA3) | Power board link |
| 5 | UART2_TX (PA2) | Power board link |
| 6 | GND | |
| 7 | UART4_RX (PA1) | Arduino with HDMI. **PA1 is not 5 V tolerant**: divide or level-shift a 5 V TX. |
| 8 | UART4_TX (PA0) | Arduino with HDMI |

**J303 SPI_Ext** (1×8, spare): 1 3V3, 2 GND, 3 SCK (PB10), 4 GND, 5 MOSI (PB2),
6 CS1 (PB1), 7 CS2 (PB0), 8 MISO (PA7). SPI2 is generated but not initialised.

**J401** USB-C 2.0 receptacle. **J201** STM SWD. **J501** nRF SWD.

### 2.5 UX: buttons and LEDs

All LEDs are red 5 mm THT parts.

| Ref | Name | Driven by | Notes |
|---|---|---|---|
| D601 | LED_STATUS | PB13, TIM8_CH2 PWM | 1 k |
| D602 | LED_ACT | PB8, TIM1_CH1 PWM | 1 k |
| D603 | BLE_LED | nRF P0.20, GPIO on/off, active high | 1 k. **The nRF52805 has no PWM**, so no dimming. |
| D402 | PWR | DVDD_3V3, hard-wired | 2 k, always on |
| D403 | LANE | LANE_3V3, hard-wired | 2 k, shows lane power |
| SW601–603 | BTN1/2/3 | PC15/PC13/PE2 | Active low, internal pull-ups |
| SW501 | BLE_BIND | nRF P0.00 | To GND. R501 10 k pull-up, so it works before firmware. |
| SW604 | BOOT | BOOT0 via 1 k | Hold BOOT and tap NRST to enter the ROM bootloader (USB DFU) |
| SW605 | NRST | STM reset | |

### 2.6 BLE module (U501)

| Item | Value |
|---|---|
| Module | Raytac MDBT42T-P, **nRF52805**: 192 KB flash, **24 KB RAM**, one BLE connection, 2M PHY capable |
| Power | DVDD_3V3. **LDO mode only**: DCC is not connected, so never set `NRF_POWER->DCDCEN`. |
| Low-frequency clock | **No 32 kHz crystal**. Use LFRC with calibration. P0.00/P0.01 are free as GPIO. |
| Programming | J501 TC2030 SWD with an ST-LINK V3 + OpenOCD (`interface/stlink-dap.cfg`, `target/nordic/nrf52.cfg`). There is no J-Link, so nrfjprog and nrfutil device do not work. |
| Peripherals | One UARTE (taken by the STM link, so no console UART), **no PWM**. TIMER0/RTC0 belong to the BLE controller. |
| Firmware brief | `ControlModule/BleFirmware/BLE_FIRMWARE_GUIDE.md` |
| SDK | nRF Connect SDK v3.4.2 in `~/ncs`. The stock `peripheral_uart` overflows; `FILE_SUFFIX=minimal` fits at about 70 % flash and **98.75 % RAM**. RAM is the constraint. |

| nRF pin | Net | Direction | Function |
|---|---|---|---|
| P0.00 | BLE_BTN | in | Bind button SW501 (10 k pull-up, press = low) |
| P0.01 | BLE_GPIO2 | — | ↔ STM PA15 (spare) |
| P0.04 | BLE_GPIO1 | out | → STM PB4: **BOOT_REQ**, active low |
| P0.05 | BLE_IRQ | out | → STM PB5: high while a central is connected |
| P0.12 | BOOT0 | out | → STM BOOT0 (R604 limits current against SW604) |
| P0.14 | NRST | out, **open drain** | → STM NRST. Only pull low, never drive high. |
| P0.16 | UART1_TX net | in (nRF RX) | ← STM PB6 |
| P0.18 | UART1_RX net | out (nRF TX) | → STM PB7 |
| P0.20 | BLE_LED | out | D603 |
| P0.21/RESET | BLE_NRST | — | J501 only |

### 2.7 Firmware-relevant hardware rules (control)

- **Lanes unpowered (LANE_PWR_EN low):** drive SCL, SDA, SYNC and CHAIN_OUT
  low or Hi-Z. Otherwise about 30 mA flows through the lane ESD diodes. BUS_SDA
  and BUS_INT read low (their pull-ups go to 0 V), so EXTI14 can fire. Ignore
  BUS_INT until lane power is up and settled (about 16 ms ramp plus margin).
- **USB_FAULT** is open drain. Keep the MCU pull-up on.
- **PB4/PA15** come out of reset with JTAG pull-ups (NJTRST/JTDI). Any
  nRF→STM strap on them must be **active low**, so that an nRF in reset (pins
  floating) reads as "no request".
- **The nRF only pulls NRST low.** If both sides drive it, the open-drain rule
  keeps them from fighting.
- **Interrupt priorities** are in `CONTROL_FIRMWARE_NOTES.md`. Power faults
  come first, then the SYNC tick, I3C, BUS_INT, USB, BLE UART, …

---

## 3. Lane module (×6, 2 layers, identical image)

| Item | Value |
|---|---|
| MCU | U1. The symbol says STM32C531CCU6; CubeMX targets the STM32C542CCU6. 256 KB flash, 64 KB SRAM. |
| Clock | **No crystal** (PH0/PH1 carry the sensor I²C). HSI/PSI at 144 MHz, ±1 %. SYNC cancels the drift. |
| Supply | DVDD_3V3 straight from the chain (LANE_3V3 of the control module). C8 100 µF bulk at the input. No regulator on the board. |
| Sensor | U2 VCNL4040M3OE on its own I²C1 (PH0 SDA / PH1 SCL, 2.2 k pull-ups), 7-bit address **0x60**, 400 kHz max. INT open drain on PA0 (10 k pull-up). Sensor VDD runs through a 22 Ω / 4.7 µF RC filter. The IR LED anode is fed **unfiltered** from 3.3 V with its own 4.7 µF (C6). IR pulses reach 200 mA. |
| LED | D1, red 5 mm, PB8 (TIM1_CH1 PWM) via 82 Ω, about 13–15 mA at full duty, active high |
| Debug | J1 TC2030-NL (SWDIO PA13, SWCLK PA14, SWO PB3, NRST). SW1 is the reset button. |
| BOOT0 | PH2-BOOT0 has a 10 k pull-down (R1) and test pad TP1. Not floating. |
| Breakout | SPI1 test pads: TP6 CS (PB4), TP5 MOSI (PB5), TP4 MISO (PB6), TP3 SCLK (PB7, **AF6**). TP2 = 3V3. |

### 3.1 Lane pin map

| Pin | Port | Net | Function |
|---|---|---|---|
| 5 | PH0 | SENS_SDA | I2C1_SDA (AF4) |
| 6 | PH1 | SENS_SCL | I2C1_SCL (AF4) |
| 10 | PA0 | SENS_INT | EXTI0 falling, also WKUP1 |
| 11 | PA1 | CHAIN_OUT | → J2 pin 2 (next lane) |
| 12 | PA2 | BUS_INT | Shared open-drain line, J2/J3 pin 9. Also WKUP2 and TIM2_CH3. |
| 15 | PA5 | SYNC | TIM2_CH1 input capture (filter N8) |
| 25 | PB12 | CHAIN_IN | ← J3 pin 2. Input with pull-down, EXTI both edges. |
| 29 | PA8 | BUS_SDA | I3C1_SDA (AF2). No pull-up on the lane. |
| 30 | PA9 | BUS_SCL | I3C1_SCL (AF2) |
| 40–43 | PB4–PB7 | CS/MOSI/MISO/SCLK | SPI1 breakout pads |
| 45 | PB8 | STATUS_LED | TIM1_CH1 (AF1) |

### 3.2 Lane connectors

- **J3 IN:** right-angle male header on the right board edge. The control
  module, or the upstream lane, plugs in here.
- **J2 OUT:** right-angle female socket on the left board edge. The next lane
  plugs in here.
- **Pinout:** same as J301. Pins pass straight through, except pin 2:
  CHAIN_IN on J3, CHAIN_OUT on J2.
- **Order:** lane 1 is the board nearest the control module.

---

## 4. The lane bus (electrical summary)

| Line | Driven by | Electrical | Use |
|---|---|---|---|
| LANE_3V3 | Control U403 | Switched, slew-limited | Powers every lane |
| BUS_SCL/SDA | Control = I3C controller | I3C SDR, **1 MHz to start**, push-pull SCL. SDA has a 2.2 k pull-up for the open-drain phases. 47 Ω source series. | Register access, IBI, hot-join, DAA |
| SYNC | Control TIM12_CH2 | 1 kHz square wave, rising edge = tick | Shared timebase. Lanes capture it on TIM2_CH1. |
| CHAIN | Each board to the next | Point to point, active high | Enumeration: a lane hot-joins only while its CHAIN_IN is high |
| BUS_INT | Shared | Open drain, 10 k pull-up at the control module | Planned as control → all lanes wake/strobe. IBI carries lane → control. |

Enumeration and timebase details are in `LANE_FIRMWARE_GUIDE.md` §3.2–3.3, and
the protocol is in `SYSTEM_ARCHITECTURE.md`.

**I3C target facts (datasheet):**

- No static address as a target, so **no SETDASA**. CHAIN + hot-join + ENTDAA
  is the only way to enumerate.
- IBI payload is up to 4 bytes.
- The target wakes from Stop only on a reset pattern or a missed start, not on
  an address match. So individual lane naps cannot be woken by address.

---

## 5. Expansion hooks (not populated / future)

| Hook | Hardware present | Intended |
|---|---|---|
| J302 pins 1–5 | EXT_PWR_EN, EXT_PWR_FAULT, USART2, GND | **Custom lane power board.** It must be **galvanically isolated**, or the laptop ground ties the transformers together. Per-lane current sense (deslot, false start, crash), per-lane switch or limit (red phase, stop-and-go, full course yellow). |
| J302 pins 6–8 | UART4, GND | Arduino with HDMI (TV output without a laptop?) |
| J303 | SPI2 + 2 CS + 3V3 | Spare (display, SD card, …) |
| Lane SPI pads | SPI1 on test pads | Experiments (extra lights per lane) |
| PC14, PB9 | Unconnected pins | Spare GPIO |

---

## 6. Boot, ROM bootloader and firmware-update facts

These facts constrain the update design in `SYSTEM_ARCHITECTURE.md` §10.

| Fact | Source | Consequence |
|---|---|---|
| The C5 ROM bootloader supports **USART, FDCAN, USB (DFU) and SPI**. **No I²C or I3C.** | Datasheet (local) | Lanes cannot be updated over the lane bus through ROM. They need SWD or their own I3C bootloader. |
| ROM USART1 uses **PA9 (TX) / PA10 (RX)**, 8E1 | ST community post on C542 boot settings, citing AN2606 (**check against the current AN2606 table**) | On the control module, PA9/PA10 are BUS_SCL and CHAIN_OUT. The nRF sits on **PB6/PB7**, so **the nRF cannot reach the ROM bootloader**. BLE updates need our own STM bootloader. This answers the open question in the BLE sheet note. |
| ROM USB DFU on PA11/PA12 | Same post | BOOT (SW604) + NRST (SW605), then STM32CubeProgrammer over USB: the recovery path for the control module |
| ROM SPI pins (C5): SPI1 PA7/PA6/PA5/PA4, SPI2 PB15/PB14/PB13/PB12 | Search snippet of ST AN6308, **unverified** | Both collide with used pins on the control module. Not an option. |
| `BOOT_SEL` option bit: 1 = BOOT0 comes from the pin, 0 = from the BOOT0 option bit | ST community / RM0522 | Program the option bytes so the BOOT0 pin is honoured (both boards have real pull-downs). Do **not** copy the C542 hub's BOOT_SEL script, which was written for a floating-BOOT0 board. |
| Empty flash → the ROM bootloader starts automatically (EMPTY flag) | ST community | A blank or erased board falls into the ROM bootloader instead of hanging |
| Lane ROM USART1 PA9 = BUS_SCL, PA10 unconnected | Lane netlist | Lane ROM UART is unusable in place |
| nRF52805 | Module | SWD only (J501). 192 KB flash cannot hold a dual-slot DFU. Keep its firmware small and stable. |

---

## 7. Open hardware items

- **VCNL4040 at 15 cm:** force-mode conversion time at the shortest PS_IT, the
  reachable sample rate, the IR current needed, and saturation on the foil.
  Measure on the bench.
- **Lane chain current:** six lanes at the chosen sample rate and LED duty.
  Check LDO heat and PTC margin.
- **J1 purpose and direction** (see §2.3).
- **USB VID/PID** for the "TimeTaker" descriptor (`SYSTEM_ARCHITECTURE.md` §8).
- **Check AN2606** for the C5 USART1 bootloader pins. They decide whether a
  ROM path over the nRF is impossible, as assumed here.
- **Schematic nit:** R501's description field says "BUS_INT open-drain
  pull-up". It is the BLE_BTN pull-up (copy-paste). Cosmetic only.
