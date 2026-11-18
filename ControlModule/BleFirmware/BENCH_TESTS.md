# BLE Module — Bench Tests

Written 2026-11-17, before any board existed. Nothing here has been run
yet. These are the procedures for the day the control module PCBs arrive,
in order: each test assumes the ones before it passed. Record results (date,
board serial, pass/fail, numbers) in `BLE_FIRMWARE_GUIDE.md` §14.

Every test has: **Setup**, **Steps**, **Expect**, and **If it fails, check
first**. Pin and net names are from `BLE_FIRMWARE_GUIDE.md` §3.2. The UART
nets are named from the STM's side: **UART1_TX = nRF RX (P0.16)**, **UART1_RX
= nRF TX (P0.18)**.

| Test | What | Needs the STM firmware? |
|---|---|---|
| T0 | Tools, images, RTT | no |
| T1 | Flashing with ST-LINK + OpenOCD, APPROTECT recovery | no |
| T2 | Supply, regulator, clocks, reset pin | no |
| T3 | Advertising in the nRF Connect phone app | no |
| T4 | Connect: BLE_IRQ on the scope, MTU / PHY / DLE | no |
| T5 | The UART link with `link_tool.py` (STM held in reset) | no |
| T6 | LED patterns | T5 tool, or control C5 |
| T7 | Button classes | T5 tool, or control C5 |
| T8 | Echo through the STM (`CTRL_ENABLE_NRF_LINK_ECHO`) | control C5 first step |
| T9 | Throughput | T5 tool or T8 |
| T10 | Watchdog | no |
| T11 | Binding with the web app | control C5 + web app |
| T12 | STM reset lines (B5 groundwork) | no (scope only) |

---

## T0. Tools and images

**T0.1 Hardware on the bench**

- Control module, powered from its normal USB-C input (the nRF runs from
  DVDD_3V3 of the board LDO; never power the module from the ST-LINK alone).
- ST-LINK V3 (V3SET/V3MINI) with a **TC2030-CTX-NL** (or -IDC) cable into J501.
  The ST-LINK's target voltage sense pin must see the board's 3.3 V.
- 2-channel scope (or a logic analyzer with a UART decoder at 460800 8N1).
- A 3.3 V USB-UART adapter (FT232/CP2102) with three flying leads, for T5.
- A phone with **nRF Connect for Mobile** (Android or iOS).
- A Chrome/Edge laptop with the web app, for T11.

**T0.2 Software**

```bash
# all from ControlModule/BleFirmware/
tools/build.sh                         # release image -> build/zephyr/zephyr.hex
test/run_all.sh                        # host tests, must say ALL SUITES PASSED
BUILD=build-debug tools/build.sh -- -DEXTRA_CONF_FILE=debug.conf      # RTT log build
BUILD=build-wdt   tools/build.sh -- -DEXTRA_CONF_FILE=wdt_selftest.conf  # T10 only
```

`pip install pyserial` if `tools/link_tool.py` complains.

**T0.3 Which image for which test**

- Release (`build/`): T1–T9, T11, T12. This is what ships.
- Debug (`build-debug/`, RTT logging + thread analyzer): first bring-up if
  something misbehaves, and once for the stack measurement (T0.4).
- WDT self-test (`build-wdt/`): T10 only. Reflash release afterwards.

**T0.4 RTT logging (debug image only)**

UARTE0 belongs to the STM, so the only console is SEGGER RTT over SWD.
OpenOCD serves it:

```bash
~/.local/bin/openocd -f boards/timetaker/tt_ble/support/openocd.cfg \
  -c "init" -c "rtt setup 0x20000000 0x6000 \"SEGGER RTT\"" \
  -c "rtt start" -c "rtt server start 9090 0"
# second terminal:
nc localhost 9090
```

Expect `TimeTaker BLE bridge 0.1.0+<rev>, link 0.1, reset cause <n>` after
a reset, and every 30 s a thread analyzer table. **Write the stack use of
every thread into the guide §14** (main, BT RX, BT TX processor, sysworkq,
MPSL work, ISR, idle). Any thread above 75 % gets a bigger stack in
`prj.conf`; RAM headroom is in §14. The debug image has only ~270 B of RAM
left, so it is for measurement, not for shipping.

If `rtt setup` finds no control block: the image is not the debug one, or
the search range is wrong (RAM is 0x20000000, 24 KB = 0x6000).

---

## T1. Flashing (ST-LINK V3 + OpenOCD)

**Setup:** board powered, TC2030 in J501, ST-LINK on USB. Release image
built.

**Steps**

1. Check the probe sees the target:
   ```bash
   ~/.local/bin/openocd -f boards/timetaker/tt_ble/support/openocd.cfg -c "init; targets; shutdown"
   ```
2. **First flash of every new board** (the module may carry a Raytac test
   image, and newer nRF52 revisions ship with the debug port locked):
   ```bash
   tools/flash.sh --recover
   ```
3. Every later flash:
   ```bash
   tools/flash.sh
   # or the west way (same OpenOCD config, no APPROTECT step):
   west flash -d build --runner openocd --openocd ~/.local/bin/openocd
   ```
4. Power-cycle the board, then repeat step 1.

**Expect**

- Step 1: `nrf52.cpu` examined, Cortex-M4 found. OpenOCD must **not** print
  "A high level adapter ... cannot access the CTRL-AP" (that would mean HLA
  mode, and `nrf52_recover` would not work).
- Step 2: "device has been successfully erased and unlocked", then
  "UICR.APPROTECT written: 0x5A", `verified <n> bytes`.
- Step 4: the target still examines after a power cycle (the debug port
  stayed open: UICR.APPROTECT = 0x5A, firmware copies it to
  APPROTECT.DISABLE at boot).
- `mdw 0x10001200 2` (UICR.PSELRESET[0..1]) reads `0x00000015` twice after
  the first boot: the firmware wrote P0.21 as the reset pin and reset once
  (`gpio-as-nreset`).

**If it fails, check first**

- "AP lock engaged" / examine fails: run `tools/flash.sh --recover` (mass
  erase; it erases UICR too, which is why `flash.sh` writes APPROTECT back).
- "Error: Cannot access nRF52 CTRL-AP!": OpenOCD is in HLA mode. Fall back to
  the guide §4.3 lines (`interface/stlink-dap.cfg`, `transport select
  dapdirect_swd`) in `support/openocd.cfg`, and update the ST-LINK firmware
  (STM32CubeProgrammer) — dapdirect needs a recent ST-LINK V3 firmware.
- No target voltage: J501 pin 1 (VCC) orientation, board not powered.
- Locked again after every power cycle: UICR.APPROTECT is not 0x5A
  (`mdw 0x10001208`). Recover, then flash with `tools/flash.sh` (not
  `west flash`, which skips that step).
- SWD flaky: lower `adapter speed` (nrf52.cfg sets 1000 kHz) to 400.

---

## T2. Supply, regulator, clocks, reset pin

**Setup:** release image, scope.

**Steps**

1. Measure DVDD_3V3 at the module during advertising.
2. In OpenOCD: `mdw 0x40000578` (POWER.DCDCEN).
3. `mdw 0x40000518` (CLOCK.LFCLKSRC) and `mdw 0x4000041C` (CLOCK.LFCLKSTAT).
4. Press the reset of J501 (or pull P0.21 low briefly with a probe tip
   through 1 k): the nRF must reboot.

**Expect**

- 3.3 V ± 3 %, no dips below 3.0 V during radio events (scope, 10 µs/div,
  AC coupled: dips < 100 mV).
- DCDCEN = 0 (LDO). **If it reads 1, stop**: the board DTS is wrong, the
  module's DCC pin is not connected.
- LFCLKSRC = 0 (RC), LFCLKSTAT = 0x00010000 (running, source RC).
- T4 later shows `NRF_HELLO.resetCause = PIN (2)` after step 4.

**If it fails:** DCDCEN = 1 → `build/zephyr/zephyr.dts` `regulator-initial-mode`
must be 0, `CONFIG_SOC_DCDC_NRF52X` must be unset (`main.c` asserts both at
build time). LFCLK not running → the BLE stack never starts; check
`CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC=y` in `build/zephyr/.config`.

---

## T3. Advertising seen in the nRF Connect app

**Setup:** release image, phone with nRF Connect, nothing connected.

**Steps**

1. Scanner tab, scan, no filter.
2. Tap the "TimeTaker" entry to expand the advertising data; open the raw
   data.
3. Watch the advertising interval shown by the app (or the "RSSI graph").

**Expect**

- Device named **"TimeTaker"** (from the scan response), connectable.
- Advertising data: flags `0x06`, complete list of 128-bit service UUIDs
  `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` (NUS). Scan response: complete
  local name "TimeTaker". No other fields.
- Interval about 100 ms (the app shows ~100–110 ms with the random delay).
- The LED gives a short blink every 3 s (§7.2 row 7).
- RSSI at 1 m: about −40 to −60 dBm. Much worse (< −75 dBm at 1 m) points
  at the antenna keep-out or a matching problem.

**If it fails:** no device at all → T1 step 1 works? Is the image running
(`reset run`, RTT log in the debug image)? Name missing but device present
(shows "N/A") → the phone did not do an active scan yet; scan again. Very
weak signal → check the module's antenna area on the PCB for copper or
ground pour beneath it.

---

## T4. Connect, BLE_IRQ on the scope, link parameters

**Setup:** scope CH1 on BLE_IRQ (P0.05 / STM PB5), CH2 on the nRF TX net
(UART1_RX, P0.18). STM held in reset or not flashed (it does not matter
here). Phone with nRF Connect.

**Steps**

1. Connect from the app. Note the time from tap to BLE_IRQ high.
2. In the app: "Request MTU" 247. "Set preferred PHY" 2M. Read the
   connection parameters (Android: the app's log tab).
3. Enable notifications on the TX characteristic (6E400003...).
4. Disconnect from the app.
5. Connect again; this time switch the phone's Bluetooth off.

**Expect**

- Step 1: BLE_IRQ goes high within ~100 ms of the connection. CH2 shows one
  burst at 460800 baud right after it: the `LINK_UP` frame (16 bytes on the
  wire). LED: double blink every 2 s (connected, session NONE).
- About 5 s after connecting the nRF asks for 7.5–30 ms, latency 0,
  timeout 4 s; the central may pick something else (Android often 30–45 ms).
- Step 2: MTU 247 accepted; PHY 2M (if the phone supports it); each change
  makes another `LINK_UP` burst on CH2. DLE: the nRF starts the data length
  update itself; nothing to do in the app.
- Step 4: BLE_IRQ low at once, a `LINK_DOWN` burst on CH2, advertising
  resumes (the app sees "TimeTaker" again within a second), LED back to the
  3 s blink.
- Step 5: BLE_IRQ goes low after the supervision timeout (the central's,
  up to the 4 s the nRF asked for).

**If it fails:** BLE_IRQ never moves → check P0.05 is configured (RTT log in
debug build: `stm_ctrl` init error) and that PB5 is not driven by the STM
(it must be an input). Not advertising again after a disconnect → the
`recycled` callback path (`serviceAdvertising`); RTT shows "advertising
start failed: <err>".

Decode the `LINK_UP` burst with T5's `monitor` instead of by hand once the
adapter is wired.

---

## T5. The UART link with `link_tool.py` (STM held in reset)

This tests the whole nRF side of the link without the control firmware.
`tools/link_tool.py` plays the STM.

**Setup**

- Hold the STM in reset: keep SW605 (NRST) pressed with a clip, or tie STM
  NRST to GND through a jumper. Its PB6/PB7 are then high-impedance.
- USB-UART adapter (3.3 V!): adapter **TX → UART1_TX net** (STM PB6 / nRF
  P0.16), adapter **RX → UART1_RX net** (STM PB7 / nRF P0.18), GND → GND.
  Use the STM-side test points or the pins of U501 / the STM.
- Release image. Phone with nRF Connect.

**Steps**

1. `tools/link_tool.py monitor --port /dev/ttyUSB0`, then reset the nRF
   (OpenOCD `reset run`, or T2 step 4).
2. Keep `monitor` running; connect with the phone, enable notifications,
   disconnect.
3. `tools/link_tool.py send --port /dev/ttyUSB0 stm-hello 0 1`.
4. Connect with the phone and enable notifications; then
   `tools/link_tool.py send --port /dev/ttyUSB0 data 48656c6c6f` (needs a
   credit: the `echo` mode below tracks them; with `send`, send at most 4
   DATA frames after an `NRF_HELLO`).
5. Run `tools/link_tool.py echo --port /dev/ttyUSB0`. From the phone, write
   "hello" (UTF-8) to the RX characteristic (6E400002...).
6. With `echo` running, from the phone write 20 bytes of hex 00..13.
7. Baud check: scope on P0.18 during a `LINK_UP` burst, measure one bit.

**Expect**

- Step 1: `NRF_HELLO linkMajor=0 linkMinor=1 fwMajor=0 fwMinor=1 fwPatch=0
  credits=4 resetCause=PIN ... fwGitRev=<the build's rev> btAddr=<the
  address the phone app shows>`.
- Step 2: `LINK_UP attMtu=23 ...` (or 247 after the phone's MTU exchange),
  another `LINK_UP` per parameter change, once a second `STATS rssi=<dBm>
  dataDropped=0 linkCrcErrors=0 uartErrors=0`, and `LINK_DOWN hciReason=19`
  (0x13) or 8 (timeout) at the end.
- Step 3: a new `NRF_HELLO` (credits = 4), and `LINK_UP` if connected.
- Step 4: the phone shows a notification "Hello" on the TX characteristic.
  `monitor` (in another terminal on the same adapter, or by restarting it)
  sees `CREDIT count=1` after the notification was sent.
- Step 5: `echo` prints `NRF_HELLO ... -> credits = 4`; the phone receives
  "hello" back as a notification. Every 5 s: `echoed <n> frames, credits 4,
  queued 0, bad frames 0`.
- Step 6: the 20 bytes (including 0x00) come back identical: COBS inside the
  link handles zeros.
- Step 7: bit time 2.17 µs nominal; the nRF's 460800 setting is about 0.8 %
  slow (≈ 2.19 µs). Write the measured value into §14 (guide §13 Q5).

**If it fails, check first**

- Nothing at all on `monitor`: RX/TX swapped (the most likely mistake; the
  net names are the STM's), adapter at 3.3 V, STM really in reset (if PB7
  drives, it fights the nRF TX), port at 460800 (`link_tool` sets it).
- Garbage / `bad frames` climbing: baud mismatch; some cheap adapters cannot
  do 460800 (CH340 can; old PL2303 cannot).
- `STATS uartErrors` climbing: noise or a floating RX line — the pull-up on
  P0.16 is in the board pinctrl; check `bias-pull-up` in
  `build/zephyr/zephyr.dts`.
- DATA written by the phone never reaches the adapter: `STATS dataDropped`
  rising means the UART TX ring was full (should not happen at phone rates).
- Notifications never arrive: notifications not enabled in the app (DATA is
  then dropped and credited, `dataDropped` counts it).

---

## T6. LED patterns (D603)

**Setup:** release image. With the STM held in reset, drive `UI_STATE` and
`CUE` with `link_tool.py send` (T5 wiring). Or, once control C5 runs, let the
STM drive them. Phone for connecting.

**Steps and expect** (guide §7.2; one row at a time)

| # | Do | Expect |
|---|---|---|
| 1 | Nothing connected | short blink (~100 ms) every 3 s |
| 2 | Connect from the phone | double blink every 2 s |
| 3 | `send ui-state 1 0` | still double blink (UNBOUND) |
| 4 | `send ui-state 2 0` | steady on (BOUND) |
| 5 | `send ui-state 2 1` | fast blink, 5 Hz (pair window beats BOUND) |
| 6 | `send cue 1` | 3 quick flashes, then back to row 5's pattern |
| 7 | `send cue 2` | one 1 s flash (with short dark gaps around it), then back |
| 8 | `send advertise 0` | phone disconnected (kicked), LED **off** |
| 9 | hold the button 3 s while row 8 | fast blink while held (row 2 beats row 3) |
| 10 | `send advertise 1` | advertising again, back to the 3 s blink |
| 11 | connect, `send ui-state 2 0`, disconnect | LED back to the 3 s blink; connect again: double blink (session reset to NONE locally on LINK_DOWN) |
| 12 | `send ui-state 3 0` (invalid) | no change (ignored; the nRF counts it) |

**If it fails:** LED never lights → D603 polarity / R603 (P0.20 is active
high). Patterns right but the STM's messages have no effect → T5 first
(the frames are not arriving). A pattern "stutters" → the main loop is late;
check in the debug image's RTT log for warnings.

---

## T7. Button classes (SW501)

**Setup:** T5 `monitor` running (STM in reset) or the control firmware's
event log. A stopwatch (phone).

**Steps and expect** (guide §7.1)

| # | Hold for | During the hold (LED) | On release (`monitor`) |
|---|---|---|---|
| 1 | tap (< 1 s) | — | `BUTTON press=SHORT`, LED one blink |
| 2 | 1.5 s | — | nothing (dead zone) |
| 3 | 3 s | fast blink from 2 s | `BUTTON press=LONG` |
| 4 | 12 s | fast blink from 2 s, steady from 10 s | `BUTTON press=VERY_LONG` |
| 5 | 35 s | fast, steady from 10 s, normal again from 30 s | nothing (stuck) |
| 6 | 10 quick taps | — | 10 × `SHORT` |

**If it fails:** no events at all → P0.00 reads low permanently (R501
missing, SW501 shorted): in OpenOCD `mdw 0x50000510` (GPIO IN), bit 0 must
be 1 when released. Double events per tap → bounce longer than 30 ms
(debounce is 3 × 10 ms); note it in §14.

---

## T8. Echo through the STM

**Setup:** control firmware with `CTRL_ENABLE_NRF_LINK_ECHO` (control task
C5, first step) flashed, STM running (not in reset), USB-UART adapter
disconnected. Release nRF image. Phone with nRF Connect.

**Steps**

1. Reset both chips (power cycle the box).
2. Connect from the phone, request MTU 247, enable notifications.
3. Write "ping" to the RX characteristic. Then 20 bytes 00..13. Then a
   244-byte write if the app allows (nRF Connect: "Write" with a long hex
   string; Android only after the MTU exchange).
4. Reset only the STM (SW605). Write "ping" again after it is back.
5. Reset only the nRF (T2 step 4). Reconnect, write "ping".

**Expect**

- Every write comes back as notification(s), byte-identical. A 244-byte
  write comes back as one notification at MTU 247, or split into 20-byte
  notifications at MTU 23 (the nRF splits, guide §5.2).
- Step 4: the STM's `STM_HELLO` makes the nRF answer `NRF_HELLO` and
  `LINK_UP`; echo works again without reconnecting.
- Step 5: BLE_IRQ low during the nRF reset; the STM sees `NRF_HELLO` with
  `resetCause = PIN` and resets its credits; echo works after reconnecting.
- The control firmware's NODE_LIST shows the BLE node with the nRF's link
  and firmware versions.

**If it fails:** T5 passed but T8 does not → the STM side (PB6/PB7 swap,
USART1 baud, `nrf_link` credits). A one-direction failure: phone → STM ok
but nothing back → the STM sends DATA without credits (the nRF drops it:
`STATS dataDropped` and `creditViolations` in the debug log).

---

## T9. Throughput

**Setup:** T8 (echo through the STM) or T5's `echo` mode. A laptop with the
web app's diagnostics, or nRF Connect's "Write Without Response" with a
repeat count.

**Steps**

1. Echo a 10 KB burst (guide card B2): 42 writes of 244 B (MTU 247) or 512
   writes of 20 B (MTU 23).
2. Raw-sample stream (once the control firmware has it): STM → BLE only,
   measure bytes/s at the app for 60 s.
3. Repeat with the phone forcing 1M PHY, and with a laptop (Windows Chrome)
   where the connection interval is 30–50 ms.

**Expect**

- 10 KB passes **without loss**: every byte back, `STATS dataDropped = 0`,
  `linkCrcErrors = 0`, `uartErrors = 0`.
- STM → BLE throughput is bounded by the in-flight limit (one notification
  per ATT buffer: `BT_ATT_TX_COUNT - 1` = 1 in flight) and the connection
  interval: about one 244 B notification per connection event, i.e.
  ~8 KB/s at 30 ms, ~32 KB/s at 7.5 ms. The diagnostics stream needs "a few
  KB/s" (guide §6.3), so 30 ms is enough. **Write the measured numbers into
  §14.** If it is too slow, raising `CONFIG_BT_ATT_TX_COUNT` to 3 (and the
  ACL TX count) is the knob; RAM headroom is in §14.

**If it fails:** losses with `dataDropped` rising → the STM ignored credits
or the UART TX ring overflowed (phone → STM direction); `linkCrcErrors` →
signal integrity at 460800 (T5 step 7).

---

## T10. Watchdog

**Setup:** WDT self-test image (`build-wdt`, `CONFIG_TT_WDT_SELFTEST=y`),
T5 `monitor` (STM in reset), phone.

**Steps**

1. Flash the self-test image, reset. Note `NRF_HELLO resetCause`.
2. Hold the button 12 s and release (a VERY_LONG press hangs the main loop
   on purpose in this image).
3. Wait 5 s.
4. Halt the CPU from OpenOCD (`halt`), wait 10 s, `resume`.
5. Flash the release image again.

**Expect**

- Step 2: `BUTTON press=VERY_LONG`, then advertising and the LED stop.
- Step 3: within 4 s the nRF resets by itself: a new `NRF_HELLO` with
  **`resetCause=WATCHDOG`**, advertising again.
- Step 4: no reset while halted (the WDT pauses in debug halt), the board
  continues after `resume`.
- After any later reset by pin: `resetCause=PIN` (RESETREAS is cleared at
  every boot, so causes do not accumulate).

**If it fails:** no reset in step 3 → the WDT never started (`wdt_install_timeout`
error in the debug image's log). A reset during step 4 → `WDT_OPT_PAUSE_HALTED_BY_DBG`
not applied.

---

## T11. Binding with the web app

**Setup:** control firmware with the BLE session and bind policy (control
C5 complete), web app on a Chrome laptop, release nRF image. Bind list
cleared (button hold 12 s, or the app over USB).

**Steps** (SYSTEM_ARCHITECTURE §9.5)

1. In the web app, connect over BLE ("TimeTaker" in the chooser).
2. The app shows "Hold the BLE button for 2 s". Hold SW501 3 s, release.
3. Disconnect, reconnect.
4. Leave an unbound browser connected without pressing anything for 70 s.
5. Hold the button 12 s, release.
6. Over USB, open the app on a second browser; then connect that browser
   over BLE.

**Expect**

- Step 1: LED double blink (connected, UNBOUND); the app is told
  `ERR_NOT_BOUND`.
- Step 2: fast blink while held from 2 s; on release the STM opens the
  window (LED fast blink), accepts the browser live: 3 quick flashes, then
  steady on (BOUND).
- Step 3: steady on at once, no button needed.
- Step 4: the STM kicks it after 60 s (`KICK`); the app shows the
  disconnect; the LED returns to the 3 s blink.
- Step 5: steady on from 10 s while held; on release one 1 s flash
  (`BINDS_CLEARED`); a connected session drops to UNBOUND (double blink).
- Step 6: that browser is bound by the USB HELLO and connects over BLE as
  BOUND without a button press.

**If it fails:** LED right but the app never binds → STM policy (control
guide §5.4). The app never sees the device → T3. The app connects but every
request times out → T8 (the bridge) first.

---

## T12. STM reset lines (B5 groundwork)

Nothing in v1 triggers these (task B5). This test only checks the idle
states, which matter from day one.

**Setup:** scope on STM NRST, BOOT_REQ (P0.04 / PB4), BOOT0 (P0.12).

**Steps**

1. Power-cycle the box and watch NRST and BOOT_REQ during the nRF's boot.
2. Hold the nRF in reset (J501 reset or OpenOCD `reset halt`), then press
   SW605 (STM NRST).
3. OpenOCD: `mdw 0x50000738` (PIN_CNF[14], NRST).

**Expect**

- Step 1: NRST never glitches low because of the nRF; BOOT_REQ stays high
  (PB4's pull-up while the nRF is in reset, then driven high).
- Step 2: the STM resets normally (the nRF's P0.14 is open drain and
  released, it never holds NRST).
- Step 3: DIR = output, DRIVE = S0D1 (`0x00000603`: bits 0 = 1 output, 1 =
  1 input disconnected, 8–10 = 6 = S0D1).
- BOOT0 (P0.12) undriven: the STM's R201 holds it low.

**If it fails:** NRST held low or pulsed by the nRF → stop, check
`nrst-gpios` flags in the board DTS (`GPIO_OPEN_DRAIN | GPIO_ACTIVE_LOW`,
`stm_ctrl.c` asserts it at build time).
