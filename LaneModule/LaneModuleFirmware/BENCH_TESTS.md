# Lane module — bench tests

Prepared 2026-11-18, before any board existed. Run them in order when the
boards arrive: each one relies on what the earlier ones showed. Firmware
status and the reasons behind each mechanism: `LANE_FIRMWARE_GUIDE.md` §11.

**Tools:** ST-LINK V3 + TC2030-NL (SWD + SWO), VS Code with the
`LapCounterLaneModule_cmake/.vscode` setups ("Debug" flashes; "Attach" shows
SWO on ITM port 0 at 2 MHz), a 2-channel scope, a 3.3 V bench supply
(current limit 100 mA per lane), a strip of retro foil, a toy car or a card.
From test 8 on: a bench I3C controller (NUCLEO-C562RE/C542RC) or the control
module.

**Debug-build hooks used below** (all off in release, `lane_behavior.h`):

| Hook | What |
|---|---|
| SWO trace | `laneTraceLevel` in Live Watch: 1 = boot, states, events, errors, 1 Hz `HB` line (default); 2 = + signal at 10 Hz (`SIG raw base low high noise st`); 3 = + every sample (`s <tLocal> <raw>`) |
| Scope marker | TP6 (PB4, SPI CS pad) low from sample trigger to data read |
| `laneBenchRequest` | Live Watch, acted on once: 1 HardFault, 2 hang (IWDG), 3 fatal, 4 single sample, 5 sensor recovery, 6 hot-join now, 7 sampling ON, 8 sampling OFF |
| `LANE_BENCH_STANDALONE` | Rebuild with 1 (debug block): samples and detects at boot with the default CONFIG on local time, no controller; LED flashes per crossing, stays lit while BLOCKED |
| `laneAssertTrapArmed` | Live Watch 1 = break at the source line of an error |
| `faultGuardBreadcrumb` | Live Watch: last fault id, PC, LR, CFSR |

The `HB` line reads: state, I3C address, lane index, CHAIN in/out,
SYNC locked/aligned, SYNC edges, samples, overruns, sample errors,
I3C writes/reads/errors, sensor state (0 reset, 1 busy, 2 shutdown,
3 active, 4 fault).

---

## 1. Power, flash, LED

- **Setup:** one lane, 3.3 V on J3 pin 1 / GND, Tag-Connect, nothing else.
- **Do:** check supply current before flashing (blank chip: a few mA).
  Flash the debug build with "Debug", run.
- **Expect:** LED blinks 4× (160 ms) right after boot. Supply current
  < 15 mA after the blink. Halting in the debugger and resuming works
  repeatedly (the IWDG is frozen while halted).
- **If not:** no SWD: check NRST and the ST-LINK AP1 setting in
  `launch.json`. No blink: scope PB8; check `indicatorLedInit()` returned 0
  (`laneAssertCount` 0). High current: board fault, stop.

## 2. SWO trace and boot info

- **Setup:** as 1, "Attach" configuration (SWO on).
- **Do:** press SW1, then power-cycle, then set `laneBenchRequest = 1`, then
  `= 3`.
- **Expect:** `BOOT TimeTaker lane 0.1.0+<rev>`, then
  `reset cause=2` (pin) after SW1, `=1` (power-on) after the power cycle,
  `=6 fault=1 pc=0x...` after the HardFault, `=6 fault=3 fatal=-3` after the
  fatal. `ID fw=0.1.0 rev=... crc=...`: compare crc with
  `arm-none-eabi-objcopy -O binary --gap-fill 0xFF LapCounterLaneModule.elf l.bin`
  and `python3 -c "import zlib;print(hex(zlib.crc32(open('l.bin','rb').read())))"`.
  `HB UNENUMERATED ...` once a second.
- **If not:** no SWO at all: PB3 must stay AF0 (`mx_cortex_debug_init`);
  SWO clock = 144 MHz core / 2 MHz. CRC mismatch: the .elf flashed vs the
  .bin checked are different builds, or the gap fill differs.

## 3. VCNL4040 on I2C1

- **Setup:** as 2, foil 15 cm under the sensor.
- **Do:** `laneBenchRequest = 4` (single sample), then
  `= 7` (sampling ON), `laneTraceLevel = 2`; then `= 8`.
- **Expect:** sensor state goes 0 → 1 → 2 in `HB` within a few ms of boot
  (ID 0x0186 read, then shut down). Single sample: SIGNAL raw in Live Watch
  (`senseSignal.raw`) non-zero. Sampling ON: state 3, `SIG raw=...` at
  10 Hz, samples counting ~1000/s, `ovr=0 err=0`. Scope on PH0/PH1: 400 kHz,
  clean edges. Sampling OFF: state 2, no more I2C traffic.
- **If not:** state stuck at 4 (fault): `vcnlLastId` in Live Watch — 0 = no
  ACK (address 0x60? pull-ups?), other = wrong ID. Wrong-looking values:
  check the PS_CONF bit positions marked BENCH in `vcnl4040.c` against the
  datasheet (PS_HD, PS_AF, PS_TRIG, LED_I). `laneBenchRequest = 5` runs a
  bus clear + re-probe; `vcnlRecoveries` counts.

## 4. Sample timing and the bench constants

- **Setup:** as 3, scope ch1 TP6 (marker), ch2 the IR LED anode current
  (shunt) or the 3.3 V rail at the sensor (AC coupled).
- **Do:** sampling ON at the default CONFIG (1 kHz, PS_IT code 3).
  Then try PS_IT 0 and 7, MPS 0 and 3 (write CONFIG from the controller
  later, or edit `laneRegsDefaultConfig` for now).
- **Expect:** marker period 1000 µs ±1 %, marker low ≈ trigger write
  (~90 µs) + conversion + read (~130 µs). The IR pulse sits inside the low
  phase. Data changes every sample (trace level 3).
- **Measure and write back** (all marked BENCH in the code):
  `LANE_REGS_VCNL_T_US`, `LANE_REGS_VCNL_OVERHEAD_US` (conversion end:
  shorten the wait until the value goes stale), `LANE_REGS_I2C_TRIGGER_US`,
  `LANE_REGS_I2C_READ_US` (`lane_regs.h`), and
  `LANE_SAMPLER_LATENCY_TRIM_US` (`lane_sampler.h`: trigger instant to the
  middle of the IR pulse, minus the computed value). The shortest working
  period answers HARDWARE_REFERENCE §7 / guide §9 ("reachable sample rate").
- **If not:** `ovr` counting: period shorter than the cycle. `err` counting:
  I2C errors under the IR pulse (rail noise: check C6, C8).

## 5. Signal levels at 15 cm (standalone)

- **Setup:** debug build with `LANE_BENCH_STANDALONE 1`; lane on a track
  piece, foil under it, trace level 2.
- **Do:** read the baseline; vary IR current (code 0..7) and PS_IT; hold a
  car under the sensor; put a white sheet over the foil.
- **Expect:** baseline well above noise, not at 65535 (else `SATURATED`
  events); car level < 70 % of baseline (default LOW). Note baseline, car
  minimum and noise per setting: they choose the defaults of
  `irCurrent`, `psIntegration`, `thrDropPermille`, `hystPermille`.
- **If not:** baseline 65535: lower IR current. Car barely changes the
  signal: sensor height / foil alignment.

## 6. Detection with real cars (standalone)

- **Setup:** as 5, on the test straight, cars at race speed.
- **Do:** 20 passes per car type; 10 hand blocks of ~2 s; flick a finger
  through quickly (< 2 ms); a car with a body gap (open cockpit).
- **Expect:** one `EV ... CROSSING` per pass, LED flash each; dwell 15–30 ms
  (`dwell=` in µs); `MERGED` flag only on gapped cars, never two events per
  car. Hand: `BLOCKED` after 500 ms (LED stays lit), `CLEARED` with the same
  tEntry when lifted. Fast finger: no event (glitch < minDwell 2 ms).
- **If not:** two events per car: raise `mergeGapUs`. Missed cars: lower
  `thrDropPermille`. Spurious events: raise `hystPermille`; check noise.

## 7. Holdoff watch rate (standalone)

- **Setup:** as 6, defaults edited to `holdoffUs = 3 s`,
  `holdoffSamplePeriodUs = 10000`.
- **Do:** two passes 1 s apart, then one 4 s later.
- **Expect:** `holdoff rate on (10000 us)` after the first; second event
  carries flag 0x08 (IN_HOLDOFF) and is **not** dropped; `holdoff rate off`
  after 3 s; third event unflagged. Marker period 10 ms during holdoff.

## 8. SYNC capture

- **Setup:** function generator (or the controller) 1 kHz 3.3 V square
  wave on J3 pin 7 (SYNC); trace level 1.
- **Do:** run 1 min; detune to 1010 Hz; unplug SYNC for 1 s.
- **Expect:** `HB ... sync=1/0 edges=<+1000/s>`. Detuned: still locked
  (±6 % window). Unplugged: STATUS SYNC_LOST (`sync=0/0`), back to `1/0`
  within ms when replugged. Alignment (`/1`) needs test 9.
- **If not:** no edges: PA5 TIM2_CH1 capture, filter N8; check
  `laneTimerCaptureOverruns()`.

## 9. I3C target: enumeration of one lane

- **Setup:** bench controller on J3 (SCL, SDA, GND, 2.2 k SDA pull-up on
  the controller side), CHAIN_IN (J3 pin 2) driven by the controller, SYNC
  from test 8. Controller sequence (control guide §5.1): RSTDAA, ENEC HJ,
  raise CHAIN, accept the hot-join, ENTDAA with address 0x11.
- **Expect:** `CHAIN_IN high`, `hot-join request (0)`, then
  `ENUMERATED addr=0x11, CHAIN_OUT high`; scope J2 pin 2 goes high.
  Controller reads ID (28 B): boardType 2, proto 0.1, fw, CRC as in test 2,
  UID. Writes LANE_ID = 1 → `LANE_ID = 1`. Reads STATUS (24 B): state 1.
  Without the controller answering, the hot-join repeats at 20, 40, … 1000 ms.
- **If not:** no hot-join on the bus: `LL_I3C_IsEnabledHotJoin`, DEVR0;
  the controller must ACK 0x02. Address never arrives: DAUPD interrupt
  (`lane_bus.c`). **First thing to scope:** SCL/SDA levels with the 47 Ω
  series resistors on the controller.

## 10. Register map and FIFO reads

- **Setup:** as 9, lane enumerated; controller script.
- **Do and expect:**
  1. Write CONFIG (37 B with pointer) = defaults → STATUS flags has
     CONFIG_VALID (0x10), state 2 (IDLE).
  2. SYNC_ALIGN {tick, phaseUs, 1000} measured just before sending → STATUS
     SYNC_ALIGNED (0x02); repeat once per second, `syncShifts` stays 0.
  3. COMMAND SAMPLING 1 → state 3. Hand through the beam → STATUS fifoCount
     1 → read EVENT_FIFO (68 B requested): **the lane ends the read after
     4 + 16 bytes** (early end); record seq 0, type 1, tEntryUs ≈ controller
     sysUs of the pass, flags 0. Read again: count 0, header only.
  4. Make 6 events, read: count 4, remaining 2; then count 2.
  5. Abort a read after 30 bytes (controller stops early) → next read
     returns the second record again first (only fully clocked records pop).
  6. Bad writes, each must raise `badWrites` by one and set `lastError`:
     CONFIG one byte short (-41), CONFIG with phase ≥ period (-42), payload
     to STATUS (-43), register 0x0B (-40), COMMAND 0x7F ENTER_BOOTLOADER
     (-46), COMMAND SLEEP 2 (-46). A read of CONFIG returns the old values.
  7. Read STATUS, SIGNAL, CONFIG, LANE_ID without rewriting the pointer
     (bare read repeats the last register).
  8. Issue GETSTATUS and GETPID CCCs between two EVENT_FIFO reads → no
     record lost (`cccFrames` in `laneBusStatsData` counts them).
  9. COMMAND SAMPLING 2 (RAW), read RAW_FIFO (80 B): 32 samples, chunkSeq
     increments, firstUs + 32 × periodUs = next firstUs.
  10. IBI is off in v1 (`LANE_BUS_IBI_ENABLE 0`: the controller NACKs it).
      Poll STATUS and read the FIFO instead.
- **If not:** the first read byte wrong or late (turnaround after Sr): put a
  STOP between pointer write and read and compare; if that fixes it, the
  ISR is too slow at 1 MHz (raise its priority above TIM2 or simplify the
  build). Reads of 68/80 B cut at 64: MRL (CubeMX item, also set at init).
  Records popped on CCCs: see `laneBusCccSeen` in `lane_bus.c`.

## 11. Scheduled LED actions, two lanes

- **Setup:** two lanes aligned (test 10 steps 1–2 on each), scope ch1/ch2
  on both LEDs (PB8 or the LED anodes).
- **Do:** write to both: SCHEDULE CANCEL_ALL, ADD LED_ON at T0+2 s,
  ADD LED_OFF at T0+4.321 s (T0 = controller now). Ten times with different
  holds; once with an at-time already past; once with CANCEL_ALL in between.
- **Expect:** both LEDs go dark **within a few µs of each other** every
  time; trace `SCHED fired action=1 late=<few>us`. The past one fires at
  once and `lateActions` counts 1. After CANCEL_ALL nothing fires.
  STATUS `schedulePending` follows.
- **Pulse (LANE_ACT_LED_PULSE):** two lanes side by side, scope ch1/ch2 on
  the LED pins (or just watch them). Write to both: ADD LED_PULSE param 1000
  at T0+2 s, then ADD LED_ON (level 0 = keep) at T0+5 s, then a second run
  with ADD LED_PULSE at T0+2 s and ADD LED_OFF at T0+6 s (peaks at 3 s, 5 s,
  troughs at 2 s, 4 s, 6 s). Also param 250 and param 20; also WRITE the LED
  register (ON) mid-pulse.
  **Expect:** both LEDs breathe in phase (the two duty envelopes overlay,
  no drift over 60 s), dark to full in 1 s and back in 1 s; the ON at a peak
  and the OFF at a trough show no step; the LED register write ends the pulse
  at once. Pause SYNC (controller off) mid-pulse: breathing continues, phase
  drifts slowly. param 0 is rejected (`badWrites` +1).
- **If not:** offset of one SYNC period (1 ms) between lanes: SYNC_ALIGN
  latency above P/2, re-check the controller's measurement point. Actions
  never fire: lane not aligned (`sync=1/0`).

## 12. Chain of lanes

- **Setup:** 2…6 lanes plugged edge to edge, controller on lane 1.
- **Do:** power up; then unplug lane 3 and replug; reset lane 2 with SW1;
  RSTDAA with the chain up; LANE_SCAN (controller drops CHAIN).
- **Expect:** lanes join in order 0x11, 0x12, …; each `CHAIN_OUT high`
  after its address. Unplug lane 3: lanes 3–6 go UNENUMERATED (`CHAIN_IN
  low`), rejoin in order after replug. Reset of lane 2: lanes 2–6 rejoin.
  RSTDAA: every lane waits 60 ms (`CHAIN_REJOIN_HOLD_MS`), only lane 1 joins
  first — **no two lanes ever share an address**. Supply current with six
  lanes sampling: note it (HARDWARE_REFERENCE §7 "lane chain current").
- **If not:** two lanes answer one address: increase `CHAIN_REJOIN_HOLD_MS`
  and check CHAIN debounce on the scope.

## 13. Robustness

- **Do and expect:**
  1. `laneBenchRequest = 2` (hang): reset after ~1.5 s, `reset cause=4
     fault=4`. Lane rejoins by itself (controller pushes config again).
  2. Boot-loop guard: it stands down while C_DEBUGEN is set (hub rule), and
     that bit survives a probe unplug until power-on, which also loses the
     .noinit count. So: set `faultGuardBreadcrumb.abnormalResetCount = 5`
     in Live Watch, `laneBenchRequest = 2`, and detach the debugger
     (gdbserver detach clears C_DEBUGEN; confirm DHCSR bit 0 = 0 on a later
     attach). The next boot says `PARKED`: STATUS state 5, faultId 4, LED,
     sampling and SCHEDULE refused (-70), still enumerable. COMMAND RESET
     un-parks.
  3. Stop the controller for 6 s while sampling with the LED on: STATUS
     BUS_IDLE_SAFE (0x200), sampling OFF, LED off, address kept; the flag
     stays set through ordinary transfers and clears at the next COMMAND
     SAMPLING (or after address loss).
  4. Short SENS_SDA to GND for 1 s while sampling: one SENSOR_FAULT event,
     recovery (bus clear), sampling resumes by itself.
  5. COMMAND SLEEP 1: state 4, sensor off, LED off, still answering;
     SAMPLING 1 wakes it.
- **Option bytes** (before Standby is ever used): `IWDG_STDBY = 0`;
  BOOT_SEL so the BOOT0 pin (R1 pull-down) is honoured — **do not** copy
  the C542 hub script (guide §7).

## 14. Six lanes, cars, lights out

- **Setup:** full system: control module, six lanes, web app debug console.
- **Expect:** six lanes JOINED → CONFIGURED; a car on each lane appears as
  EVT_CROSSING with plausible tEntryUs; a start sequence lights 1…6 and all
  go dark together (scope two lanes, as test 11); lap times from two passes
  agree with a stopwatch to the 1/100 s.
