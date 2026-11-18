# BLE Module Firmware — Guide for the firmware sessions

Written 2026-11-17. This is the brief for every session that writes the
nRF52805 firmware on the control module. Read it end to end before you write
code. It fits together with
`../SideModuleFirmware/CONTROL_FIRMWARE_GUIDE.md` (the STM side of the same
link, §5.4 there).

**Sources of truth, in this order:**

1. The KiCad schematic, sheet `ble.kicad_sch` in `../SideModulePCB/`.
2. `../../SYSTEM_ARCHITECTURE.md` §9 decides the role of this module. Where
   it and this guide disagree, it wins. §12 of this guide lists the places
   where this guide refines it.
3. `../../HARDWARE_REFERENCE.md` §2.6 has the pin map.
4. This guide.
5. The nRF Connect SDK v3.4.2 sources in `~/ncs/v3.4.2`. The sample this
   firmware grows from is `nrf/samples/bluetooth/peripheral_uart`, with its
   `prj_minimal.conf`.

---

## 1. What the BLE module is

U501, a Raytac MDBT42T-P module with an **nRF52805**: 192 KB flash, **24 KB
RAM**, one BLE connection. It sits between the web app (Web Bluetooth) and
the STM32 (USART1). It is a **boring bridge, and it must stay boring.**

| It does | It never does |
|---|---|
| Advertise as "TimeTaker" | Parse host-protocol frames |
| Bridge the Nordic UART Service (NUS) to the STM, both ways | Store anything about hosts (no bonding, no bind list) |
| Run the bind button (SW501) and the BLE LED (D603) | Decide anything: binding, pairing windows, kicking clients are the STM's |
| Tell the STM about the link (connected, MTU, PHY) | Talk to the lanes, the clock, or anything else |
| Hold the STM's reset and boot lines in their safe state | Update itself in the field: SWD only |

**Why this split:** the nRF can only be reflashed over SWD (J501), and its
RAM is nearly full with the BLE stack alone. Every feature put here is a
feature that is hard to fix later. All intelligence lives in the STM. (v1
has no field update on any board: everything is flashed over SWD,
SYSTEM_ARCHITECTURE §10.)

---

## 2. Where it stands (2026-11-18)

**Cards B1–B4 are built and compile for real; B5 was dropped with the
control bootloader (no field update in v1). Nothing has run on hardware: the
PCBs are still in production.**
Details, numbers and open items: §14. Bench procedures: `BENCH_TESTS.md`.

- Build: `tools/build.sh` (or, in the toolchain shell,
  `west build -b tt_ble/nrf52805 --no-sysbuild -d build .`) builds the
  release image with stage B (MTU 247 + DLE + 2M PHY): flash 110388 B
  (56 %), RAM 22220 B (90 %), **2356 B free**.
- Host tests: `test/run_all.sh` (all green), `test/mutation_check.py`
  (31/31 mutations caught).
- Flash: `tools/flash.sh [--recover]` (ST-LINK + OpenOCD, APPROTECT kept
  open). Bench link tool: `tools/link_tool.py` plays the STM.
- The STM side of the link (`nrf_link`, control task C5) is not built yet.

---

## 3. Hardware facts

### 3.1 The module and the chip

| Item | Value | Consequence |
|---|---|---|
| Chip | nRF52805 (Cortex-M4, 64 MHz, no FPU) | — |
| Memory | 192 KB flash, 24 KB RAM | No DFU, no dual slot. RAM is the budget (§9). |
| Supply | DVDD_3V3 from the board LDO | **LDO mode only: the module's DCC pin is not connected.** Enabling the DC/DC regulator browns the chip out. |
| LF clock | **No 32 kHz crystal** | LFRC with calibration. P0.00/P0.01 are plain GPIO. |
| PWM | **The nRF52805 has no PWM peripheral** (the 52810 has one) | The LED is plain GPIO on/off. No dimming without a software PWM (§7.2). SYSTEM_ARCHITECTURE §9.2 said "LED patterns via PWM": that is not possible on this chip. |
| UART | One UARTE (UARTE0) | It belongs to the STM link. **There is no UART for a console.** Debug output goes over SWD (RTT), or nowhere (§10). |
| Timers | TIMER0 and RTC0 are taken by the BLE controller (MPSL). TIMER1/2, RTC1 are free (RTC1 is Zephyr's system clock). | Use kernel timers (`k_timer`, `k_work_delayable`), not raw peripherals. |
| Debug | J501 TC2030 SWD (SWDIO, SWCLK, nRESET on P0.21, VCC, GND) | ST-LINK V3 + OpenOCD. No J-Link (§4.3). |

### 3.2 Pin map

From the netlist (2026-11-17). The "boot state" column is what each pin does
before the firmware runs and while the nRF is in reset: all GPIO are
disconnected inputs then.

| nRF pin | Net | Use | Config | Boot / reset state, and why it is safe |
|---|---|---|---|---|
| P0.00 | BLE_BTN | Bind button SW501 to GND | Input, no internal pull (R501 10 k pull-up on the board), active low | Floats high via R501 |
| P0.01 | BLE_GPIO2 | ↔ STM PA15. **Spare.** | Leave as a disconnected input | STM side has JTDI pull-up after reset |
| P0.04 | BLE_GPIO1 | **BOOT_REQ** → STM PB4, active low. **Unused in v1** (no STM bootloader) | Output push-pull, **high** (inactive), never changed | Floats; PB4's NJTRST pull-up reads it as "no request" |
| P0.05 | BLE_IRQ | → STM PB5: high while a central is connected | Output push-pull, low at boot | Floats; PB5 has a pull-down → "not connected" |
| P0.12 | BOOT0 | → STM BOOT0 (recovery into the ST ROM only) | **Never driven in v1**: disconnected input | R201 10 k pull-down on the STM side holds it low |
| P0.14 | NRST | → STM NRST | Output **open drain** (S0D1), inactive = released. Never push-pull, not even for one instruction. | Floats; the STM's own NRST pull-up holds it |
| P0.16 | UART1_TX net | nRF **RX** ← STM PB6 | UARTE RXD, input with **pull-up** | Pull-up keeps the line idle while the STM is in reset |
| P0.18 | UART1_RX net | nRF **TX** → STM PB7 | UARTE TXD | STM side gets a pull-up at the next CubeMX pass (control guide §8) |
| P0.20 | BLE_LED | D603 via R603 1 k to GND | Output push-pull, **active high**, low at boot | Off |
| P0.21 | BLE_NRST | J501 reset | Keep `gpio-as-nreset` in UICR | — |

**The net names on the UART are from the STM's side.** UART1_TX is the STM's
TX and therefore the nRF's RX. Get this wrong and nothing breaks, but nothing
works either.

---

## 4. Toolchain, board definition, build and flash

### 4.1 The SDK environment

```bash
nrfutil sdk-manager toolchain launch --ncs-version v3.4.2 --shell
cd ~/Desktop/TimeTaker/ControlModule/BleFirmware
west build -b tt_ble/nrf52805 -d build .          # board name: see §4.2
```

Build the application **as a freestanding app** (it lives outside
`~/ncs`): `west build` works from any folder as long as the toolchain shell
is active and `ZEPHYR_BASE` points into `~/ncs/v3.4.2`.

### 4.2 Custom board: `boards/timetaker/tt_ble/`

Do not build for `nrf52dk/nrf52805`. That board **enables the DC/DC
regulator** (`regulator-initial-mode = <NRF5X_REG_MODE_DCDC>` in its DTS),
points the console at UARTE0, and declares DK buttons and LEDs on other pins.
Start from a copy of its files and change:

| File | Content |
|---|---|
| `board.yml` | name `tt_ble`, vendor `timetaker`, SoC `nrf52805` |
| `tt_ble.dts` | `&reg { regulator-initial-mode = <NRF5X_REG_MODE_LDO>; }`; `&uicr { gpio-as-nreset; }`; `uart0` at 460800 with the pins of §3.2 and `zephyr,console` **removed** from `chosen`; nodes for the button, LED and the four STM lines (`gpio-keys` / `gpio-leds` style, or a small custom binding); `adc`, `i2c0`, `spi0` disabled |
| `tt_ble-pinctrl.dtsi` | `UART_TX` P0.18, `UART_RX` P0.16 with `bias-pull-up` |
| `tt_ble_defconfig` | `CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC=y`, `CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC_CALIBRATION=y`, `CONFIG_GPIO=y`, `CONFIG_SERIAL=y`, no console |
| `board.cmake` | `include(${ZEPHYR_BASE}/boards/common/openocd.board.cmake)` |
| `support/openocd.cfg` | ST-LINK + nRF52 target (§4.3) |

The application's own `boards/` folder is on Zephyr's board search path
automatically, so `-b tt_ble/nrf52805` finds it.

**Sleep clock accuracy:** with the RC oscillator the controller must assume
500 ppm (the Zephyr default for LFRC on nRF52). Do not lower it: a too-tight
value costs connection drops, not range.

### 4.3 Flashing with the ST-LINK V3

There is no J-Link, so `nrfjprog` and `nrfutil device` do not work. OpenOCD
does:

```
# support/openocd.cfg
source [find interface/stlink-dap.cfg]
transport select dapdirect_swd
source [find target/nordic/nrf52.cfg]
```

```bash
west flash -d build --runner openocd --openocd ~/.local/bin/openocd
```

- **APPROTECT:** newer nRF52 revisions keep the debug port locked after a
  reset unless the firmware opens it. If OpenOCD reports a locked access
  port, run `nrf52_recover` from OpenOCD once (it mass-erases the chip).
  Keep the NCS default APPROTECT handling during development so SWD stays
  open. Whether to lock a release image is an open question (§13).
- **Erase before the first flash** of a board: the module may come with a
  Raytac test image.

---

## 5. The nRF ↔ STM link

Both chips run the same codec as the host protocol: COBS + CRC-16, with
**one leading channel byte** instead of the host's 4-byte header.

### 5.1 Framing

```
link frame = COBS( channel u8 | body | crc16 ) 0x00
crc16      = CRC-16/CCITT-FALSE over channel|body, little-endian
```

- **UART:** USART1 / UARTE0, **460800 8N1**, no hardware flow control (there
  are no RTS/CTS wires).
- **Baud error:** the nRF's 460800 setting runs about 0.8 % slow (it cannot
  hit 460800 exactly); the STM's is within 0.2 %. About 1 % between them is
  fine for 8N1. Check the real rates on the scope at bring-up.
- **The codec is the STM's `frame_codec.c/h`,** compiled directly from
  `../SideModuleFirmware/LapCounterControlModule_cmake/app/modules/` by this
  firmware's `CMakeLists.txt`. It is pure C99 with only `<stdint.h>` and
  `<stdbool.h>`. **Do not copy it**: one file, one set of host tests.
- **Resync:** a corrupt frame (CRC, COBS, overflow) is dropped and counted;
  the decoder resyncs at the next 0x00. Nothing else happens.

### 5.2 Channels

| Channel | Value | Body | Direction |
|---|---|---|---|
| `DATA` | 0 | **Opaque host-protocol bytes**, 1–244 per frame | both |
| `CTRL` | 1 | `type u8` + a fixed-size message (§5.3) | both |

**DATA is a byte stream, not a frame carrier.** The nRF never looks inside.

- **BLE → STM:** every NUS write the web app makes becomes exactly one DATA
  frame with the written bytes. One host frame may arrive in many writes;
  one write may hold the end of one host frame and the start of the next.
  The STM's session decoder sorts that out (COBS inside COBS).
- **STM → BLE:** each DATA frame becomes one or more NUS notifications. The
  STM sends at most 244 bytes per DATA frame (the ATT payload at MTU 247).
  If the current MTU is smaller, **the nRF splits** the frame into
  notifications of `bt_nus_get_mtu()` bytes. The STM never needs to know
  the MTU for correctness; it learns it from `LINK_UP` only for stream
  budgeting.
- **CTRL is interleaved with DATA only at frame boundaries,** which is
  automatic: each is its own link frame.

### 5.3 CTRL messages

All values are little-endian, all structs packed. Types 0x01–0x3F go nRF →
STM, 0x41–0x7F go STM → nRF. **`ble_link_protocol.h` is the authority**
(§5.6); this table is its starting point.

**nRF → STM:**

| Type | Name | Fields | When |
|---|---|---|---|
| 0x01 | `NRF_HELLO` | `linkMajor u8, linkMinor u8, fwMajor u8, fwMinor u8, fwPatch u8, credits u8, resetCause u8, reserved u8, fwGitRev u32, btAddr u8[6]` (18 B) | At boot, and in answer to `STM_HELLO` |
| 0x02 | `LINK_UP` | `attMtu u16, txPhy u8, rxPhy u8, intervalUnits u16 (1.25 ms), latency u16, timeout10ms u16, maxTxOctets u16` (12 B) | On connect, and again on every MTU, PHY, data length or parameter change |
| 0x03 | `LINK_DOWN` | `hciReason u8` | On disconnect |
| 0x04 | `BUTTON` | `press u8` (`SHORT` 1, `LONG` 2, `VERY_LONG` 3: the same values as the host protocol's `pressKind`) | On release (§7.1) |
| 0x05 | `CREDIT` | `count u8` | When DATA slots free up (§5.4) |
| 0x06 | `STATS` | `rssi i8, reserved u8, dataDropped u16, linkCrcErrors u16, uartErrors u16` (8 B) | Once a second while connected (task B4; optional before) |

**STM → nRF:**

| Type | Name | Fields | Effect |
|---|---|---|---|
| 0x41 | `STM_HELLO` | `linkMajor u8, linkMinor u8` | nRF answers `NRF_HELLO`, then `LINK_UP` if connected |
| 0x42 | `UI_STATE` | `session u8` (`NONE` 0, `UNBOUND` 1, `BOUND` 2), `pairWindow u8` (0/1) | Sets the LED (§7.2). The nRF keeps the last value; `LINK_DOWN` resets `session` to NONE locally. |
| 0x43 | `CUE` | `kind u8` (`BIND_ACCEPTED` 1, `BINDS_CLEARED` 2) | Plays a one-shot LED pattern over the current one |
| 0x44 | `KICK` | `reason u8` | Disconnects the central (HCI reason 0x13, "remote user terminated") |
| 0x45 | `ADVERTISE` | `on u8` | Stops (0) or resumes (1) advertising; a connected central is kicked on 0. Default after boot: on. |

- **Unknown types are ignored** (counted). That is how MINOR versions stay
  compatible in both directions.
- **A CTRL body shorter than its type needs** is dropped. A longer one is
  accepted and the extra bytes are ignored (a newer MINOR appended fields).

### 5.4 Flow control: credits (STM → nRF only)

The nRF has a few KB of RAM for everything; the STM has 64 KB. So only the
STM → nRF direction needs flow control.

- **The nRF owns `N` DATA slots** of 244 B (start with N = 4, about 1 KB).
  `NRF_HELLO.credits` is the number of slots **free at that moment**
  ("granted now"); slots still busy return their credit later with
  `CREDIT`. The STM sets its credit counter to the HELLO value on every
  `NRF_HELLO` and never clamps later CREDITs to it. At boot the value is
  always N, so a reset also resyncs.
- **Each DATA frame from the STM uses one credit.** The STM sends no DATA
  frame without a credit.
- **The nRF returns the credit** (`CREDIT{1}`, or batched) when the slot is
  free: after the **sent callback of the last notification** made from it
  (`bt_gatt_notify_cb` with a completion function), not when the call
  returns. That bounds what is in flight in the BLE stack to N frames.
- **Not connected:** a DATA frame is dropped at once and its credit returned.
  The STM does not send DATA while BLE_IRQ is low, so this is only a race at
  disconnect.
- **nRF → STM needs no credits:** the STM drains its 1 KB ring every loop
  pass (< 1 ms), the UART moves 46 bytes/ms, and BLE writes from a browser
  are small requests. If the TX ring to the STM is ever full, the nRF drops
  the write and counts it (`STATS.dataDropped`); the host protocol's
  request retry covers it.

### 5.5 Versions and resync

- **Link protocol 0.1** (`linkMajor` 0, `linkMinor` 1).
- **MAJOR mismatch:** the STM refuses DATA (drops it, counts it) and keeps
  CTRL working, so the nRF still advertises and the app can still read the
  versions in NODE_LIST and report "BLE module needs a reflash"
  (SYSTEM_ARCHITECTURE §10.3).
- **Either side may reboot alone.** The nRF sends `NRF_HELLO` at boot; the
  STM sends `STM_HELLO` at boot. Whoever is up first gets the other's hello.
  After an nRF reboot, any connection is gone and the STM's BLE session
  resets on BLE_IRQ low.

### 5.6 The shared header: `ble_link_protocol.h`

- Plain C: channel values, CTRL types, packed structs with `_Static_assert`
  on their sizes, `BLE_LINK_DATA_MAX` (244), link version.
- **Byte-identical in two places:** `ControlModule/BleFirmware/src/` and the
  STM's `app/config/`. A host test in each tree compares the files. Write it
  in task B2, in one change, into both trees.

---

## 6. The BLE side

### 6.1 Advertising

| Item | Value |
|---|---|
| Name | **"TimeTaker"** (complete local name), in the **scan response** |
| Advertising data | Flags + the **128-bit NUS service UUID**. Name and UUID do not both fit in 31 bytes. Chrome merges the scan response, so a filter on both still works. |
| Type | Connectable, undirected. No extended advertising (it costs RAM and Web Bluetooth does not need it). |
| Interval | 100 ms. The box is USB-powered; fast discovery is worth more than µA. |
| When | Always while no central is connected, unless the STM sent `ADVERTISE{0}`. Restart at once after a disconnect. |

### 6.2 GATT: the Nordic UART Service

- **NUS** from NCS (`CONFIG_BT_NUS=y`, `bt_nus_init()`, `bt_nus_send()` or,
  better here, `bt_gatt_notify_cb()` on the TX characteristic for the
  completion callback of §5.4).
- Standard UUIDs (6E400001-…), so the nRF Connect phone app and Nordic's
  UART tools can talk to it at the bench without the web app.
- **RX characteristic:** write and write-without-response. Every write
  becomes one DATA frame (§5.2).
- **TX characteristic:** notify. Data is only sent after the central enabled
  notifications (`send_enabled` callback); before that, DATA from the STM
  is dropped and credited.
- **No other services** except the mandatory GAP/GATT ones. Keep the GATT
  service-changed and caching features off (`prj_minimal.conf` does).

### 6.3 Connection parameters, MTU, data length, PHY

| Item | Request | Note |
|---|---|---|
| Connection interval | 7.5–30 ms | The central (Chrome) decides. Android and Windows often pick 30–50 ms. |
| Peripheral latency | 0 | |
| Supervision timeout | 4 s | A dropout of a few seconds is not a disconnect |
| ATT MTU | 247 | Needs `CONFIG_BT_L2CAP_TX_MTU=247` and larger RX buffers. **Gated on RAM (§9).** |
| Data length extension | 251 B | Same. Without it, a 244 B notification is cut into 27 B link packets: slower, still correct. |
| PHY | 2M preferred | `CONFIG_BT_CTLR_PHY_2M` costs flash, not much RAM. Gated on §9 like the rest. |

**The bridge must work with the smallest settings** (MTU 23, no DLE, 1M
PHY). Everything above is throughput, and v1 traffic is small: a crossing
event is about 40 bytes on the wire. Only the raw-sample diagnostics stream
wants a few KB/s, and the STM caps it on BLE.

### 6.4 Security

**None at the BLE layer.** No pairing, no bonding, no SMP. Binding is done
by the STM at the application layer with the web app's `clientId`
(SYSTEM_ARCHITECTURE §9.5).

- This is a RAM decision as much as a design one: `peripheral_uart` enables
  SMP and bonding by default (`CONFIG_BT_NUS_SECURITY_ENABLED=y` pulls in
  SMP, settings and NVS). Turning it off frees the ECC thread's stack, the
  SMP buffers and the settings backend (§9).
- **What it protects against, honestly:** a curious phone joining the race.
  It does not stop someone who sniffs the radio. If that ever matters, LE
  Secure Connections bonding is the upgrade, if RAM allows (§13).

---

## 7. Button, LED and the STM control lines

### 7.1 The bind button (SW501, P0.00)

Sample it every 10 ms from a kernel timer (no GPIO interrupt needed; the
loop is cheap and debouncing is trivial). Debounce: 3 equal samples.

| Held for | On release | Local LED feedback while held |
|---|---|---|
| < 1 s | `BUTTON{SHORT}` | one blink at release |
| 1–2 s | nothing (dead zone against ambiguous presses) | — |
| 2–10 s | `BUTTON{LONG}` | from 2 s: fast blink (preview of "pairing") |
| 10–30 s | `BUTTON{VERY_LONG}` | from 10 s: steady on ("release to clear all bindings") |
| > 30 s | nothing (stuck button) | back to normal |

- **The nRF only classifies and reports.** Opening the pairing window and
  clearing the bind list are the STM's decisions (control guide §5.4). The
  STM then sends `UI_STATE` / `CUE`, and the LED follows.
- **If the STM is silent** (no frame from it for 5 s), the press is still
  reported; it just has no effect. The LED shows that nothing happened.

### 7.2 The BLE LED (D603, P0.20)

**No PWM on this chip (§3.1),** so the LED is on or off. "Steady, dim" in
SYSTEM_ARCHITECTURE §9.5 becomes **steady on**. If dimming is ever wanted,
Zephyr's `nordic,nrf-sw-pwm` driver can do it with a free TIMER + PPI +
GPIOTE, at a small RAM cost; not in v1.

The nRF renders one pattern from what it knows (link state) and what the STM
tells it (`UI_STATE`, `CUE`). First matching row wins:

| Condition | Pattern |
|---|---|
| A `CUE` is playing | `BIND_ACCEPTED`: 3 quick flashes. `BINDS_CLEARED`: one 1 s flash. |
| Button held (§7.1) | the hold feedback |
| `ADVERTISE{0}` (BLE disabled) | off |
| `pairWindow = 1` | fast blink, 5 Hz |
| Connected, `session = BOUND` | steady on |
| Connected, `session = UNBOUND` or `NONE` | double blink every 2 s |
| Advertising, nobody connected | short blink every 3 s |

Patterns run from one `k_timer` at 50 ms resolution. The LED code is a
table, not logic.

### 7.3 STM reset and boot lines (`stm_ctrl`)

- **NRST (P0.14), open drain.** Released at init. `stmCtrlResetPulse()`
  pulls it low for 10 ms and releases it; nothing calls it in v1. Never
  drive it high.
- **BOOT_REQ (P0.04), active low:** driven high (inactive) at init and never
  changed. It was the strap into the control bootloader; v1 has no
  bootloader (every board is flashed over SWD), so the line is unused.
- **BOOT0 (P0.12)** stays undriven in v1. Entering the ST ROM bootloader
  needs a USB cable anyway (HARDWARE_REFERENCE §6), and then the user can
  press SW604 BOOT + SW605 NRST.
- **Removed 2026-11-18:** `stmCtrlResetIntoBootloader()` and task B5 (the
  button trigger for it), together with the control bootloader. Both are at
  git tag `archive/bootloader-update`.

---

## 8. Firmware structure (NCS v3.4.2, Zephyr)

```
ControlModule/BleFirmware/
  BLE_FIRMWARE_GUIDE.md   this file
  CMakeLists.txt          app + ../SideModuleFirmware/.../app/modules/frame_codec.c
  prj.conf                from prj_minimal.conf, trimmed further (§9)
  boards/timetaker/tt_ble/  custom board (§4.2)
  src/
    main.c                init order, the event loop, the state (§8.2)
    ble_link.c/h          advertising, NUS, connection callbacks, notify with completion
    stm_link.c/h          UARTE async RX/TX, link framing, DATA slots, credits, CTRL encode/decode
    bind_ui.c/h           button classification (§7.1), LED pattern table (§7.2)
    stm_ctrl.c/h          BLE_IRQ, BOOT_REQ, NRST lines (§7.3)
    ble_link_protocol.h   shared with the STM (§5.6)
    fw_version.h          generated: MAJOR.MINOR.PATCH + git rev (like the STM's version stamp)
  test/                   host tests (§11.1)
```

### 8.1 Threads

Every thread is a stack in RAM. Keep exactly these:

| Thread | Source | Stack |
|---|---|---|
| main | ours: the event loop | ~1 KB (measure) |
| BT RX, BT TX (HCI) | Zephyr host | from `prj_minimal.conf`: 1024 / 640 |
| MPSL work, system workqueue | NCS / Zephyr | 256 / ~1 KB (we put no long work on it) |
| idle, ISR stack | kernel | 128 / 1024 |

**Do not keep the sample's `ble_write_thread`** (its second NUS thread).
With SMP gone, the "BT ECC" thread disappears too.

### 8.2 Data paths and the event loop

```
UARTE0 RX (DMA, double buffer 2×64 B) ──UART_RX_RDY (ISR)──► rx ring 512 B ──┐
                                                                             ▼
main loop  k_poll(sem) ◄── signals from: UART RX, button timer, LED timer, BLE callbacks
   │
   ├─ stm_link: decode link frames from the rx ring
   │     DATA → copy into a free slot → ble_link: notify (split to MTU) → on sent: CREDIT
   │     CTRL → UI_STATE / CUE → bind_ui; KICK / ADVERTISE → ble_link; STM_HELLO → NRF_HELLO
   │
   └─ events from ble_link (connected, disconnected, MTU, PHY, write)
         connected → BLE_IRQ high, LINK_UP          disconnected → BLE_IRQ low, LINK_DOWN
         NUS write (BT RX thread) → encode one DATA frame into the tx ring → UARTE async TX
```

- **Never block the BT RX thread.** The NUS write callback only encodes into
  the TX ring (`k_spinlock`-protected) and kicks the UART; if the ring is
  full it drops and counts.
- **UARTE async TX** needs its buffer to stay valid until `UART_TX_DONE`.
  Send from a contiguous region of the TX ring and advance on `TX_DONE`.
- **UART errors** (`UART_RX_STOPPED`, framing, overrun): count, re-enable RX
  at once. The link decoder resyncs on its own.
- **No dynamic allocation** in our code (`CONFIG_COMMON_LIBC_MALLOC=n` stays).

---

## 9. RAM budget

The sample's minimal build sits at 98.75 % of 24 KB. Our firmware must have
**at least 1.5 KB free** after everything, or it will fail on the first
unforeseen buffer.

**What frees RAM, in the order to try:**

| Change | Frees | Note |
|---|---|---|
| `CONFIG_BT_NUS_SECURITY_ENABLED=n`, `CONFIG_BT_SMP=n` | the BT ECC thread (~1.1 KB stack) and SMP state | §6.4 decision |
| `CONFIG_BT_SETTINGS=n`, `CONFIG_SETTINGS=n`, `CONFIG_NVS=n`, `CONFIG_FLASH=n`, `CONFIG_FLASH_MAP=n` | settings and NVS buffers | Nothing is persisted on the nRF |
| `CONFIG_BT_MAX_PAIRED=0` | key storage | |
| Drop `ble_write_thread`, `CONFIG_DK_LIBRARY=n` | one 512 B–1 KB stack, DK state | §8.1 |
| `CONFIG_HEAP_MEM_POOL_SIZE` down to what the BT host needs (try 0, then 512) | up to 1.5 KB | The sample sets 1536 |
| Our UART buffers small: RX 2×64 + ring 512, TX ring 512, DATA slots 4×244 | — | Our own total ≈ 2.1 KB; this is the knob if RAM runs out (fewer slots) |

**What costs RAM, to add only with headroom (stage B):**

| Feature | Cost (estimate, measure) |
|---|---|
| ATT MTU 247 (`L2CAP_TX_MTU`, `BUF_ACL_RX_SIZE` 251) | ~1–2 KB of larger RX buffers |
| DLE (`BT_DATA_LEN_UPDATE`, `BT_BUF_ACL_TX_SIZE` 251, controller data length 251) | ~1 KB TX buffers + controller memory |
| 2M PHY (`BT_PHY_UPDATE`, `BT_CTLR_PHY_2M`) | mostly flash |
| RTT logging in debug builds | buffer size (keep ≤ 512 B) |

**Method:**

1. Stage A: the bridge with all the "frees" applied and the defaults of
   `prj_minimal.conf` otherwise. Measure with `west build -t ram_report` and
   the linker map. Run the thread analyzer once (temporarily, with RTT) to
   size the stacks, as the sample's comment shows.
2. Stage B: add MTU 247, then DLE, then 2M PHY, one at a time, measuring
   each. Keep each only if ≥ 1.5 KB stays free.
3. Write the numbers into §14 (status) every time.

---

## 10. Robustness

- **Watchdog:** the nRF WDT at 4 s, fed from the main loop (`CONFIG_WATCHDOG`
  plus a few lines). A wedged nRF otherwise needs a power cycle of the whole
  box. Pause it while halted by the debugger.
- **Reset cause** goes into `NRF_HELLO.resetCause` (POWER_ON, PIN, WATCHDOG,
  LOCKUP, SOFT). The STM shows it in NODE_LIST and logs it.
- **The STM can be absent** (in reset, unflashed, being flashed). The
  nRF keeps advertising and bridging; DATA to a silent STM just goes
  nowhere and the web app's HELLO times out. Nothing on the nRF waits for
  the STM.
- **Asserts:** `CONFIG_ASSERT=n` in release (the sample's choice). In debug
  builds, a fault resets via the default fatal handler; keep it that way, so
  a debug image never hangs the box.
- **Debug output:** UARTE0 belongs to the STM, so the console is RTT over
  SWD (OpenOCD can serve RTT) in debug builds only, with a small buffer. Or
  no logging at all: the `STATS` message and the STM's NODE_LIST carry the
  counters that matter.

---

## 11. Testing and bring-up

### 11.1 Host tests (`test/run_all.sh`, like the STM's)

Pure C, built with the laptop's `cc`, `-Wall -Wextra -Werror`, one broken
rule per rejection case:

- **Link framing:** DATA and CTRL frames through `frame_codec` and back, a
  244 B DATA frame at the wire bound, corrupt-frame resync.
- **CTRL pack/parse:** every message at its exact size, short bodies
  rejected, long bodies accepted, unknown types ignored.
- **Credits:** slot accounting (take, free on completion, drop-and-credit
  while disconnected, reset on hello).
- **Button classifier:** every row of §7.1 including the dead zone and the
  stuck button, as a pure function of sample sequences.
- **LED pattern selection:** every row of §7.2, first match wins.
- **`ble_link_protocol.h` copy check** against the STM tree's copy.

Put the classifier, the pattern selection and the credit accounting in pure
modules (no Zephyr includes) so these tests can exist.

### 11.2 Bench tools that need no web app

- **The nRF Connect app** (Android/iOS) connects to NUS, enables
  notifications and writes bytes: enough to test advertising, connection and
  the BLE half of the bridge before the STM speaks the link.
- **An STM echo knob:** a control firmware debug switch
  (`CTRL_ENABLE_NRF_LINK_ECHO` in `ctrl_behavior.h`, control task C5) that
  answers every DATA frame with the same bytes. Then the phone app sees its
  own writes come back through both chips.

### 11.3 Task cards

| Card | Job | Needs | Done when |
|---|---|---|---|
| **B1** | Custom board (§4.2), stage A `prj.conf`, advertising + NUS, BLE_IRQ, LED patterns for advertising/connected, the button classifier reporting nothing yet, flash with OpenOCD | a board with J501 reachable | The phone sees "TimeTaker", connects, BLE_IRQ goes high on the scope, the LED follows; RAM report written into §14 |
| **B2** | `ble_link_protocol.h` in both trees, `stm_link` (UART, framing, DATA slots, credits, all CTRL messages), host tests of §11.1 | B1 | With the STM echo knob (control C5's first step), phone writes come back through both chips; a 10 KB burst passes without loss |
| **B3** | Bind UI end to end: button → `BUTTON`, `UI_STATE`/`CUE` → LED, `KICK`, `ADVERTISE` | B2 + control C5 | The binding flow of SYSTEM_ARCHITECTURE §9.5 works with the web app |
| **B4** | Stage B (MTU, DLE, 2M PHY as RAM allows), `STATS`, watchdog | B2 | Throughput measured and written into §14; WDT resets a deliberately hung loop |
| ~~B5~~ | Removed 2026-11-18 with the control bootloader (no field update in v1; tag `archive/bootloader-update`) | | |

---

## 12. Refinements to SYSTEM_ARCHITECTURE §9

This guide changes the architecture's draft in these places (applied to
SYSTEM_ARCHITECTURE §9 on 2026-11-17):

1. **No PWM on the nRF52805.** The LED is GPIO; "steady, dim" becomes
   "steady on" (§3.1, §7.2).
2. **The STM owns the pairing window.** The draft had the nRF report
   `PAIR_WINDOW{open/closed}` and the STM send `OPEN_PAIR_WINDOW`. Now the
   nRF only reports button presses and renders `UI_STATE`, so there is one
   owner of every bind decision and the nRF holds no timer that matters
   (§5.3, §7.1).
3. **New CTRL messages:** `STM_HELLO`, `UI_STATE` (replaces
   `SESSION_STATE`), `CUE`, `CREDIT`, `STATS`; `CONNECTED` and
   `DISCONNECTED` are named `LINK_UP` / `LINK_DOWN` (§5.3).
4. **Credit-based flow control** STM → nRF (§5.4). The draft had none, and
   without RTS/CTS the nRF's tiny buffers need it.
5. **The nRF splits DATA to the MTU**, so the STM never depends on the
   current MTU for correctness (§5.2).

---

## 13. Open questions

1. **RAM after stage A and B** (§9): the real numbers decide MTU, DLE and
   2M PHY. Measure in B1/B4.
2. **LE Secure Connections later?** Only if RAM allows after stage B, and
   only if sniffing becomes a real concern (§6.4).
3. **APPROTECT for release images:** lock the debug port or not. Locking
   protects nothing of value here and makes a recovery need `nrf52_recover`
   (a full erase). Leaning: leave it open.
4. **Advertised name:** "TimeTaker" for every box. If two boxes ever sit in
   one room, append 4 hex digits of the BT address ("TimeTaker-3F2A"); the
   web app filters by name prefix, so it costs nothing to change later.
5. **Real UART baud error** on both chips (§5.1): check on the scope in B2.
6. **Web Bluetooth write size:** Chrome does not expose the negotiated MTU.
   The web app's `bleTransport` should write in chunks of 20 B until it is
   shown that larger writes work on every target platform (Windows, Android,
   ChromeOS, macOS). Host → device traffic is small, so this costs little.

---

## 14. Firmware status

### 2026-11-18 — B1–B4 built (no hardware yet)

**What exists** (`ControlModule/BleFirmware/`)

| Path | Content |
|---|---|
| `boards/timetaker/tt_ble/` | Custom board: LDO, LFRC + calibration (500 ppm), `gpio-as-nreset`, UARTE0 460800 on P0.18 TX / P0.16 RX (pull-up) with TIMER1 RX byte counting, no console, button/LED nodes, `stm_lines` node (binding in `dts/bindings/`), OpenOCD runner (`support/openocd.cfg`) |
| `prj.conf` | Stage A (§9 "frees" applied); `stage_b_{mtu,dle,phy}.conf` are stage B, all on by default via `TT_STAGE_B_CONF` in `CMakeLists.txt` |
| `debug.conf`, `wdt_selftest.conf` | RTT logging + thread analyzer; WDT self-test (bench T10 only) |
| `src/main.c` | Init order, k_poll event loop, bridge state, WDT 4 s, reset cause, STATS 1/s |
| `src/ble_link.c` | Advertising §6.1, NUS, conn/MTU/PHY/DLE callbacks, notify with completion tagged by slot + connection generation, RSSI |
| `src/stm_link.c` | UARTE async RX (2×64 B DMA → 512 B ring), TX ring 512 B under a spinlock, frame_codec, counters |
| `src/bind_ui.c`, `src/stm_ctrl.c` | 10 ms button timer + LED renderer; BLE_IRQ, BOOT_REQ (held high, unused), NRST open drain released |
| Pure modules | `button_classifier`, `led_table`, `data_slots` (credits + MTU split), `link_msg` (framing, CTRL pack/parse, KICK reason, reset cause), `byte_ring` |
| `cmake/version*.cmake` | `fw_version.h`: 0.1.0 + git rev (+dirty), as the STM does |
| `test/` | `run_all.sh`, `mutation_check.py` |
| `tools/` | `build.sh`, `ram_stages.sh`, `flash.sh`, `link_tool.py` |

**RAM / flash per stage** (`tools/ram_stages.sh`, 24576 B RAM, keep ≥ 1536 B free)

| Stage | Flash | RAM | Free RAM | Kept |
|---|---|---|---|---|
| A | 106596 B | 18688 B | 5888 B | — |
| A + MTU 247 | 106596 B | 20876 B | 3700 B | yes |
| + DLE 251 | 108572 B | 22116 B | 2460 B | yes |
| + 2M PHY | 110388 B | 22220 B | **2356 B** | yes (release) |
| debug.conf on top | 131208 B | 24308 B | 268 B | measurement only |

Our own static RAM ≈ 2.7 KB (4 slots 1020 B, rings 1024 B, DMA 128 B,
decoder 249 B, TX scratch 251 B). The slot array is sized from
`CONFIG_TT_DATA_SLOTS` (an 8-slot array cost 1 KB before that fix). Largest
other users: SDC memory 2836 B, HCI RX pool 1536 B, five 1 KB stacks.

**Host tests** (`test/run_all.sh`, cc `-Wall -Wextra -Werror -DRELEASE`):
contract copy check (identical), `host_button` 44, `host_led` 43,
`host_slots` 45, `host_link_msg` 95 (through the control tree's
`frame_codec.c`), `host_byte_ring` 19 checks, `link_tool.py selftest`. All
pass. `test/mutation_check.py`: 31 one-rule mutations, all caught (a
mutation counts only if it still compiles and a check fails).

**Decisions taken here**

- `NRF_HELLO.credits` = slots free at that moment; slots busy then return
  their credit with `CREDIT` later. Invariant: STM credits + owed + used = N.
  At boot this is always N.
- Notifications in flight are capped at `BT_ATT_TX_COUNT − 1` (= 1), so the
  main thread never blocks in `bt_gatt_notify_cb` (it waits K_FOREVER for an
  ATT buffer outside the system work queue) and the host keeps a buffer for
  its responses. Throughput ≈ one notification per connection event; raise
  `BT_ATT_TX_COUNT` if T9 shows it is too slow (RAM allows).
- A disconnect frees every slot at once (ATT drops callbacks of failed
  notifications); late completions are discarded by a connection generation.
- Values out of range (UI_STATE session > 2 / pairWindow > 1, CUE kind,
  ADVERTISE on > 1) are ignored and counted like unknown types. KICK reasons
  the controller would refuse become 0x13.
- `CONFIG_RESET_ON_FATAL_ERROR=y`: a fault reboots at once.
- `support/openocd.cfg` uses `interface/stlink.cfg` + `transport select swd`
  (xPack 0.12.0-7 deprecates the §4.3 lines; it still selects dapdirect,
  checked). `tools/flash.sh` writes UICR.APPROTECT = 0x5A after programming,
  because `nrf52_recover` leaves it erased and newer nRF52 revisions would
  lock SWD at the next reset.
- System work queue stack 1024 B (sample: 1536); the sample measured 136 B.
  `BT_HCI_TX_STACK_SIZE` is unused in this Zephyr; the TX processor has 1 KB.
- DLE and 2M are started by the peripheral (`BT_AUTO_DATA_LEN_UPDATE`,
  `BT_AUTO_PHY_PERIPHERAL_2M`); the MTU exchange is the central's.

**Unverified until hardware** (most risky first): APPROTECT/flash flow with
the ST-LINK; LDO/LFRC bring-up; UARTE async RX with TIMER1 at 460800 and
the real baud error; stack sizes (run the debug image's thread analyzer);
notify/credit timing under load and throughput; advertising restart after
disconnect; RSSI read; LED/button timing by eye; WDT reset.

**Open:** release APPROTECT (§13 Q3); measured
stacks, baud error and throughput go here after the bench (BENCH_TESTS.md).
