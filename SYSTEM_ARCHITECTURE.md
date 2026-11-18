# TimeTaker — System Architecture

Version 1 of the architecture, written 2026-11-16. It covers what runs where,
how the parts talk, and how the system grows. It spans all four pieces of
software:

- the lane module firmware (×6),
- the control module firmware (STM32),
- the BLE module firmware (nRF52805),
- the web app.

Hardware facts are in `HARDWARE_REFERENCE.md`. Each firmware has its own
brief for the sessions that write it:

- lane: `LaneModule/LaneModuleFirmware/LANE_FIRMWARE_GUIDE.md`;
- control: `ControlModule/SideModuleFirmware/CONTROL_FIRMWARE_GUIDE.md`;
- BLE: `ControlModule/BleFirmware/BLE_FIRMWARE_GUIDE.md`;
- web app: `WebApp/README.md` and §11 here.

§13 lists what this document changed in other documents. The code structure follows the Yuhka hub firmware
(`~/Desktop/ATWU_Firmware/hub_STM32C542_MainboardV3/…_cmake/app`):

- **hardware modules** with small APIs, one owner per peripheral;
- **coordinators** for groups of logically related tasks;
- **one master** that inits everything in order and runs the super-loop.

---

## 0. Decisions at a glance

| Question | Decision |
|---|---|
| Where are cars detected? | **On each lane module.** The lane samples fast, detects, and timestamps in system time. It sends *crossing events*, not samples. |
| Who counts laps, computes lap times, judges false starts, ranks? | **The web app**: a pure, event-sourced race engine. |
| What does the control module do then? | It is the **time master**, **lane bus manager**, **event log** (sequenced and replayable), **transport router** (USB/BLE), and the **real-time executor** (start lights, scheduled actions, later the power interlocks). It counts nothing. |
| Lap times on the device? | No. A lap time is `t(n) − t(n−1)` of two timestamps the host already has. Computing it on the device saves nothing and creates a second copy of race state. |
| "Sleep the lane for 20 s after a pass"? | Keep the logic and drop the literal sleep. Use a **host-set holdoff**: after a pass the lane drops to a slow watch rate (for example 100 Hz) and *flags* any event inside the window. It never goes blind. This is a config knob, so it needs no architecture change (§2.4). |
| USB identity | USB CDC-ACM device with the product string **"TimeTaker"**. The web app uses Web Serial and filters by VID/PID. |
| BLE | The nRF52805 is a **dumb bridge**: Nordic UART Service ↔ USART1. It advertises as **"TimeTaker"**. It never parses the host protocol. |
| BLE binding | Hold the button 2 s to open a 60 s pairing window (fast-blinking LED). The STM stores the list of bound web app client IDs. Over USB you are always trusted, and you can bind from there too (§9.5). |
| Firmware updates (control STM, lanes, nRF) | **v1: no field update.** Every board is flashed over SWD / Tag-Connect. No bootloader on any board; the freed RAM buffers events. A field update is deferred; the control bootloader that was built is at git tag `archive/bootloader-update` (§10). Recovery: SWD, or BOOT + NRST → ROM USB DFU. |
| Lane firmware update later | An I3C lane bootloader fed through the control module (the ROM has no I3C). The cheap hooks stay reserved (§10). |
| One protocol for USB and BLE | Yes. Same frames and same messages. A session can drop on one transport and **resume on the other without losing a crossing** (§7.4). |
| Future power electronics (FCY, crash detection) | A new node class behind the control module. It follows the same rule: classify at the edge, decide in the host, act at the edge on the system clock. Safety reflexes run on the control module (§12). |

---

## 1. The rule everything follows

> **Sense and classify at the edge. Decide at the host. Act at the edge, on
> one clock.**

```
            facts (timestamped, sequenced, replayable)         ▲ up
  ┌──────────┐   crossing{t}   ┌────────────────┐  events   ┌─────────┐
  │  LANE    │ ──────────────► │ CONTROL MODULE │ ────────► │ WEB APP │
  │ sense +  │                 │ clock, log,    │           │ decide: │
  │ classify │ ◄────────────── │ route, execute │ ◄──────── │ race    │
  └──────────┘  "LED off at T" └────────────────┘  commands └─────────┘
            intents (immediate, or scheduled at system time T)  ▼ down
```

- **Edges** are the lane modules and, later, the power board. They are the
  only places that see raw signals. They turn signals into **facts with a
  timestamp**: "car entered at T", "lane current dropped at T".
- **The host** (web app) owns every **decision that depends on context**:
  - is this crossing a lap,
  - is this a false start,
  - who leads,
  - when to call a full course yellow.

  Rules change often, and the host is where they are cheap to change, test and
  replay.
- **Timing-critical execution** (lights out, power on, power cut) never waits
  for a host round trip. The host sends *what* and *when*: "at T do X". The
  control module executes on the system clock. A small set of
  **host-armed reflexes** run without the host at all (§12.2).
- **Exactly one clock.** It is the control module's crystal. Every timestamp
  in the system lives on that axis. The host never timestamps anything.

---

## 2. Where each thing is processed

| Layer | Owns | Never does |
|---|---|---|
| **Lane module** | Sampling schedule, sensor driver, detection, timestamping (mapped to system time), event FIFO, sensor health, LED execution (including scheduled actions) | Lap logic, race state, anything about other lanes, persistence |
| **Control module STM** | System clock + SYNC, lane chain lifecycle (power, enumeration, config push), **event log**, host sessions on USB and BLE, start sequence and scheduled actions, host-armed reflexes, device config persistence, bind list | Lap counting, penalties, standings, any rule that can be decided later |
| **BLE module nRF** | Radio, advertising, the GATT bridge, bind button + LED, STM reset line (released) | Parsing host frames, storing anything about hosts, deciding anything |
| **Web app** | Race engine (laps, lap times, positions, gaps, false starts, reaction times, penalties, race end), race config, persistence, all views, all policy | Timestamping. It only ever does arithmetic on device timestamps. |

### 2.1 Why the lanes detect

Detection does not belong on the control module or in the web app:

1. **Bandwidth.** Raw data is 6 lanes × ≥ 1 kHz × 2–4 B = 12–24 KB/s. That
   saturates I3C at 1 MHz and is far too much for BLE. A crossing event is
   about 16 B per car pass.
2. **Precision.** The lane timestamps at the sample instant and interpolates
   between the samples around the crossing. No bus latency touches the
   timestamp.
3. **Compute.** Each lane has a whole 144 MHz M33 for one sensor. That leaves
   room for robust detection: adaptive baseline, thresholds derived from
   measured noise, a merge gap for cars with body/wing gaps, dwell-time
   validation, and saturation and dirty-foil monitoring. The sample rate is
   limited by the VCNL4040 conversion time, not by the MCU (bench item).
4. **Diagnostics stay possible.** Raw samples are streamed on demand for one
   lane at a time. A later option keeps a raw snapshot window around each
   event, fetched by event number (§5).

### 2.2 Why the control module does not count laps

Counting laps on the control module would be easy. It is the wrong place:

1. **Nothing is gained.** Lap time is a subtraction of two timestamps the host
   receives anyway.
2. **Lap validity depends on context the device should not own:**
   - the start phase,
   - the first crossing after lights out is not a lap,
   - false start penalties,
   - a marshal putting a deslotted car back *before* the line,
   - manual corrections,
   - race finished,
   - later stop-and-go and FCY rules.

   All of these are rules. Rules belong where they can change without a
   reflash and can be regression-tested against recorded races.
3. **One source of truth.** Two copies of derived state (device lap counter
   and app lap counter) drift apart sooner or later. One immutable event log
   plus one pure race engine cannot disagree with itself.
4. **Robustness comes from the log, not from on-device counting.** The control
   module keeps every event with a sequence number. A browser tab reload, a
   laptop crash, a BLE dropout, or a switch from USB to BLE all end in
   `RESUME(fromSeq)`, a replay, and identical standings (§7.4).

The door stays open. A standalone mode (no laptop, Arduino HDMI display) would
add a `race_engine` coordinator on the control module. It would consume the
same event log. Nothing else would change.

### 2.3 Why the web app is the race engine

The race engine is a **pure function**:
`raceState = fold(raceConfig, deviceEvents + hostActions)`.

- **Testable** with recorded logs. Every race you run becomes a regression
  test.
- **Recoverable.** Rebuild the state from the log at any time.
- **Correctable.** A manual "+1 lap", a penalty or a disqualification is a
  *host action event* appended to the same log. It is auditable and undoable.
- **Multi-view.** The control window and the TV window run the same engine,
  or the TV window just subscribes.

### 2.4 Sleeping the lane after a pass (holdoff)

The idea: if no lap can be faster than, say, 50 s, the lane can stop looking
for 20 s after a car passes.

| | Literal sleep (sensor off for 20 s) | **Holdoff watch (chosen)** |
|---|---|---|
| IR pulses saved | 100 % in the window | about 90 % (1 kHz → 100 Hz) |
| What a wrong holdoff costs | **A silently lost lap** | An event flagged `IN_HOLDOFF`; the host decides |
| Sees a car parked on the line, a hand in the beam, a marshal reset | No | Yes |
| Baseline keeps tracking | No | Yes |

- **Benefit:** the system runs on USB power, so the gain is fewer 200 mA IR
  pulses on the shared rail (less noise and heat), not battery life. That is
  real, but it is not worth going blind for.
- **Mechanics:** the lane CONFIG gets `holdoffUs` and `holdoffSamplePeriodUs`.
  The host sets them adaptively, for example 0 until the first valid lap, then
  55 % of the fastest valid lap in this session.
- **Order:** implement it after the basic pipeline works. It needs no other
  change.
- **The holdoff never deletes anything.** The lane does not reject events
  inside the minimum lap time; it **flags** them. The only thing the lane
  suppresses is chatter *within one pass*, via the merge gap: a dip, a short
  rise, then a dip again within about 30 ms counts as one car.

---

## 3. Time

| Item | Definition |
|---|---|
| **System time** | `sysUs`: u64 µs since control module boot. TIM12 at 1 MHz from the 24 MHz crystal (±20 ppm, about 1 ms per 50 s, the same for all lanes). |
| **SYNC** | TIM12_CH2, 1 kHz. Rising edge = tick k = `k × 1000` µs. |
| **Lane mapping** | Lane TIM2 (local 1 µs, HSI ±1 %) captures SYNC edges. `sync_time` maps local → sysUs per period. This is implemented and host-tested. The epoch comes from a bus message (tick + µs-since-edge) with no marked edge. Every mapped time carries `NO_LOCK / UNALIGNED / EXTRAPOLATED` flags. |
| **Boot identity** | The control module draws a random 32-bit `bootId` (TRNG) at power-up. sysUs and the event sequence restart with every boot. The host keys everything on `(bootId, evSeq)`. A new bootId mid-race means a timing gap; the app shows it as a flagged gap. |
| **Host clock** | Used only to *animate* a running clock. The app estimates `sysUs ↔ performance.now()` from PING round trips (midpoint). That is ±1 ms on USB and ±tens of ms on BLE. **Results never use it.** |

Precision budget for one crossing:

- 1 ms sample period with linear interpolation: about 0.1 ms;
- SYNC mapping: µs;
- crystal: 20 ppm.

That is well inside 1/100 s. Transport latency (USB about 1 ms, BLE 7.5–50 ms)
does not affect accuracy, because timestamps are taken at the edge.

---

## 4. One car pass, end to end

```
Lane n          Control module                         Web app
──────          ──────────────                         ───────
sample @1 kHz (phase-staggered per lane: the 200 mA IR pulses never overlap)
signal < LOW  → entry instant interpolated → tEntry (sysUs)
signal > HIGH for > mergeGap → exit → push CROSSING{seq, tEntry, dwell, depth, flags}
IBI (1 byte) ───► read EVENT_FIFO (I3C private read)
                  check per-lane seq gaps
                  append to event_log → evSeq++ (global)
                  each session pump: cursor < head → send EVT_CROSSING ──► eventStore.append (IndexedDB)
                                                                           raceEngine.fold → lap 17, 8.432 s, P2
                                                                           control + TV views update
```

- **When the lane emits:** on **exit**, or after a `blockTimeoutUs` (about
  500 ms) with type `BLOCKED`, for example a car parked on the line or a hand.
- **Why exit and not entry:** the event then carries dwell and depth, so a
  glitch (too short) or a hand (too long) can be told apart. The 15–30 ms this
  adds is invisible on a display.
- **The timing reference is `tEntry`**, the car's nose crossing the line.
- **Latency** from car to screen: under 50 ms on USB and under about 100 ms
  on BLE.

---

## 5. Lane module

The structure in `LANE_FIRMWARE_GUIDE.md` §6 stands. The changes this
architecture makes are listed in §13. To keep the lane master small, group the
modules into two coordinators:

```
app/coordinators/
  lane_master.c     init order, super-loop, error policy, lane state machine (§3.1 of the guide)
  sense_manager.c   lane_sampler → vcnl4040 → lane_detect → event_fifo; holdoff; raw stream; calibration
  bus_manager.c     lane_bus (I3C target, register map, IBI) + lane_chain; turns register writes into
                    calls on sense_manager / indicator_led / the scheduled-action queue
app/modules/        indicator_led, led_pattern, lane_timer, sync_time (done), vcnl4040, lane_sampler,
                    lane_detect (pure), event_fifo (pure), lane_bus, lane_chain, sys_power, fault_guard
```

**Crossing event (lane → control), draft.** `lane_protocol.h` is the
authority.

| Field | Type | Meaning |
|---|---|---|
| seq | u16 | Per-lane counter. The control module detects gaps from it. |
| type | u8 | `CROSSING`, `BLOCKED`, `CLEARED`, `SENSOR_FAULT` |
| flags | u8 | Sync flags (`NO_LOCK`, `UNALIGNED`, `EXTRAPOLATED`), `IN_HOLDOFF`, `MERGED`, `SATURATED` |
| tEntryUs | u64 | sysUs of the interpolated entry |
| dwell | u16 | Car under the sensor, in units of 10 µs (max 655 ms) |
| depth | u16 | Signal minimum relative to baseline: a confidence measure |

That is 16 B. FIFO depth is ≥ 32.

**Sampling modes the lane needs.** The lane needs no race states. It does not
need ARMED or RACE, because false starts are judged in the host against
`LIGHTS_OUT`. Its modes are:

| Mode | Meaning |
|---|---|
| `OFF` | Sensor in shutdown |
| `ON` | Period and phase from CONFIG, plus holdoff |
| `RAW` | Additionally streams `(t, value)` for diagnostics, decimated |

**Later, optional: event snapshots.** Keep the last N samples around each
event (for example 32 before and 32 after, 8 events deep, about 1 KB of RAM).
The host fetches them by `seq`. This lets you tune detection at the venue
without streaming everything.

---

## 6. Control module firmware

### 6.1 Structure

```
LapCounterControlModule_cmake/
  main.c                     mx_system_init(); ctrlInit(); while (1) ctrlRun();
  app/
    config/
      ctrl_behavior.h        knobs only (debug vs RELEASE), like hub_behavior.h
      ctrl_status.h          ctrlStatus_e: 0 OK, negative errors grouped in tens; CTRL_ASSERT* macros
      lane_protocol.h        byte-identical copy of the lane tree's (host test checks it)
      host_protocol.h        GENERATED from protocol/ (§7.7); do not edit
    coordinators/
      ctrl_master.c/h        THE master: init order, super-loop, error policy, link-loss policy, idle
      host_manager.c/h       sessions on USB and BLE: HELLO/bind, request dispatch, event pump, streams
      lane_manager.c/h       lane chain: power, enumeration, config push, SYNC align, event collection, health
      sequence_manager.c/h   start sequence, scheduled actions at sysUs ("timeline"), abort
      panel_manager.c/h      LED_STATUS/LED_ACT patterns, BTN1-3 → host events + local fallbacks
      (power_manager.c/h)    later: power board node, telemetry, reflexes (§12)
      (display_manager.c/h)  later: Arduino HDMI on UART4
    modules/                 one owner per peripheral and pin; no policy
      sys_time               TIM12: sysUs (u64), SYNC output, 1 ms tick, sysUs alarms
      i3c_ctrl               I3C1 controller: async transfer queue, DAA, hot-join, IBI, error recovery
      lane_chain             LANE_PWR_EN, CHAIN_OUT, BUS_INT; parks bus pins when lanes are off
      usb_cdc                TinyUSB glue: "TimeTaker" descriptors, CDC rx/tx rings, USB_FAULT
      nrf_link               USART1 (DMA) + nRF link framing (DATA/CTRL channels), BLE_IRQ, BLE_GPIO1/2
      frame_codec            COBS + CRC16, pure, host-tested (shared with the nRF bridge)
      event_log              sequenced ring of host events, pure, host-tested
      host_session           session layer per transport: HELLO/PING/RESUME, dispatch, event pump with
                             backpressure; pure (transport and clock as function pointers), host-tested
      device_id              MCU UID (USB serial), bootId, reset cause
      settings_store         flash pages with CRC: bind list, lane config table, device name
      rng                    TRNG: bootId, start-delay randomness, nothing else
      indicator_led          one instance each for TIM8/PB13 and TIM1/PB8 (port of the lane driver)
      button_handler         BTN1-3 debounce, short/long press
      fault_guard, sys_power ports from the hub
      (power_link)           later: USART2 + EXT_PWR_EN/FAULT framing to the power board
      (aux_uart)             later: UART4 to the Arduino
  test/                      host tests: frame_codec, event_log, sequence planner, protocol copies
  docs/                      ERROR_HANDLING.md, RELEASING.md (like the hub)
```

### 6.2 Who calls whom

- **Ownership:**
  - modules own hardware and know nothing about the race;
  - coordinators own behaviour and call modules;
  - `ctrl_master` calls coordinators.
- **`host_manager` owns the protocol, not the behaviour.** It decodes a
  request and calls the public API of the coordinator that owns it. Example:
  `LANE_LED` → `laneManagerSetLed()`, `START_SEQUENCE` →
  `sequenceManagerStart()`.
- **Coordinators never send frames.** They publish facts by appending to
  `event_log` (`eventLogAppend(EVT_LANE_JOINED, …)`). `host_manager` pumps the
  log out to each session.
- **One owner per resource.** `lane_manager` is the only user of `i3c_ctrl`.
  `sequence_manager` queues its time-critical lane writes through
  `laneManagerSchedule()`, which jumps the queue.

### 6.3 Super-loop and timing

```c
void ctrlRun(void)
{
    faultGuardTick();
    buttonHandlerPoll();
    ctrlErrorHandler(laneManagerRun());      /* bus transactions, enumeration, event collection */
    ctrlErrorHandler(sequenceManagerRun());  /* due actions, lead-time scheduling */
    ctrlErrorHandler(hostManagerRun());      /* rx frames → dispatch; pump events/streams per session */
    panelManagerRun();
    ctrlLinkLossPolicy();                    /* only matters once there are reflexes (§12.2) */
    sysPowerIdle();                          /* WFI; USB-powered, no Stop needed */
}
```

- **Nothing blocks.** Every wait is a state-machine step. Target: loop < 1 ms,
  and an IWDG at about 1–2 s fed once per loop.
- **Hard real time lives in ISRs and timers:**
  - the SYNC tick (prio 1);
  - sysUs alarms for board-local scheduled actions;
  - I3C completion and IBI (prio 2);
  - EXTI for power faults (prio 0).

### 6.4 Start sequence (the first "act at the edge on one clock" feature)

1. The host sends `START_SEQUENCE{lights=6, stepMs=1000, holdMinMs=200,
   holdMaxMs=3000, armSampling=1}`.
2. `sequence_manager` plans the sequence:
   - `T0 = now + lead`, with lead ≥ 150 ms to write all lanes over the bus;
   - lane k LED on at `T0 + k·step`;
   - a TRNG hold h from [holdMin, holdMax];
   - `Tout = T0 + 6·step + h`, all LEDs off at Tout.
3. It writes each lane's **scheduled actions** (LED on at T, LED off at Tout)
   through `lane_manager`. Each lane executes them from its own TIM2 compare on
   the mapped clock. All six go dark within a few µs of each other, whatever
   the bus latency.
4. It emits `EVT_SEQ_STARTED{T0, plan}` at once, and `EVT_LIGHTS_OUT{Tout}`
   when Tout passes. Tout is never revealed earlier.
5. `ABORT_SEQUENCE` (or a long press on BTN2) cancels the queued lane actions,
   blinks all lights, and emits `EVT_SEQ_ABORTED`.

False start, reaction time and "first crossing is not a lap" are all decided
in the web app by comparing crossings with `Tout`.

---

## 7. Host protocol (same on USB and BLE)

### 7.1 Layers

```
 web app   │ raceEngine ← deviceSession ← codec ← transport (WebSerial | WebBluetooth | Sim)
───────────┼──────────────────────────────────────────────────────────────────────────────
 bytes     │ USB CDC byte stream          │ BLE NUS byte stream ──► nRF ──► USART1 (DATA channel)
───────────┼──────────────────────────────────────────────────────────────────────────────
 control   │ usb_cdc ─┐                   nrf_link ─┐
           │          └──► frame_codec ──► host_manager (one session per transport)
```

### 7.2 Framing

- The frame is `COBS( header | payload | crc16 ) 0x00`.
  - COBS gives a zero delimiter that resynchronises after garbage at any
    point.
  - CRC-16/CCITT-FALSE checks each frame. The C5 CRC unit can compute it.
- The header is 4 bytes: `msgId u8 | flags u8 | tag u16`.
  - flags: RESPONSE, EVENT, STREAM.
  - tag: request ID for requests and responses, 0 for events and streams.
- **Every response payload starts with `status i16`** (`ctrlStatus_e`). On
  error the payload is the status alone. **Every event payload starts with
  `evSeq u32`.** The generator injects both fields.
- Maximum payload is 240 B. All values are little-endian. Structs are packed
  and fixed-size, with no varints. Only the last field may be a variable
  "tail".
- Implemented in `frame_codec.c/h` (C, shared with the
  nRF) and `WebApp/src/protocol/codec.ts`. Both are held byte for byte to the
  reference encoder in `protocol/gen_protocol.py`.

### 7.3 Three traffic classes

| Class | Direction | Delivery | Examples |
|---|---|---|---|
| **Request / response** | host → device → host | Reliable by retry. Every request gets exactly one response with a `ctrlStatus_e`. Requests are idempotent where possible. | HELLO, LANE_CONFIG_SET, START_SEQUENCE |
| **Events** | device → host | **Sequenced (`evSeq` u32), logged, replayable.** Never dropped. A session that falls behind just lags. | CROSSING, SEQ_STARTED, LIGHTS_OUT, LANE_JOINED/LEFT/FAULT, BUTTON, POWER_FAULT |
| **Streams** | device → host | Best effort, rate-limited per transport, **dropped first** under backpressure, never logged | Raw lane samples, telemetry, debug log |

### 7.4 Sessions, replay and switching transports

- **One session per transport.** Up to two exist at once (USB + BLE). Each has
  a **cursor** into `event_log`. The pump sends `log[cursor..head]` as fast as
  that transport accepts.
- **HELLO** carries the client's protocol version, `clientId` and name. The
  device answers with:
  - protocol version, firmware versions, `bootId`;
  - log head and tail, current sysUs;
  - the **node list** (§7.5).
- **RESUME(bootId, fromSeq)** moves the cursor back.
  - Same bootId, and `fromSeq` still in the log: the device replays and
    nothing is lost.
  - `fromSeq` already overwritten: the device answers `ERR_LOG_GAP` with the
    oldest sequence it has, and the app flags the gap.
- **Pulling the USB cable mid-race:**
  1. The app's deviceSession notices the transport closing.
  2. It reconnects over BLE (one click in Chrome's chooser) or USB again.
  3. It sends HELLO and RESUME.
  4. The missed crossings arrive.
  5. The standings come out identical.

  This is a required test.
- **Log size:** compact 12–16 B records, 1024+ entries in RAM. That covers
  link dropouts of many minutes. Long-term history is the **app's** job
  (IndexedDB), not the device's.
- **Two sessions at once** (a laptop on USB and a tablet on BLE): both get all
  events. Both may command. Every state change goes out as an event, so all
  UIs converge. HELLO lists the other active session so the app can warn
  "another controller is attached".
- **Heartbeat:** the host sends PING every 1 s. After about 3 s of silence the
  session is *stale*. In v1 that only affects the panel LED. With power
  electronics it triggers the configured safe state (§12.2).

### 7.5 Node model and message ranges

Everything addressable is a **node**: `(class u8, index u8)`.

| Class | Index | Present in v1 |
|---|---|---|
| `CTRL` | 0 | Yes |
| `LANE` | 1–6 | Yes, as enumerated |
| `BLE` | 0 | Yes (versions and RSSI via the nRF CTRL channel) |
| `POWER` | 0 = board, 1–6 = lane channels | Later |
| `DISPLAY` | 0 | Later (Arduino HDMI) |

HELLO returns the node list with board types and firmware versions. **The web
app builds its UI from it.** A power panel appears only when a POWER node
exists. Adding hardware therefore adds message IDs and a node class. It never
changes the shape of the protocol.

Message ID ranges (draft, grouped like the status codes, with gaps):

| Range | Group |
|---|---|
| 0x00–0x0F | Session: HELLO, PING, RESUME, BIND_LIST, BIND_ALLOW, BIND_REMOVE |
| 0x10–0x1F | System: INFO, STATUS, REBOOT, SETTINGS_GET/SET/SAVE |
| 0x20–0x3F | Lanes: SCAN, POWER, CONFIG_GET/SET, CALIBRATE, SAMPLING, LED, STATUS, RAW_STREAM |
| 0x40–0x4F | Sequences: START_SEQUENCE, ABORT_SEQUENCE, SCHEDULE, CANCEL |
| 0x50–0x5F | Stream subscriptions and telemetry rates |
| 0x60–0x7F | **Reserved: power** |
| 0x80–0x8F | **Reserved: display / aux** |
| 0xA0–0xAF | **Reserved** (held the update messages until 0.4; never reuse) |
| 0xC0–0xDF | Events (device → host) |
| 0xE0–0xEF | Streams (device → host) |
| 0xF0–0xFF | Debug streams |

Protocol 0.4 (2026-11-18) defines these messages:

- **Session:** HELLO, PING, RESUME, NODE_LIST, BIND_LIST, BIND_REMOVE,
  BIND_ALLOW
- **System:** REBOOT, STATUS, SETTINGS_GET/SET
- **Lanes:** LANE_SCAN, LANE_POWER, LANE_CONFIG_GET/SET, LANE_SAMPLING,
  LANE_LED, LANE_STATUS, LANE_RAW_STREAM, LANE_CALIBRATE
- **Sequences:** START_SEQUENCE, ABORT_SEQUENCE
- **Events:** EVT_BOOT, EVT_CROSSING, EVT_LANE_STATE, EVT_BUTTON,
  EVT_SEQ_STARTED, EVT_LIGHTS_OUT, EVT_SEQ_ABORTED, EVT_BIND_CHANGED,
  EVT_BLE_LINK, EVT_FAULT (every event ≤ 24 B, the event-log slot; the
  generator enforces it)
- **Streams:** STR_RAW_SAMPLES, DBG_LOG

New in 0.3 (no new messages): HELLO's response reports `boardType`;
`faultSource` gains SETTINGS (EVT_FAULT once at boot when stored settings
failed their CRC and defaults were loaded).

New in 0.4: **no field update in v1.** UPDATE_BEGIN/CHUNK/END/ABORT and
BOOT_APP are gone (0xA0–0xAF reserved, the generator refuses them), as is
`[usb] pid_boot`. `CONTROL_BOOT`, `LANE_BOOT`, `REBOOT_BOOTLOADER` and the
ERR_IMAGE_*/ERR_UPDATE_SEQUENCE codes keep their values, marked reserved;
REBOOT{BOOTLOADER} answers ERR_UNSUPPORTED.

The rest of the table is reserved. `protocol/host_protocol.toml` is the
authority. The lane bus contract is `lane_protocol.h` and the nRF link
contract is `ble_link_protocol.h`, each byte-identical in two trees with a
copy check in the host tests.

### 7.6 Versioning

- **Protocol `MAJOR.MINOR`.** MINOR adds messages or fields at the end of
  payloads. MAJOR breaks.
- **The device rejects an unknown request** with `ERR_UNSUPPORTED`.
- **The app ignores unknown events and streams.** Old apps keep working
  against new firmware.
- **Mismatches show in the UI:** firmware versions of every node are in HELLO.
  The app shows "lane 4 runs 1.2.0, others 1.3.0" and offers an update.

### 7.7 One source for C and TypeScript

The lane bus protocol stays a C header: `lane_protocol.h`, byte-identical in
both trees, with a copy-check host test, as the guide says.

The host protocol is shared by C and TypeScript, so it gets a schema:

```
protocol/
  host_protocol.toml       messages, fields, types, IDs, status codes, enums, examples
  gen_protocol.py          stdlib Python (tomllib), writes:
                           → ControlModule/.../app/config/host_protocol.h       packed C structs
                           → ControlModule/.../test/host_protocol/golden_cases.h examples as C structs + bytes
                           → WebApp/src/protocol/generated.ts                   descriptors, types, wrappers
                           → protocol/golden/golden.json                        the same bytes for the TS tests
```

- **Three implementations agree byte for byte.** The generator packs every
  TOML example with its own reference packer and COBS encoder. The C host
  tests hold the firmware's structs and `frame_codec` to those bytes, and the
  TS tests hold `wire.ts` and `codec.ts` to the same bytes.
- **The generated files never drift.** `test/run_all.sh` runs
  `gen_protocol.py --check`, so a stale output fails the build.
- **The status codes (`ctrlStatus_e`) live in the same schema.** The app can
  show the *name* of an error, and the firmware uses the same enum
  internally.
- **TOML, not YAML:** Python's standard library parses it, so the generator
  needs no dependencies.

---

## 8. USB: showing up as "TimeTaker"

| Item | Choice |
|---|---|
| Class | **CDC-ACM** (virtual COM port). Works out of the box on Windows 10+, macOS and Linux. The web app uses **Web Serial** (Chrome/Edge). Plain tools (`screen`, Python) work for debugging. |
| Strings | iManufacturer "TimeTaker", **iProduct "TimeTaker"**, iSerialNumber = MCU 96-bit UID in hex. Chrome's port picker shows "TimeTaker (COM5)" or "TimeTaker (ttyACM0)". |
| VID/PID | The web app filters `requestPort({filters:[{usbVendorId, usbProductId}]})`. Pick a proper pair before the design is published: pid.codes (VID 0x1209) for open hardware, or a PID from ST. Any unused pair works on the bench. One PID (no bootloader in v1). |
| Stack | **TinyUSB.** Light, no RTOS, CDC + WebUSB + MS OS 2.0 descriptors. Its `stm32_fsdev` driver covers this USB_DRD_FS IP (H5/G0/U0/C0). Check upstream for C5 support; the family glue should be small. The CubeMX project has only the PCD driver, as intended. |
| Linux permissions | `/dev/ttyACM*` needs the user in the `dialout` group, or a udev rule with `TAG+="uaccess"` for the VID/PID. Ship the rule with the app. |
| Session start | Never gate TX on DTR. A session starts only with HELLO. |
| Later, optional | A WebUSB descriptor with a landing-page URL: Chrome then offers "open TimeTaker" when you plug the box in. |

---

## 9. BLE module (nRF52805)

### 9.1 Role

- **It is a boring bridge, and it must stay boring.** It can only be
  reflashed over SWD and has 24 KB of RAM, so all intelligence lives in the
  STM.
- **What it does:**
  - advertises as "TimeTaker";
  - bridges the Nordic UART Service to USART1;
  - runs the bind button and LED;
  - holds the STM's NRST released and BOOT_REQ high (both unused in v1).
- **What it never does:** it never parses host frames.

### 9.2 Structure (NCS v3.4.2, Zephyr)

```
ControlModule/BleFirmware/
  boards/              custom board def: MDBT42T, LFRC (no 32k crystal), no DCDC
  prj.conf             based on peripheral_uart minimal; RAM-trimmed buffers
  src/
    main.c             bridge master: init order, state machine (idle/advertising/connected/pairing)
    ble_link.c/h       advertising, NUS service, 2M PHY, DLE/MTU (as RAM allows), conn params 7.5–30 ms
    stm_link.c/h       UARTE async DMA @460800, link framing (DATA/CTRL), rx/tx rings
    bind_ui.c/h        button (P0.00) press classification, LED (P0.20) patterns (GPIO: the nRF52805 has no PWM)
    stm_ctrl.c/h       NRST (open drain, released), BOOT_REQ (held high, unused in v1), BOOT0 (undriven)
```

### 9.3 nRF ↔ STM link

Both directions use the same codec as the host protocol: COBS + CRC16, one
leading **channel** byte.

| Channel | Payload | Notes |
|---|---|---|
| `DATA` (0) | Opaque host-protocol bytes, ≤ 244 per frame | BLE writes and notifications are wrapped and unwrapped. Host frames may span several DATA frames. |
| `CTRL` (1) | Small fixed messages | nRF → STM: `NRF_HELLO{versions, credits, btAddr}`, `LINK_UP{mtu, phy, …}`, `LINK_DOWN{reason}`, `BUTTON{short/long/veryLong}`, `CREDIT{n}`, `STATS{rssi, …}`. STM → nRF: `STM_HELLO`, `UI_STATE{session, pairWindow}` (sets the LED), `CUE{bindAccepted/bindsCleared}`, `KICK`, `ADVERTISE{on/off}`. Byte layouts: BLE_FIRMWARE_GUIDE §5.3. |

The nRF interleaves CTRL with DATA only at frame boundaries. A CTRL message
therefore never splits a host frame.

- **Flow control STM → nRF is by credits:** the nRF grants one credit per
  free 244 B DATA slot and returns it when the slot's notifications are
  sent. There are no RTS/CTS wires. The other direction needs none.
- **The nRF splits a DATA frame to the current MTU,** so the STM never
  depends on the MTU for correctness.
- **The STM owns the pairing window and every bind decision.** The nRF
  reports button presses and renders `UI_STATE`; it holds no state that
  matters.

### 9.4 GPIO roles

| Line | Direction | Role |
|---|---|---|
| BLE_IRQ (P0.05 → PB5) | nRF → STM | High while a central is connected. The STM knows the link state without parsing anything. |
| BLE_GPIO1 (P0.04 → PB4) | nRF → STM | **BOOT_REQ, active low. Unused in v1** (no STM bootloader): the nRF drives it high. PB4 has the NJTRST pull-up, so an nRF in reset reads as "no request". |
| BLE_GPIO2 (P0.01 ↔ PA15) | — | Spare. Candidate: STM → nRF "STM alive". |
| NRST (P0.14) | nRF → STM | Open-drain pulse only: "reset the STM" |
| BOOT0 (P0.12) | nRF → STM | Recovery only: enters the ROM bootloader (USB DFU needs a cable anyway) |

### 9.5 Binding with the button and the LED

**Why at the application layer.** Laptops and phones use random BLE
addresses. So "binding" by BT address needs LE Secure Connections bonding
(SMP, IRKs, flash settings), which does not fit in 98.75 % RAM. Instead the
**web app** carries a random 128-bit `clientId` (kept in localStorage) and the
**STM** keeps the bind list (up to 8 entries, LRU) in flash. The nRF stays
stateless.

**The flow:**

1. **The nRF always advertises** as "TimeTaker" while no central is connected.
2. **The app connects and sends HELLO{clientId, name}.**
   - **Bound** → session OK, and the LED goes steady.
   - **Not bound, pairing window open** → the STM adds the client. The LED
     gives 3 quick flashes, and the session is OK.
   - **Not bound, window closed** → `ERR_NOT_BOUND`. The app says "Hold the
     BLE button on the TimeTaker for 2 s". The session stays connected but
     can do nothing except wait. When the window opens, the STM accepts it
     **live**: connect first, press the button second, and it just works.
     After 60 s unbound → KICK.
3. **USB is trusted** (physical access). Two shortcuts follow from that:
   - a USB session can send `BIND_ALLOW` to open the window from the app, with
     no button needed;
   - it can bind the browser it runs in directly.

| Button (SW501) | Action |
|---|---|
| Hold 2 s | Open the pairing window for 60 s |
| Hold 10 s | Clear all bindings (the LED changes pattern at 10 s; release to confirm) |
| Short press | Show status (blink the LED once) |

| BLE LED (D603) | Meaning |
|---|---|
| Off | BLE disabled by setting |
| Short blink every 3 s | Advertising, nobody connected |
| Double blink every 2 s | Connected, **not bound** (waiting for the button) |
| Fast blink (5 Hz) | Pairing window open |
| Steady on | Bound host connected (no dimming: the nRF52805 has no PWM) |
| 3 quick flashes | Bind accepted |
| One long flash | Binding list cleared |

**Security, honestly stated.** This keeps a curious kid's phone from running
the race. It does not stop a determined attacker: the token crosses an
unencrypted link and can be sniffed. If that ever matters, enable LESC
bonding on the nRF (if RAM allows) for an encrypted link. The app-level
binding stays as it is.

### 9.6 Web Bluetooth notes

- **Where it works:** Chrome/Edge on Windows, macOS, ChromeOS and **Android**.
  An Android tablet as the race director's remote is a real use case. Linux
  Chrome may need the experimental-platform flag. There is nothing for
  Firefox, Safari or iOS.
- **Reconnecting:** this needs a user gesture in the chooser, unless
  `navigator.bluetooth.getDevices()` is available.
- **Device filter:** NUS service UUID + namePrefix "TimeTaker".
- **Throughput:** events need bytes per second. Raw diagnostics need a few
  KB/s, which is fine with 2M PHY and DLE. The app limits stream rates when it
  is on BLE.

---

## 10. Firmware updates

**v1 has no field firmware update** (decided 2026-11-18). Every board is
flashed over SWD / Tag-Connect at home: the control STM32 (J201), the six
lane STM32s (Tag-Connect) and the nRF52805 (J501). There is no bootloader on
any board.

- **Control STM:** the app links at 0x0800_0000 (240 KB); the last two 8 KB
  pages (0x0803_C000) hold the settings (bind list, lane config table,
  device name), and `flash_drv` writes nothing else. REBOOT{BOOTLOADER}
  answers ERR_UNSUPPORTED. The RAM the bootloader hand-over and the update
  path used goes to the event log (1600 events, about 8 KB RAM left free).
- **Lanes:** the event FIFO fills the RAM the lane does not use (2480
  events, about 16 KB left free), so nothing is lost while the bus or the
  control module is slow.
- **Recovery:** SWD; or SW604 BOOT + SW605 NRST → ROM USB DFU →
  STM32CubeProgrammer.
- **Mismatches still show:** NODE_LIST reports every node's version and git
  revision, so the app can warn "lane 4 runs 1.2.0, others 1.3.0".
- **Deferred, not designed away:** the control bootloader, the image header,
  the update messages and the web app updater were built and host-tested,
  then removed. They are at git tag `archive/bootloader-update`. The
  protocol keeps ids 0xA0–0xAF and the `*_BOOT` values reserved; the lane
  keeps COMMAND `ENTER_BOOTLOADER` and the "BOOT" image kind reserved. A
  later field update (an I3C lane bootloader that keeps CHAIN behaviour,
  relayed by the control module) starts from that tag.

---

## 11. Web app

Requirements that shape it:

- **Chrome/Edge only:** Web Serial and Web Bluetooth.
- **Secure context:** https or localhost.
- **Offline:** the track is at a relative's house, maybe with no internet. So
  it is an **installable PWA** with a service worker.
- **Raspberry Pi later:** it must still run unchanged there (from the concept
  doc).

```
WebApp/src/
  transport/   serialTransport.ts · bleTransport.ts · simTransport.ts      ← all implement Transport
  protocol/    generated.ts (from schema) · codec.ts (COBS+CRC16)
  device/      deviceSession.ts   HELLO/version/bind, requests with timeout+retry, RESUME, heartbeat,
                                  clock estimate, transport preference (USB > BLE), node model
               simDevice.ts       a fake control module: generates crossings for 6 cars → develop without hardware
  store/       eventStore.ts      append-only (bootId, evSeq) log in IndexedDB; export/import race files
  race/        raceEngine.ts      PURE fold(config, events, hostActions) → raceState; no IO, no timers
               rules/             race by laps / by time, false start, first-crossing, min-lap validation,
                                  penalties, finish; later FCY, stop-and-go
  director/    raceDirector.ts    UI intents → device commands (start/abort/calibrate) + host actions (penalty, ±lap)
  views/       control/ (laptop) · tv/ (fullscreen second window) · diagnostics/ (raw signal, thresholds,
               calibration, lane health, LED test) · setup/ (drivers, lanes, binding)
  sync/        BroadcastChannel: the TV window is a pure view of raceState
```

- **The device session hides the transport.** The race engine never knows
  whether bytes came over USB or BLE.
- **The simulator is a first-class transport.** The web app gets built and
  tested against `simTransport` while the hardware is on the bench.
- **Recorded races are regression tests.** Export the event store and replay
  it into `raceEngine` in a unit test.
- **Framework** is a separate decision. The layering above is
  framework-agnostic. Keep `race/`, `device/`, `protocol/` and `store/` free
  of UI-framework imports.

---

## 12. Growing it: power electronics, displays, standalone

### 12.1 The power board (J302: USART2 + EXT_PWR_EN/FAULT)

The power board joins the system like any other edge node:

| Concern | Where it lives |
|---|---|
| Sense per lane: voltage, current; classify deslot, crash, car stopped | **Power board, or the control module's `power_manager`** → events `DESLOT{lane, t}`, `LANE_CURRENT_LOST{lane, t}`, telemetry stream (decimated, about 20–50 Hz per lane) |
| Decide: FCY, stop-and-go, penalties, "crash on lane 3 → yellow" | **Web app** rules, using power events + crossings |
| Act: cut a lane, limit all lanes to X % | **Control module → power board**, immediately or **at sysUs T**, through the same `sequence_manager` timeline. "Lights out" and "power on" share one T. |

- **No SYNC wire to the power board.** The control module fires power actions
  from its own sysUs alarm, so the UART latency (~100 µs at 115200) is the
  jitter. That is plenty for power switching.
- **Galvanic isolation is mandatory** on the power board
  (HARDWARE_REFERENCE §5).

### 12.2 Reflexes: the only "decisions" on the device

Some reactions cannot wait for a host round trip or survive a dead link. They
are **host-armed rules** that the control module executes:

| Reflex | Trigger | Action | Armed by |
|---|---|---|---|
| Hardware fault | EXT_PWR_FAULT (EXTI prio 0) | All lanes off, event | Always on |
| Red phase interlock | Start sequence running | Lane power off until Tout: a physical false-start prevention | Race config |
| Link loss | No PING for N s while racing | Configured safe state: hold, limit X %, or all off | Race config |
| Instant yellow | `DESLOT` event while "auto-FCY" is armed | Limit all lanes to X % at once. The host then confirms or lifts. | Race config |
| Abort | BTN2 long press | Abort the sequence, blink all lights, (later) all power off | Always on |

**A reflex is data, not code:** `{trigger, filter, action, params}`. It is
armed and disarmed by host messages and reported as events. New rules are new
trigger or action types, not new coordinators.

### 12.3 Display on UART4 (Arduino with HDMI)

- **Node class `DISPLAY`.** The web app sends compact standings frames, and
  `display_manager` forwards them.
- **No laptop:** the display needs a race engine on the device (standalone
  mode, §2.2). That stays deliberately out of v1.

### 12.4 What stays fixed as it grows

All of these stay as they are:

- the time axis;
- the event log + replay;
- the node model;
- the message-range plan;
- the master / coordinator / module split;
- the transport-agnostic session.

Growth adds three things: a module, a coordinator, a node class and its
message IDs.

---

## 13. Changes to existing documents

### No field firmware update in v1 (2026-11-18)

- **Decision:** every board is flashed over SWD / Tag-Connect; no
  bootloader anywhere. The code is archived at git tag
  `archive/bootloader-update`.
- **Host protocol 0.3 → 0.4:** UPDATE_BEGIN/CHUNK/END/ABORT and BOOT_APP
  removed, 0xA0–0xAF reserved (the generator refuses ids there), `[usb]`
  `pid_boot`/`product_boot` removed; `CONTROL_BOOT`, `LANE_BOOT`,
  `REBOOT_BOOTLOADER` and the update status codes stay as reserved values.
- **Control firmware:** `ctrl_boot`, `boot/`, the `_boot` presets, the
  bootloader app layout, `image_header`, `update_writer`,
  `tools/image_stamp.py`, `test/host_update`, `boot_shared.h` removed;
  REBOOT{BOOTLOADER} → ERR_UNSUPPORTED; `flash_drv` writes the settings
  pages only; NODE_LIST CTRL `imageCrc` is 0. Event log 1024 → 1600 events.
- **Lane firmware:** event FIFO 32 → 2480. **`lane_protocol.h`** (no wire
  change, no version bump): STATUS `fifoCount` and the EVENT_FIFO header's
  `remaining` saturate at `LANE_FIFO_COUNT_MAX` (255); the controller then
  takes the first record it reads as the seq start.
- **BLE firmware:** `stmCtrlResetIntoBootloader()` and task B5 removed;
  BOOT_REQ stays driven high, NRST stays released.
- **Web app:** `src/update/`, the simulator's bootloader, the CLI `update`
  command and suite removed; race fixtures re-recorded (protocol "0.4").
- **Docs:** this file §0, §7.5, §9, §10, §14; CONTROL_FIRMWARE_GUIDE §2,
  §5.5, §5.7, C7; CONTROL_FIRMWARE_NOTES; control BENCH_TESTS C7;
  HARDWARE_REFERENCE PB4; BLE_FIRMWARE_GUIDE §7.3, B5; LANE_FIRMWARE_GUIDE
  §3.6; WebApp README; `tools/check_all.sh`.

### Contract 0.3 (2026-11-18)

- **Host protocol 0.2 → 0.3:** HELLO_RSP `boardType` (was reserved), USB
  `pid_boot`/`product_boot` (bootloader uses the generated constants; web
  app and CLI accept both PIDs), UPDATE_CHUNK resume rule written down,
  `faultSource` SETTINGS. The updater tells app from bootloader by HELLO.
- **`lane_protocol.h` 0.1 → 0.2:** `LANE_SLEEP_*` levels (NAP, STANDBY
  reserved), CONFIG ranges as `LANE_CFG_*` (lane and controller check the
  same; the sensor-cycle floor stays lane-side), documented: CLEARED timing,
  holdoff start, duplicate records after a cut read (controller drops known
  seq), re-push of the sampling mode on BUS_IDLE_SAFE, bare reads, ENTDAA
  gated by CHAIN_IN, RELEASE_CHAIN.
- **`ble_link_protocol.h`** (comment only): `NRF_HELLO.credits` = credits
  granted now.

### Applied 2026-11-17, with the control and BLE firmware guides

- **§9 refined by `BLE_FIRMWARE_GUIDE.md` §12:** no PWM on the nRF52805 (LED
  is GPIO, "steady, dim" became "steady on"); the STM owns the pairing
  window; the CTRL message set was renamed and extended (`STM_HELLO`,
  `LINK_UP/DOWN`, `UI_STATE`, `CUE`, `CREDIT`, `STATS`); credit-based flow
  control STM → nRF; the nRF splits DATA to the MTU.
- **`HARDWARE_REFERENCE.md`:** BLE_GPIO1 is BOOT_REQ, BLE_IRQ means "central
  connected", D603 is GPIO-driven.
- **`CONTROL_FIRMWARE_NOTES.md`:** points to the guide; PB7 pull-up joins
  the next CubeMX pass.

### Applied 2026-11-16

The KiCad BLE sheet note is the one exception: it is still unchanged, because
schematic edits wait until KiCad is closed.

**`LANE_FIRMWARE_GUIDE.md`:**

- **§3.5 step 4:** "Reject an event within the minimum lap time" becomes
  **"flag it (`IN_HOLDOFF`), never drop it"**, with an optional reduced sample
  rate during the holdoff (§2.4).
- **§3.5 race states:** delete the lane-side ARMED/RACE handling and
  false-start flagging. The host judges against `LIGHTS_OUT`, and the lane only
  has sampling modes (§5).
- **§3.5 step 5:** emit on **exit** (or block timeout) with `tEntry`, dwell
  and depth. Add a merge gap.
- **§5 register map:**
  - SCHEDULE needs a queue (≥ 4 actions) and **cancel**;
  - COMMAND reserves `ENTER_BOOTLOADER`;
  - ID carries the image CRC.
- **§9:** the SYNC/epoch proposal (1 kHz, epoch by bus message) is
  **accepted** for the control side as-is.

**Other files:**

- **`CONTROL_FIRMWARE_NOTES.md`:**
  - PB4 (BLE_GPIO1) becomes BOOT_REQ, an active-low input;
  - PB5 BLE_IRQ means "central connected".
- **BLE sheet note:** the AN2606 question is answered. The ROM is not
  reachable from the nRF, so the BLE update goes through our own bootloader.

---

## 14. Build order

Each step ends in something you can observe:

1. **Protocol skeleton.** Done 2026-11-16:
   - `protocol/host_protocol.toml` + `gen_protocol.py`;
   - `frame_codec` in C, `codec.ts` / `wire.ts` / `packet.ts` in TS;
   - golden tests on both sides.

   Both sides pass and are mutation-checked: the C suites, and the TS suite
   (41 tests, `npm test` in `WebApp/`, Node 26 LTS).
2. **Control firmware skeleton.** Done 2026-11-16, built and host-tested;
   not flashed yet because no board was attached:
   - `ctrl_master`, status and behavior headers;
   - both LEDs, buttons;
   - `sys_time` + SYNC (on after LANE_POWER);
   - TinyUSB CDC "TimeTaker" with HELLO/PING/RESUME/NODE_LIST/LANE_POWER/REBOOT;
   - the event log;
   - the web app debug console (`npm run debug`) over Web Serial or the
     simulator.

   Details: `CONTROL_FIRMWARE_NOTES.md` "Firmware status".
3. **Lane bus with one lane:** `i3c_ctrl` + `lane_manager` enumeration and
   config push. This runs in parallel with lane guide steps 4–7.

   From here on, the task cards in the guides break each step into one
   session's work: CONTROL_FIRMWARE_GUIDE §9 (C0–C6) and BLE_FIRMWARE_GUIDE
   §11.3 (B1–B4; B5 dropped with the bootloader).
4. **Event pipeline end to end:**
   - lane crossing → `event_log` → web app;
   - diagnostics view with the raw stream;
   - RESUME tested by unplugging.
5. **Start sequence + race engine v1:**
   - laps and time races, false start, first crossing;
   - TV window.
6. **BLE:**
   - nRF bridge + binding;
   - `bleTransport`;
   - **pull USB mid-race, finish on BLE**, with identical results.
7. ~~Control bootloader + the web app updater.~~ **Dropped 2026-11-18:**
   no field update in v1; everything is flashed over SWD / Tag-Connect
   (§10). The built code is at git tag `archive/bootloader-update`.
8. **Later:**
   - holdoff tuning, event snapshots;
   - lane bootloader;
   - power board, display, standalone mode.

---

## 15. Open questions

- **AN2606 C5 table:** confirm USART1 = PA9/PA10. Only matters if a field
  update comes back (§10); not a blocker.
- **TinyUSB C5 support upstream**, or the size of the port.
- **nRF52805 RAM** after link framing and the button/LED logic. Can DLE/MTU
  247 and 2M PHY still fit, and is there headroom for LESC later?
- **VCNL4040 reachable sample rate** (bench). It sets the default sample
  period, the merge gap and the holdoff watch rate.
- **USB VID/PID.**
- **Git.** The TimeTaker tree is not a git repository yet. The hub-style
  version stamp (`MAJOR.MINOR.PATCH+gitrev`) and the generated-file checks
  both want it.
