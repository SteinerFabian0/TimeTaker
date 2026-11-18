# Control module — bench tests

Written 2026-11-18, before any board existed. Run them in order when the
boards arrive; each block needs the ones above it. Build, flash and preset
commands: `CONTROL_FIRMWARE_NOTES.md` ("Build") and
`LapCounterControlModule_cmake/docs/RELEASING.md`.

**Tools.** ST-LINK V3 on J201, scope (2+ channels), USB-C cable, a lane module
(then six) with the ribbon, the nRF52805 flashed with the BLE firmware, the
nRF Connect phone app. The **tt bench client** is the web-app agent's Node CLI
in `WebApp/tools/`: it speaks the host protocol over `/dev/ttyACM*`, sends
requests and prints events and DBG_LOG lines. Below it is named by intent
("tt bench client: send LANE_SCAN, watch events"). The web app's debug
console (`npm run debug`) does the same in a browser.

**Debug knobs** (`app/config/ctrl_behavior.h`, debug block; rebuild after a
change): `CTRL_ENABLE_DBG_LOG` (on), `CTRL_ENABLE_NRF_LINK_ECHO`,
`CTRL_ENABLE_FAULT_INJECT`, `CTRL_ENABLE_LANE_AUTO_POWER`,
`CTRL_DISABLE_WATCHDOG`, `CTRL_DISABLE_IDLE_WFI`. A release build warns about
any of them left on.

**First thing on any failure:** STATUS (`lastError`, `errorCount`,
`loopMaxMs`), then the DBG_LOG lines. With a probe: `ctrlAssertLastStatus`,
`faultGuardBreadcrumb`, `i3cCtrlStats`, `nrfLinkStats`, the sessions' `stats`.

---

## C0 — Board alive

**C0-1 Flash and boot.** Ribbon unplugged, `debug_GCC_main`.
1. Flash (`-e all` first on a new chip), `-rst`.
2. *Expect:* both panel LEDs flash 4× (boot flash), then D601 blips every 2 s
   (no USB host yet).
3. *If not:* SWD connect at all? `mode=UR` under reset. VDD 3.3 V, HSE running
   (scope PH0: 24 MHz). If the probe loses the core, set `CTRL_DISABLE_IDLE_WFI`.

**C0-2 USB port.** `ls -l /dev/ttyACM*`, `udevadm info /dev/ttyACM0`.
*Expect:* model `TimeTaker`, 1209:0001, serial = 24 hex digits of the UID; D601
blinks 1 Hz. *If not:* `dmesg`; USB_FAULT (PB12) low means the TPD4S201 cut
VBUS.

**C0-3 HELLO.** tt bench client: HELLO, NODE_LIST, STATUS.
*Expect:* HELLO OK, transport USB, bindState TRUSTED, a bootId, logNextSeq ≥ 1;
EVT_BOOT is evSeq 0 (RESUME from 0 shows it) with resetCause POWER_ON;
NODE_LIST: CTRL (boardType CONTROL, imageCrc 0: no image header in v1) and BLE
(ABSENT without the nRF); STATUS lastError 0 — or ERR_OPTION_BYTES (-71) on a
chip optprog.py has not seen (F-4). D601 steady while connected.

**C0-4 Buttons.** Press BTN1/2/3 short, 1 s, 5 s.
*Expect:* EVT_BUTTON per press: SHORT / LONG / VERY_LONG. *If not:* PC13/PC15/PE2
read with the debugger; `buttonHandlerIsPressed()`.

**C0-5 SYNC on the scope.** CH1 PB15 (J301-7), CH2 LANE_3V3 (J301-1),
trigger CH2 rising. tt bench client: LANE_POWER{1}, later {0}.
*Expect:* rail ramps in ~16 ms; SYNC starts ~30 ms after the switch: 1.000 kHz,
50 %, within ±20 ppm. CHAIN_OUT (J301-2) goes high ~20 ms after SYNC (RSTDAA,
then the chain-low wait). On power off SYNC and CHAIN_OUT stop before the rail
falls. SCL/SDA idle high only while the rail is up.
*If not:* `sysTimeIsSyncOutputOn()`, `laneManagerPower`; DBG_LOG "LANE 0 BUS
START FAILED" means `mx_i3c1_init()` refused.

## C1 — Lane bus

**L-1 One lane enumerates.** One lane on the ribbon (lane firmware with
hot-join), LANE_POWER{1} (or `CTRL_ENABLE_LANE_AUTO_POWER`). Scope SCL/SDA,
trigger on SDA falling after CHAIN_OUT rises.
*Expect events:* EVT_LANE_STATE{1, JOINED}, then {1, CONFIGURED}; DBG_LOG
"LANE 0 CHAIN UP", "LANE 1 JOINED addr=17", "LANE 1 CONFIGURED seq=…",
"LANE 1 ENUM DONE" ~200 ms later. NODE_LIST lists LANE 1 with the lane's
versions and imageCrc, state OK. STATUS lanesPresent 1.
*On the scope:* the hot-join (address 0x02) right after CHAIN_OUT, ENTDAA,
then private transfers at 1 MHz.
*If not, check in this order:*
- no JOINED, `i3cCtrlStats.hotJoins` = 0: the lane never asked (CHAIN_IN?
  lane firmware?), or HJACK is off (`mx_i3c1_init` enables it);
- hotJoins > 0, DBG "DAA NOBODY": ENTDAA got no answer — the C5 HAL DAA flow
  (guide §10 Q2) and the lane's ENTDAA payload config;
- "DAA MULTI": a lane without CHAIN_IN answered ENTDAA (lane side);
- JOINED but no CONFIGURED: DBG "REPUSH"/"PUSH FAILED flags=…": the lane does
  not set CONFIG_VALID+SYNC_ALIGNED; check SYNC_ALIGN reaches it within 500 µs
  (scope: SYNC edge to the end of the SYNC_ALIGN write);
- every transfer fails: `i3cCtrlStats.lastSer` (ANACK = wrong address, DNACK
  = lane rejects); try the HAL note in `i3c_ctrl.h` (hand-built control words).

**L-2 Six lanes.** All six on the ribbon. Measure the rail current
(guide §10 Q6) with all six sampling (LANE_SAMPLING{0x3f, ON}).
*Expect:* JOINED/CONFIGURED for 1..6 strictly in order, within ~1.5 s; NODE_LIST
lists six lanes; `loopMaxMs` ≤ 1. Scope SCL: polling bursts every 10 ms.
*If not:* a lane stuck between n and n+1: CHAIN_OUT of lane n. Bus errors with
six lanes only: SDA rise time (2.2 k, ribbon length) — keep 1 MHz.

**L-3 Unplug lane 3.** Running six lanes, pull lane 3's ribbon in, wait, plug back.
*Expect:* within ~30 ms (3 failed polls) EVT LEFT for 3..6 (status
ERR_LANE_ABSENT / ERR_TIMEOUT); after re-plugging JOINED/CONFIGURED for 3, 4,
5, 6 in order. Lanes 1–2 never leave. DBG "LANE 4 LEFT" etc.
*If not:* lanes 4–6 rejoining with stale addresses: check "RELEASE_CHAIN" is
honoured by the lane firmware.

**L-4 LANE_SCAN.** *Expect:* every lane LEFT, then the chain re-enumerates
from 1. Also LANE_STATUS{3}: a full status (deferred answer), and {7} →
ERR_INVALID_ARG, an absent lane → ERR_LANE_ABSENT.

**L-5 Calibrate, power-cycle, reference applied.** Two lanes, both free,
sampling OFF (the default). LANE_CALIBRATE{0x03, LEARN}.
*Expect:* response OK; within ~200 ms each lane samples (LED/scope) and
EVT_LANE_CALIBRATED{1, SAVED, reference≈the bare-foil level, noise} and {2,
…} arrive (up to 10 s later if a settings write happened just before);
sampling is OFF again afterwards; STATUS settingsWrites +1 (one write for
both). LANE_STATUS{1}: `reference` equals the event's value. Now park a
car (or a finger) on lane 1's sensor and power-cycle the board (or LANE_POWER
0/1). *Expect:* after CONFIGURED, LANE_STATUS{1} `reference` = the saved
value and `baseline` near it (not the covered level); on the scope/I3C
trace, `COMMAND 08` (SET_REFERENCE) right after the CONFIG write. Then
LANE_CALIBRATE{0x01, FORGET}: EVT {1, FORGOTTEN}; after the next power cycle
no SET_REFERENCE is sent. Also: LANE_CALIBRATE during a start sequence →
ERR_SEQ_RUNNING; {0x01, LEARN} twice quickly → second ERR_BUSY; a mask of
absent lanes → ERR_LANE_ABSENT; a lane held covered so learning never
settles, or unplugged while learning → FAILED.
*If not:* DBG "LANE n CALIBRATED result=3" = timeout: STATUS flags never
showed CALIBRATED clear then set (lane firmware < lane_protocol 0.4?).

**L-6 Fault cleared.** Make a lane report a fault (e.g. a sensor error that
sets lastError), then clear it (COMMAND CLEAR_COUNTERS or fix the cause).
*Expect:* EVT_LANE_STATE{n, FAULT, status} then, within 1 s of the clearing,
{n, CONFIGURED, 0}.

## C2 — Event pipeline

**E-1 Crossings.** Lanes sampling ON (LANE_SAMPLING{0x3f,1}). Hand through
each beam.
*Expect:* EVT_CROSSING{lane, type CROSSING, tEntryUs ≈ STATUS uptime·1000,
laneSeq +1 per pass, dwell plausible}; flags without NO_LOCK/UNALIGNED once
the lane is CONFIGURED. Hold a hand 1 s: BLOCKED, then CLEARED.
*If not:* events on the lane but none here: `LANE_STATUS` fifoCount > 0 →
EVENT_FIFO parsing (DBG "FIFO BAD"); "SEQ GAP" lines = lost on the lane.

**E-2 RESUME by unplugging USB** (the required test). Burst of crossings
(a helper waving at two lanes), pull USB for 30 s, plug back, tt bench client:
HELLO, RESUME{bootId, last evSeq + 1}.
*Expect:* every crossing from the dropout arrives, in order, exactly once;
evSeq contiguous. *If not:* ERR_LOG_GAP = more than 1600 events (log too
small for that test); ERR_BOOT_MISMATCH = the box rebooted (check EVT_BOOT).

**E-3 Raw stream.** LANE_RAW_STREAM{3, 0, 4}.
*Expect:* STR_RAW_SAMPLES for lane 3, chunkSeq +1 per chunk, periodUs = 4 ×
the sample period; LANE_RAW_STREAM{0} stops it and the lane goes back to its
mode. Over BLE (B-3) the stream is capped (a few KB/s).

## C3 — Start sequence

**S-1 Two lane LEDs on the scope.** Two lanes CONFIGURED, scope on both LED
pins. START_SEQUENCE{lights 6, arm 1, step 1000, hold 200..3000, lead 0}, ten times.
*Expect:* response t0Us ≈ now + 150 ms; EVT_SEQ_STARTED at once (no Tout);
LEDs on 1 s apart (lit = lanes present), both go dark within a few µs of each
other; EVT_LIGHTS_OUT{toutUs} after; holds differ every time; DBG "SEQ LIGHTS
OUT v=<hold µs>".
*If not:* EVT_SEQ_ABORTED{LANE_FAILED}: the lane did not show the actions in
its STATUS.schedulePending (lane SCHEDULE queue) within 10 read-backs 2 ms
apart, or not by T0 − 20 ms.
*armSampling:* with the lanes' sampling OFF before the start, LANE_STATUS
after T0 shows samplingMode ON; unplug and replug a lane after lights out:
it re-joins and is still sampling (the table mode followed T0).

**S-2 Abort.** Mid-sequence: BTN2 held 1 s. Again with ABORT_SEQUENCE.
*Expect:* EVT_BUTTON{BTN2, LONG} and EVT_SEQ_ABORTED{BUTTON} (then {HOST});
lights blink 3×; no LIGHTS_OUT. ABORT_SEQUENCE when idle → ERR_INVALID_STATE.
LANE_LED during a sequence → ERR_SEQ_RUNNING. Unplug a lane mid-sequence →
{LANE_LEFT}; LANE_POWER{0} → {POWER}.

## C4 — Settings and RNG

**P-1 Settings across power cycles.** SETTINGS_SET{name "Bench 1", ble 1};
LANE_CONFIG_SET{0x3f, save 1, mergeGapUs 25000}; wait 12 s; unplug USB; plug.
*Expect:* SETTINGS_GET and LANE_CONFIG_GET return the new values; STATUS
settingsWrites counted 1–2 before the power cycle, 0 after.
*If not:* `settingsStoreIsDirty()`; flash errors as lastError ERR_FLASH.

**P-2 Corrupt page.** With the probe write garbage into 0x0803_C000 and
0x0803_E000 (`$P -c port=SWD -w32 0x0803C000 0x12345678`), reset.
*Expect:* defaults load, STATUS lastError ERR_SETTINGS_CORRUPT (-41). Erase
both pages: defaults, lastError 0 (a new board is not "corrupt").
**P-3 Erase vs. SYNC/USB.** Scope SYNC while settings are written (P-1):
no missing edge, USB stays up (the pages are in bank 2, RWW).

**R-1 bootId.** Ten resets: ten different bootIds in HELLO.

## C5 — BLE

**B-1 nRF link up.** nRF fitted and flashed.
*Expect:* NODE_LIST BLE node state OK with the nRF's versions; STATUS
bleLink DOWN (no central). Scope PB6/PB7: STM_HELLO at boot, 460800 baud.
*If not:* `nrfLinkStats` badFrames (baud error, BLE guide §5.1), BLE node
FAULT = link MAJOR mismatch (EVT_BLE_LINK{VERSION_MISMATCH}).

**B-1b Bridge liveness.** nRF running: STATUS bleBridge OK (1), bleRssi 127
(no central), nrfLinkCrcErrors stays 0; with a central connected bleRssi
shows its dBm. Now hold the nRF in reset (probe: `-rst` held, or pull its
nRESET low) or unplug its UART.
*Expect:* within 3 s STATUS bleBridge ABSENT (0), bleRssi 127, NODE_LIST
BLE node ABSENT. Release: NRF_HELLO, bleBridge OK again within ~1 s. An nRF
image with another link MAJOR: bleBridge VERSION_MISMATCH (2), node FAULT.
*If not:* the nRF image does not send STATS every second (BLE firmware
older than the heartbeat).

**B-2 Echo test.** `CTRL_ENABLE_NRF_LINK_ECHO = 1`. nRF Connect app: connect
to "TimeTaker", enable notifications, write bytes to the RX characteristic.
*Expect:* the same bytes come back as notifications, also with 244-byte
writes and 20 writes in a row (credits: `nrfLinkStats.dataFramesOut`, no
`overCredit`). Set the knob back to 0.

**B-3 Web app over BLE, binding.** Bind list empty (BIND_REMOVE{0…0} over USB).
1. Connect the web app over BLE: HELLO OK with bindState UNBOUND; any other
   request ERR_NOT_BOUND; nRF LED double blink; no events arrive.
2. Hold SW501 2 s: EVT_BUTTON{BLE_BIND, LONG}, EVT_BIND_CHANGED{WINDOW_OPENED},
   then {ADDED}; the session works now (events arrive), LED 3 flashes then steady.
3. Reconnect later: BOUND at once.
4. Wait 60 s unbound (another browser): KICK, EVT_BIND_CHANGED{REJECTED}.
5. SW501 10 s: {CLEARED}, LED one long flash, the open session UNBOUND.
6. Plug USB with the web app: its clientId appears in BIND_LIST (it binds
   for BLE later). BIND_ALLOW{30} over USB opens the window without a button.
*If not:* `nrfLinkStats`, the BLE session `stats.notBound`.

**B-4 Pull USB mid-race, finish on BLE** (SYSTEM_ARCHITECTURE §14 step 6).
*Expect:* identical standings after RESUME over BLE; EVT_BLE_LINK UP with
the link parameters. BLE_IRQ falling (central disconnects) resets the session.

## C6 — Robustness

**F-1 HardFault.** `CTRL_ENABLE_FAULT_INJECT = 1`, hold BTN1 5 s.
*Expect:* reboot; EVT_BOOT resetCause FAULT (6); `faultGuardBreadcrumb`
lastFaultId 1 with a PC in `faultGuardInjectHardFault`.
**F-2 Watchdog.** Same knob, hold BTN3 5 s.
*Expect:* the loop hangs; ~1.4–2.6 s later reboot, resetCause WATCHDOG (4).
With a probe halted at a breakpoint the IWDG must NOT bite (frozen).
**F-3 Boot-loop guard.** Repeat F-2 six times without 60 s of uptime between.
*Expect:* the 6th boot runs without the watchdog (F-2 now hangs
for good), lastError ERR_INVALID_STATE. Power-cycle clears it.
**F-4 Option bytes.** `python3 tools/optprog.py --dry-run`, then without,
then power-cycle. *Expect:* BOOT_SEL = 1, IWDG_STDBY = 0; STATUS lastError no
longer -71. Then hold SW604 BOOT, tap SW605 NRST: the ROM DFU device appears
(`lsusb`: 0483:df11) — this proves BOOT_SEL = 1 means "the pin decides".
If it does NOT appear, the BOOT_SEL meaning is inverted (see optprog.py
docstring): set BOOT_SEL = 0, BOOT0 = 0 and re-test.
**F-5 Fault inputs.** Short PB12 (USB_FAULT) to GND briefly through 1 k.
*Expect:* EVT_FAULT{USB, -61}, STATUS usbFaults +1, at most one event per second.

## C7 — Bootloader (removed)

No field update in v1 (2026-11-18): every board is flashed over SWD /
Tag-Connect, so there is nothing to bench here. The bootloader tests U-1..U-5
are at git tag `archive/bootloader-update`. What remains to check (with C0-3):
`REBOOT{BOOTLOADER}` and a request with id 0xA0 both answer ERR_UNSUPPORTED
(`tt suite usb` does it), and the box does not reset.
