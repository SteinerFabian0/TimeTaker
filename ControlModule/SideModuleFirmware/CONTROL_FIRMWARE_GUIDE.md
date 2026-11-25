# Control Module Firmware — Guide for the firmware sessions

Written 2026-11-17. This is the brief for every session that writes control
module firmware. Read it end to end before you write code. The lane module
has the same kind of brief (`LaneModule/LaneModuleFirmware/LANE_FIRMWARE_GUIDE.md`);
the two are written to fit together.

**Sources of truth, in this order:**

1. The KiCad schematic in `../SideModulePCB/`.
2. The CubeMX2 project `LapCounterControlModule.ioc2` and its notes,
   `CONTROL_FIRMWARE_NOTES.md` (clocks, pins, IRQ priorities, firmware
   status). Those notes stay the place for **CubeMX facts and the status
   log**. This guide is the place for **what to build and how**.
3. `../../SYSTEM_ARCHITECTURE.md` decides what the control module does and
   what it leaves to the lanes and the web app. Where it and this guide
   disagree, it wins. `../../HARDWARE_REFERENCE.md` has the hardware gist.
4. This guide.
5. `../../protocol/host_protocol.toml` is the authority for every host
   message. This guide proposes new messages; the TOML decides them.

**Reference firmware:** the Yuhka hub firmware (same MCU family, almost
production ready) at
`~/Desktop/ATWU_Firmware/hub_STM32C542_MainboardV3/hub_STM32C542_MainboardV3_cmake/`.
Its `fault_guard`, `sys_power`, `docs/ERROR_HANDLING.md`, `docs/RELEASING.md`
and `tools/optprog.py` are the templates for the ports listed below. Copy its
structure and lessons, not its features.

---

## 1. What the control module is

One STM32C542 at 144 MHz from a 24 MHz crystal. USB-powered, never sleeps
while lanes run. It has five jobs and nothing else:

| Job | Meaning |
|---|---|
| **Time master** | TIM12 is the system clock (`sysUs`) and drives SYNC to the lanes. Every timestamp in the system lives on this axis. |
| **Lane bus manager** | Lane rail, CHAIN enumeration over I3C, config push, SYNC alignment, event collection, lane health. |
| **Event log** | Every fact gets a sequence number (`evSeq`) and is replayable after a dropout (`RESUME`). |
| **Transport router** | One host session per transport: USB CDC now, BLE via the nRF later. Same protocol on both. |
| **Real-time executor** | Start sequence and scheduled actions at a system time. Later the power-board reflexes (SYSTEM_ARCHITECTURE §12.2). |

**It never counts laps,** computes lap times, judges false starts or ranks
anything. That is the web app's race engine (SYSTEM_ARCHITECTURE §2.2). If a
feature needs race context, it belongs in the web app, and the device only
reports the facts it needs.

---

## 2. Where it stands (2026-11-18, after C1–C6 without a board)

Task cards C1–C6 are written as far as they go without hardware: everything
builds (debug and release presets, 0 warnings, `-Werror` on `app/`) and all
host suites pass (mutation-checked). **C7 was removed the same day: no field
firmware update in v1.** Every board is flashed over SWD / Tag-Connect; the
bootloader and the update path are at git tag `archive/bootloader-update`
(§5.7).
**Nothing has been flashed:** the PCBs are still in production, so the bench
half of every card is open. `BENCH_TESTS.md` (next to this guide) has the
numbered procedures for when boards arrive; run them in order.

| Part | State |
|---|---|
| Bench bring-up (C0 first half, every card's bench half) | **Open: never flashed.** BENCH_TESTS.md |
| C1 `i3c_ctrl` + `lane_manager` (enumeration, config table + push, SYNC_ALIGN, polling, health, LEFT/rejoin, LANE_* requests, NODE_LIST lanes) | Written; pure rules in `lane_logic` (host-tested) |
| C2 EVT_CROSSING, RAW stream, per-transport stream budget | Written; `stream_budget` host-tested |
| C3 `seq_planner` (host-tested), `sequence_manager`, BTN2 long = abort | Written |
| C4 `settings_store` (two 8 KB pages, bank 2), `rng` (bootId) | Written; `settings_record`, `crc32` host-tested |
| C5 `nrf_link`, BLE session, bind gating in `host_session`, `bind_list`, bind policy, BIND_*, EVT_BIND_CHANGED / EVT_BLE_LINK, echo knob | Written; `nrf_link_proto`, `byte_ring`, `bind_list`, gating host-tested |
| C6 `fault_guard`, IWDG, `sys_power` (WFI), fault inputs, STATUS, SETTINGS_*, `tools/optprog.py`, `docs/` | Written |
| Coordinator decisions (`sequence_manager`, `host_manager` bind policy, `panel_manager`) | In the pure `ctrl_policy` (host-tested, seam review 2026-11-18); the coordinators keep only HAL calls and side effects |
| C7 bootloader + update path | **Removed 2026-11-18** (no field update in v1). App at 0x0800_0000 is the only layout; the freed RAM went to the event log (1600 events, about 8 KB RAM left free) |

**Build, flash and test** commands are in `CONTROL_FIRMWARE_NOTES.md`
("Build"). Flash with the ST-LINK V3 over J201.

---

## 3. Structure and ownership

### 3.1 The target tree

`✓` exists, `○` to write. Paths are under `LapCounterControlModule_cmake/`.

```
main.c                       ✓ mx_system_init(); ctrlInit(); while (1) ctrlRun();
app/config/
  ctrl_behavior.h            ✓ knobs only (debug vs RELEASE)
  ctrl_status.h              ✓ assert macros; the enum itself is generated
  host_protocol.h            ✓ GENERATED from protocol/host_protocol.toml, never edit
  lane_protocol.h            ○ byte-identical copy of the lane tree's (§7.2)
  ble_link_protocol.h        ○ byte-identical copy of the BLE tree's (§7.3)
  tusb_config.h              ✓
app/coordinators/
  ctrl_master.c/h            ✓ init order, super-loop, error policy (+ link-loss policy later)
  host_manager.c/h           ✓ sessions, request dispatch, event log; ○ BLE session, bind policy
  lane_manager.c/h           ✓ power + SYNC; ○ enumeration, config table, events, health
  sequence_manager.c/h       ○ start sequence, scheduled actions, abort
  panel_manager.c/h          ✓ LEDs + buttons; ○ local fallbacks (BTN2 long = abort)
app/modules/
  sys_time                   ✓ TIM12 sysUs + SYNC output; ○ sysUs alarms (only when needed, §5.3)
  lane_chain                 ✓ LANE_PWR_EN, CHAIN_OUT, bus pin parking
  i3c_ctrl                   ○ I3C1 controller: transfer queue, RSTDAA/ENTDAA, hot-join, IBI, recovery
  usb_cdc                    ✓ TinyUSB CDC "TimeTaker"
  nrf_link                   ○ USART1 + link framing (DATA/CTRL), credits, BLE_IRQ
  frame_codec                ✓ COBS + CRC16, pure (also compiled into the BLE firmware)
  event_log                  ✓ pure
  host_session               ✓ pure; ○ per-session bind gating (§5.4)
  bind_list                  ○ pure: client IDs, LRU, serialise (host-tested)
  seq_planner                ○ pure: start sequence plan (host-tested)
  settings_store             ○ flash pages with CRC: bind list, lane config table, device name
  device_id                  ✓ UID, bootId, reset cause
  rng                        ○ TRNG: bootId seed, start-hold randomness
  indicator_led, led_pattern ✓
  button_handler, irq_guard  ✓
  fault_guard, sys_power     ○ ports from the hub
test/                        ✓ run_all.sh + 4 suites; ○ lane_protocol copy check, bind_list,
                               seq_planner, nrf link framing, led_pattern port
docs/                        ○ ERROR_HANDLING.md, RELEASING.md (hub style)
```

### 3.2 The rules

- **Modules own hardware and know nothing about the race.** One owner per
  peripheral and per pin, named in the module header's opening comment
  together with what it must never do and its IRQ context.
- **Coordinators own behaviour.** They call modules. They never send frames.
  They publish facts with `hostManagerPublishEvent()`, and `host_manager`
  pumps the log to each session at that session's pace.
- **`host_manager` owns the protocol, not the behaviour.** It decodes a
  request, checks its length, and calls the public API of the coordinator
  that owns the subject: `LANE_LED` → `laneManagerSetLed()`,
  `START_SEQUENCE` → `sequenceManagerStart()`.
- **`lane_manager` is the only user of `i3c_ctrl`.** `sequence_manager`
  queues its lane writes through `laneManagerSchedule()`, which puts them at
  the front of the queue.
- **`ctrl_master` is the only caller of coordinators' `Run()`** and the single
  error policy point (`ctrlErrorHandler()`).
- **Pure logic is a separate module with a host test.** Anything with a rule
  in it (planning, parsing, list management, framing) goes in a module with
  no HAL, driven by `test/`. Drivers and timing stay bench work.

### 3.3 Interrupt contexts

The priorities are set in CubeMX (`CONTROL_FIRMWARE_NOTES.md`). What runs in
each:

| Prio | IRQ | Allowed work |
|---|---|---|
| 0 | EXTI4 EXT_PWR_FAULT, EXTI12 USB_FAULT | Set a flag (later: cut the external power enable at once). **No `sysTimeNowUs()`** (sys_time.h explains why). The main loop stamps and publishes. |
| 1 | TIM12 update | Count ticks. Nothing else. |
| 2 | I3C1 EV/ERR | Advance the current transfer, latch IBIs and hot-join requests into a small queue. |
| 3 | EXTI14 BUS_INT | Reserved. Ignore it until the rail has settled (HARDWARE_REFERENCE §2.7). |
| 4 | USB | TinyUSB queues events for `usbCdcRun()`. |
| 5 | USART1 | Move bytes between the UART FIFO and `nrf_link`'s rings. |
| 6 | EXTI5 BLE_IRQ | Latch "BLE link changed". |
| 9 | Buttons | Latch the edge; `button_handler` debounces in the loop. |

Everything else runs in the main loop. The event log, the sessions and every
coordinator are main-loop only and not reentrant.

---

## 4. Super-loop and timing

The loop today (`ctrl_master.c`) runs buttons, USB, host, lanes, panel, LEDs.
The target adds three entries and keeps the order:

```c
void ctrlRun(void)
{
    faultGuardTick();                        /* ○ feeds the IWDG once per pass */
    buttonHandlerPoll(now);
    usbCdcRun();
    nrfLinkRun();                            /* ○ decode link frames, CTRL messages, credits */
    ctrlErrorHandler(laneManagerRun());      /* bus transactions, enumeration, polling */
    ctrlErrorHandler(sequenceManagerRun());  /* ○ due actions, lights-out publication */
    ctrlErrorHandler(hostManagerRun());      /* rx → dispatch; pump events per session */
    ctrlErrorHandler(panelManagerRun());
    ctrlErrorHandler(indicatorLedRun(now));
    sysPowerIdle();                          /* ○ WFI until the next IRQ; never Stop */
}
```

- **Nothing blocks.** Every wait is a state-machine step with a timestamp.
  Target: one pass under 1 ms. `ctrlMasterGetLoopMaxMs()` already measures
  it; report it in the STATUS request once that exists.
- **Hard real time is not in the loop.** The SYNC edge is hardware (TIM12
  CH2). The start lights go out from each lane's own timer. Lane timestamps
  are taken on the lane. The loop's latency only moves *when the host hears*
  about a fact, never the fact's time.
- **Lanes first, host second** in the loop, so a crossing read in this pass
  is pumped in the same pass.
- **Watchdog** (task C6): IWDG at about 2 s, fed by `faultGuardTick()`. Any
  wait longer than about 10 ms must be a state, not a loop.

---

## 5. Features, in build order

Each subsection is one build-order step of SYSTEM_ARCHITECTURE §14 and ends
in something you can see on the bench.

### 5.1 Lane bus (build step 3)

**Goal:** power the lanes, enumerate them through the CHAIN, push their
config and SYNC alignment, and keep a live table of which lane is where.

#### `i3c_ctrl` (module)

- Owns I3C1 (PA8/PA9 while released) and its EV/ERR IRQs.
- **An asynchronous transfer queue.** Callers queue `{addr, write bytes,
  read length, done callback}`; the ISR runs one at a time; the loop collects
  results. Depth about 16. `laneManagerSchedule()` uses a front-of-queue
  insert.
- **Broadcast CCCs:** RSTDAA, ENTDAA (with the address to give), DISEC/ENEC
  for hot-join and IBI.
- **Hot-join and IBI** requests are latched in the ISR into a 4-entry queue
  that `lane_manager` drains.
- **Error recovery:** on a bus error, abort the transfer, clear the error
  flags, return `CTRL_ERR_I3C` to the caller, and leave the bus idle. Never
  stall the queue. If SDA is stuck low, release the pins to GPIO and clock
  SCL up to 9 times (the hub's I²C recovery, for I3C pins).
- **Speed:** 1 MHz push-pull to start (CubeMX). Raise it only after a bench
  test with six lanes on the ribbon.
- Check the C5 HAL's I3C controller API and the CubeMX2 I3C controller
  examples for the NUCLEO-C562RE before writing it. **Read §10 Q1 first**:
  the number of target slots in the controller's device table may limit
  IBIs.

#### Enumeration (`lane_manager`)

Lane side: LANE_FIRMWARE_GUIDE §3.2. Control side:

```
POWER_OFF ─LANE_POWER on─► SETTLING (30 ms, done) ─► ENUMERATING ─► RUNNING
    ▲                                                     │            │
    └──────────────── LANE_POWER off (signals first) ─────┴────────────┘
```

1. **Settled:** SYNC on (done), then `laneChainReleaseBusPins()`, enable the
   I3C controller, accept hot-join (ENEC), and send **RSTDAA** so a lane left
   with an address from before forgets it.
2. **Raise CHAIN_OUT.** Lane 1 sees CHAIN_IN high and hot-joins.
3. **On each hot-join:** run ENTDAA and give the address for the next index
   n (proposal: `LANE_ADDR(n) = 0x10 + n`, so 0x11…0x16; any scheme that
   avoids 0x02 and the 0x7E family works; `lane_protocol.h` decides). Then:
   - read `ID` (board type, image kind, versions, CRC, UID);
   - write `LANE_ID = n`;
   - publish `EVT_LANE_STATE{n, JOINED}`;
   - start the config push (below).
4. **The lane raises its CHAIN_OUT** once addressed. Lane n+1 hot-joins.
   Repeat.
5. **End of chain:** no hot-join within the timeout after the last address
   (start with 500 ms for lane 1, which still boots, and 200 ms after that;
   bench values). The count is the number of lanes. Fewer than six is fine:
   a short track or a broken ribbon both just show fewer lanes.
6. **RUNNING:** poll (below), and keep accepting hot-joins.

**A hot-join while RUNNING** means a lane reset or was re-plugged. Its whole
downstream lost their addresses too (their CHAIN_IN fell). The rule that
makes this simple: **joins always arrive in chain order.** So:

1. probe every known address (one STATUS read each);
2. mark every lane that does not answer as `LEFT` (publish
   `EVT_LANE_STATE{n, LEFT}`);
3. give the joiner index = (number of lanes answering from 1 upward) + 1;
4. the rest follow, one hot-join each, exactly as at power-up.

**`LANE_SCAN`** (host request) re-runs the whole chain: CHAIN_OUT low,
RSTDAA, all lanes `LEFT`, then step 2.

#### Config table and push

- **The control module keeps the lane config table**, one entry per index,
  with compiled defaults until the host writes it (`LANE_CONFIG_SET`). The
  lane keeps nothing across a reset (LANE_FIRMWARE_GUIDE §5).
- **On every JOINED,** push from the table: `CONFIG`, then (only if a
  reference is saved for the lane index, see "Calibration") `COMMAND
  SET_REFERENCE{value}`, then `SYNC_ALIGN`, then the sampling mode with
  `COMMAND`. The same order on every re-push (a lane that lost its config);
  a `BUS_IDLE_SAFE` re-push sends `SET_REFERENCE` (if saved) before the
  sampling `COMMAND`. The order is `laneLogicPushNext` (host-tested). Read `STATUS` back. When it shows
  the config applied and SYNC aligned, publish `EVT_LANE_STATE{n,
  CONFIGURED}`. **A lane that resets mid-race comes back configured and
  sampling without the host doing anything.** The host only sees LEFT,
  JOINED, CONFIGURED in its event stream.
- `LANE_CONFIG_SET` writes the table and pushes to that lane at once if it
  is present. Persisting the table is `settings_store`'s job (§5.5); until
  then it lives in RAM.

#### SYNC alignment

The epoch scheme is decided (LANE_FIRMWARE_GUIDE §9, SYSTEM_ARCHITECTURE §3):
the control module writes **(tick of its last SYNC edge, µs since that
edge)** to `SYNC_ALIGN`, and the lane resolves which of its own edges that
was.

- **Measure just before the transfer starts,** with the I3C queue empty:
  read `sysTimeTicks()` and `TIM12->CNT` together (the same pending-UIF care
  as `sysTimeNowUs()`), fill the payload, start the transfer. Do not queue a
  pre-filled SYNC_ALIGN behind other transfers.
- **Latency budget:** measurement to the lane's receipt must stay under
  P/2 = 500 µs. At 1 MHz a SYNC_ALIGN write is about 15 bytes, about 150 µs.
- **Repeat it** once a second per lane as a consistency check. The lane
  counts any shift it sees; `lane_manager` reads that count in the health
  poll and publishes `SYNC_LOST`/`SYNC_OK` changes.
- **The tick index is `sysTimeTicks()`** because the SYNC period equals the
  TIM12 period (1000 µs). If the SYNC period ever changes, this coupling
  changes with it.

#### Event collection: polling first, IBI second

**Poll first.** Every 10 ms per RUNNING lane, read `EVENT_FIFO`. Proposal
for `lane_protocol.h`: one private read drains up to 4 records of 16 B, and
the lane ends the read early (I3C T-bit) when it has fewer. Cost: six lanes
× about 100 µs per 10 ms, about 6 % of the bus at 1 MHz.

Polling is the right first step, not a fallback:

- it works before IBI is understood on this chip (§10 Q1);
- it does not depend on the device-table limit;
- **latency does not touch accuracy:** the timestamp was taken on the lane,
  and 10 ms of collection delay is invisible next to the < 50 ms
  car-to-screen budget.

**Add IBI after** as an optimisation: an IBI from lane n triggers an
immediate `EVENT_FIFO` read, and the poll rate drops to 50 ms as a safety
net. LANE_FIRMWARE_GUIDE §3.6 already says: never depend on IBI alone.

**Each record becomes one `EVT_CROSSING`:**

| Lane record (16 B) | EVT_CROSSING field |
|---|---|
| — | `lane` = index |
| `type`, `flags` | copied |
| `tEntryUs` | copied (already sysUs, mapped on the lane) |
| `seq` | `laneSeq` |
| `dwell`, `depth` | `dwell10us`, `depth` |

- **Per-lane sequence gaps:** count them and log a `DBG_LOG` line. Publish
  nothing extra. The host sees the gap in `laneSeq` itself.
- **Duplicates and where checking starts:** a record whose seq was already
  taken (a cut read re-sends it) is dropped. At CONFIGURED the expected seq
  is STATUS `nextEventSeq − fifoCount`, unless `fifoCount` saturated or the
  lane ever dropped events (each drop spends a seq, so the queue holds a
  gap): then the first record read sets it (`laneLogicSeqStart`).
- **Order:** publish records in the order read. Across lanes the host orders
  by `tEntryUs`, never by `evSeq`.

#### Health

- Read `STATUS` of each lane once a second. Publish changes only:
  `FAULT` (with the lane's last error mapped to a status), `SYNC_LOST`,
  `SYNC_OK`. FAULT means STATUS `lastError != 0` or state `PARKED`.
- **Fault cleared:** when a lane that reported FAULT stops doing so
  (`lastError` back to 0 and not PARKED), `EVT_LANE_STATE{n, CONFIGURED}`
  with status 0 goes out once, so the host sees it recover. Not for a lane
  whose config push gave up (that FAULT stays until it rejoins).
- **Three failed transfers in a row** to a lane → `LEFT`. Its downstream
  will usually fail too. Do not re-address anyone: wait for the hot-joins.
- **Lane power off** publishes `LEFT` for every lane.

#### Host messages for this step

Add to `host_protocol.toml` when you need them (IDs are proposals inside
SYSTEM_ARCHITECTURE §7.5's ranges):

| ID | Name | Purpose |
|---|---|---|
| 0x20 | `LANE_SCAN` | Re-run the chain |
| 0x22 / 0x23 | `LANE_CONFIG_GET` / `_SET` | Read / write one lane's table entry |
| 0x24 | `LANE_SAMPLING` | Mode OFF / ON / RAW for a lane mask |
| 0x25 | `LANE_LED` | Off / on / level / blink for a lane mask |
| 0x26 | `LANE_STATUS` | One lane's STATUS, decoded |
| 0x27 | `LANE_RAW_STREAM` | Start/stop `STR_RAW_SAMPLES` for one lane, with decimation |

#### Calibration (`LANE_CALIBRATE`, protocol 0.6, lane_protocol 0.4)

The lane's *reference* is its bare-foil level (lane_protocol.h
"Reference"). The control module saves one per lane index in the settings
record (version 2; a version-1 record loads with all references 0) and seeds
it at every configure, so a car parked on the line at power-up cannot spoil
the baseline. `LANE_STATUS.savedReference` reports the kept value (0 =
none), so the host can tell `reference == savedReference` (running on the
saved calibration) from a reference the lane learned at power-up.

- `LANE_CALIBRATE{laneMask, action}`; the response only acknowledges, each
  lane's outcome follows as `EVT_LANE_CALIBRATED{lane, result, reference,
  noise}`. Rejections, in order: `ERR_SEQ_RUNNING` while a start sequence
  runs (host_manager); mask 0 / beyond lane 6 / unknown action →
  `ERR_INVALID_ARG`; a lane of the mask still calibrating → `ERR_BUSY`;
  LEARN with no lane of the mask CONFIGURED → `ERR_LANE_ABSENT`
  (`laneLogicCalAccept`). Masked lanes that are not CONFIGURED are skipped
  without an event.
- **LEARN**, per lane, as a state machine in `laneManagerRun` (never
  blocks): `COMMAND CALIBRATE`, then `COMMAND SAMPLING ON` if the lane's
  table mode is OFF (the lane learns and shows CALIBRATED only while
  sampling; OFF is restored when learning ends — this order also means a
  set flag after a clear one can only be the new baseline). `STATUS` every
  10 ms until `CALIBRATED` was seen clear and is set again (learning is 64
  samples ≈ 64 ms at 1 kHz; 1000 ms at most, else `FAILED`). Then `SIGNAL`:
  `reference` (0 → `FAILED`) and `noise`. A lane that leaves meanwhile:
  `FAILED`.
- **Save:** once no lane is learning any more, every finished lane's
  reference goes to `settings_store` in one change, so a multi-lane
  calibration is one flash write. The write's outcome (save ticket,
  `settingsStoreSaveState`) gives `SAVED`, or `NOT_SAVED` when it failed (the
  lane keeps the learned value until power-off; the store retries the write
  later on its own). The usual write rules apply (no write while a start
  sequence runs, at most one every 10 s), so the event can come up to 10 s
  late.
- **FORGET:** references 0 for every lane of the mask (present or not: the
  reference belongs to the index), `SET_REFERENCE{0}` to those on the bus
  (they learn as at boot), then the write: `FORGOTTEN` per lane, or
  `NOT_SAVED`.
- The calibration's `SIGNAL` read resets the lane's `rawMin`/`rawMax`, as
  every `SIGNAL` read does.
- `LANE_STATUS` carries `reference`, `rawMin`, `rawMax` from the `SIGNAL`
  read.

`NODE_LIST` gains one `nodeInfo` per lane: board type, versions, image CRC
from the lane's `ID`, state from the table.

**Done when:** with 1–6 lanes on the ribbon, the debug console shows each
lane JOINED → CONFIGURED in order, NODE_LIST lists them with versions, and
unplugging lane 3 shows lanes 3–6 LEFT and then rejoining in order.

### 5.2 Event pipeline end to end (build step 4)

**Goal:** a car crossing a lane shows up in the web app, and nothing is lost
over a dropout.

- Everything in §5.1's event collection, plus:
  - **`LANE_RAW_STREAM`:** one lane at a time. `lane_manager` reads the
    lane's raw records and hands them to `host_manager` as
    `STR_RAW_SAMPLES`. Streams are best effort: the session sends a chunk
    only when the transport has room beyond the response reserve, and
    drops it otherwise. Never log a stream.
  - **Stream budget per transport:** USB takes all; BLE gets a cap (a few
    KB/s; set it once `nrf_link` reports the link parameters).
- **The event log slot is 20 B of body** (`EVENT_LOG_MAX_BODY`, sized by
  `EVT_CROSSING`). Every new event must fit: **at most 24 B of payload
  including `evSeq`.** If an event needs more, split it or grow the slot
  deliberately (1600 slots × each extra byte = 1.6 KB of RAM, and the RAM
  has only about 8 KB left: CONTROL_FIRMWARE_NOTES "RAM map").
- **RESUME by unplugging** is the required test: crossings during a
  disconnect arrive after reconnect, in order, exactly once
  (SYSTEM_ARCHITECTURE §7.4).

**Done when:** a hand through the beam on any lane appears as a crossing in
the debug console with a plausible `tEntryUs`, and pulling USB for 30 s
during a burst of crossings loses none.

### 5.3 Start sequence (build step 5)

**Goal:** all lights breathe in unison, one stays lit at the start of the
chain (lane 1), the others light one by one toward the end of the chain, and all go out together at a
random time that nobody knows in advance.

The design is SYSTEM_ARCHITECTURE §6.4. Split it in two:

**`seq_planner` (pure, host-tested):** `plan(params, presentMask, nowUs,
randomU32) → {T0, Tlit, Tout, per-lane action list}`.

- P = `pulseRiseMs` (0 = 500), k = `attentionPulses`, n = min(lights,
  present); lit lanes = the first n present lanes in index order; first lane
  = the lowest of them (lane 1 = start of the daisy chain).
- `T0 = now + lead`, lead ≥ 150 ms (enough to write every lane twice at
  1 MHz, with retries).
- T0: every lit lane `LED_PULSE(P)` (lane_protocol 0.3: level(t) =
  (1 − cos(π·t/P))/2 from atUs, same phase on all lanes). Peaks at
  `T0 + (2i+1)·P`. At the last peak `T0 + (2k−1)·P` the first lane gets
  `LED_ON`; every other lane `LED_OFF` at the trough `T0 + 2k·P` (a following ON
  at a peak or OFF at a trough is seamless). Then the next lanes upward get `LED_ON`
  at `T0 + 2k·P + j·step`. k = 0: no pulse, first lane ON at T0, the others every
  step.
- `Tlit = T0 + 2k·P + (n−1)·step`; hold `h` drawn uniformly from
  `[holdMin, holdMax]` with the random word; `Tout = Tlit + h`; `LED_OFF` at
  Tout on every lit lane. `armSampling` adds `SAMPLING_ON` at T0 on every
  present lane.
- At most 5 actions per lane (pulse, off/on, on, off, sampling), inside the
  lane SCHEDULE queue of 8.
- Rejects: lights 0 or > 6, step 0, holdMin > holdMax, a sum that overflows,
  and `ERR_TOO_LATE` for a requested start time already inside the lead.

**`sequence_manager` (coordinator):**

1. `START_SEQUENCE` → `ERR_SEQ_RUNNING` if one runs. Otherwise plan it,
   draw the random word from `rng`, and queue to each present lane: cancel
   any pending actions, the lane's action list from the planner (the lane's
   SCHEDULE queue, 8 deep, LANE_FIRMWARE_GUIDE §3.7). Use
   `laneManagerSchedule()`.
2. **Read the writes back:** STATUS `schedulePending` per lane (the lane
   patches it live). A short count means the lane's main loop has not taken
   the writes off its mailbox yet: re-read every 2 ms, up to 10 times
   (`laneLogicSchedVerify`). If a lane did not confirm by T0 − 20 ms, abort
   with LANE_FAILED rather than start with a dark lane (a lane refusing a
   write at START time answers `ERR_LANE_NACK`).
3. Publish `EVT_SEQ_STARTED{T0, lights, stepMs}` at once. **Never put Tout
   in it.** The host must not know the hold before it ends.
4. When `sysTimeNowUs() ≥ Tout` in the loop, publish
   `EVT_LIGHTS_OUT{Tout}`. A loop pass late is fine: the event carries
   Tout, and the lanes already went dark on their own timers.
5. **Abort** (`ABORT_SEQUENCE`, or BTN2 long press from `panel_manager`):
   cancel all lanes' pending actions, blink all lights 3 times, publish
   `EVT_SEQ_ABORTED{atUs, reason}`.

- **No sysUs alarm is needed for v1:** nothing on the control module itself
  has to happen at an exact µs. Add alarms to `sys_time` (TIM12 CH1
  compare, armed in the tick ISR for the last ms) when the power board
  arrives (SYSTEM_ARCHITECTURE §12.1).
- **`armSampling`:** if set, also queue "sampling ON at T0" so a lane that
  was off is watching before the lights go out. At T0 the lanes' mode in the
  config table becomes ON as well (`laneManagerArmSampling`), so a re-join
  or a BUS_IDLE_SAFE re-push mid-race keeps the lane sampling.
- **False starts, reaction times, the first crossing** are decided in the web
  app against Tout. Nothing here.

Messages to add: `START_SEQUENCE` (0x40), `ABORT_SEQUENCE` (0x41),
`EVT_SEQ_STARTED` (0xC4), `EVT_LIGHTS_OUT` (0xC5), `EVT_SEQ_ABORTED` (0xC6).

**Done when:** a scope on two lane LEDs shows them going dark within a few
µs of each other, across ten sequences with different holds, and a BTN2
long press mid-sequence aborts it.

### 5.4 BLE transport (build step 6)

**Goal:** the web app connects over Web Bluetooth exactly as over USB, and a
race survives pulling the USB cable.

The nRF side is `../../ControlModule/BleFirmware/BLE_FIRMWARE_GUIDE.md`.
The link protocol between the two is defined there (§5 of that guide) and
copied here as `ble_link_protocol.h`.

#### `nrf_link` (module)

- Owns USART1 (PB6/PB7, 460800 8N1) and BLE_IRQ (PB5).
- **RX:** USART1 IRQ with the RX FIFO and the idle-line event into a 1 KB
  ring (DMA only if the loop shows load: 460800 baud is one byte every
  22 µs, a FIFO IRQ every ~170 µs). `nrfLinkRun()` feeds the ring through a
  `frame_codec` decoder for **link frames** (channel byte + body + CRC).
- **DATA frames:** their payload bytes go into a second ring, the BLE
  session's byte stream. The BLE `hostTransport_t.read()` drains it, and the
  session's own decoder finds the host frames in it. Host frames may span
  DATA frames, and one DATA frame may hold the end of one host frame and the
  start of the next. This is COBS inside COBS and that is fine.
- **TX:** `write()` takes whole host wire frames into a 1 KB staging ring
  (all or nothing, like `usbCdcWrite`). `writeSpace()` is that ring's free
  space. `nrfLinkRun()` cuts the ring into DATA link frames of ≤ 244 B
  payload and sends one **only when it holds a credit** from the nRF
  (BLE guide §5.4). This is the backpressure the session's pump already
  understands.
- **Credit resync:** a DATA frame corrupted on the wire is dropped by the
  nRF and its credit would be lost. `STATS` (1/s while connected) carries
  `slotsFree`; when it arrives, the link is up and no DATA frame went out in
  the last 20 ms, the credit count is set to `slotsFree`, up or down
  (`nrfLinkCreditsResync`, counted in `creditResyncs`). A recent DATA frame
  blocks it, a disconnected link ignores it.
- **Bridge liveness:** the nRF sends `CTRL STATS` once a second, connected
  or not. `nrf_link` keeps the time of the last valid CTRL frame and the
  latest STATS. STATUS `bleBridge` (and the BLE node of NODE_LIST:
  ABSENT / OK / FAULT) is `ABSENT` before `NRF_HELLO` or after 3000 ms
  without a CTRL frame, `VERSION_MISMATCH` (node FAULT) on another link
  MAJOR, else `OK` (`nrfLinkProtoBridge`, `ctrlPolicyBleNodeState`).
- **CTRL frames** go to small handlers: `NRF_HELLO` (versions, address) →
  the BLE node in NODE_LIST; `LINK_UP` / `LINK_DOWN` → session reset and
  stats; `BUTTON` → bind policy; `CREDIT` → TX.
- **Link version check:** if `NRF_HELLO` reports another link MAJOR, refuse
  DATA (drop it, count it) and keep CTRL working (SYSTEM_ARCHITECTURE §10.3).
  NODE_LIST then shows the BLE node as `FAULT`.
- **BLE_IRQ** high = a central is connected. It is polled every loop pass
  (EXTI5 is rising-only until the next CubeMX pass); its fall resets the BLE
  session, like `usbCdcTakeLinkReset()` does for USB. The pin is
  authoritative; `LINK_DOWN` is the backup. **Every** report resets the
  session (idempotent); only the first after an UP publishes
  `EVT_BLE_LINK{DOWN}` (`ctrlPolicyBleDown`), so a lost `LINK_UP` frame can
  never leave a session open for the next central.
- At boot, send `STM_HELLO` so an nRF that booted earlier re-announces
  itself.

#### The BLE session (`host_manager`)

- A second `hostSession_t` with transport `BLE`. `otherSessions()` now
  returns 1 when the other one is open.
- **Bind state becomes per session and mutable.** `host_session` today
  reports the configured `bindState` in HELLO but gates nothing. Add:
  - a call to set the bind state of an open session;
  - a gate: while `UNBOUND`, only HELLO and PING are served; everything
    else gets `ERR_NOT_BOUND`;
  - host-test cases for both (one broken rule per case).
- **The bind policy** (SYSTEM_ARCHITECTURE §9.5) lives in `host_manager`.
  The list itself is `bind_list` (pure, host-tested: lookup, add with LRU
  eviction at 8 entries, remove, clear, serialise for `settings_store`).

| Event | Action |
|---|---|
| BLE HELLO, clientId on the list | `BOUND`, touch its LRU stamp, send nRF `UI_STATE{bound}` |
| BLE HELLO, not on the list, window open | add it, `BOUND`, send `CUE{BIND_ACCEPTED}` |
| BLE HELLO, not on the list, window closed | `UNBOUND`, start a 60 s timer, send `UI_STATE{unbound}` |
| Window opens while an UNBOUND session is open | bind it live (same as above) |
| 60 s UNBOUND | send nRF `KICK` |
| nRF `BUTTON{LONG}` (2 s) | open the window for 60 s, send `UI_STATE{pairing}` |
| nRF `BUTTON{VERY_LONG}` (10 s) | clear the list, close the window, send `CUE{BINDS_CLEARED}`; an open BLE session drops to `UNBOUND` |
| `BIND_ALLOW{windowS}` from a TRUSTED or BOUND session | open (or with 0, close) the window |
| **USB HELLO from a web app** | **add its clientId to the list.** Plugging the laptop in once binds that browser for BLE later. Physical access is the trust anchor (SYSTEM_ARCHITECTURE §9.5). |

- **The STM owns the pairing window and its timer.** The nRF only renders
  what `UI_STATE` says (BLE guide §7.2). This keeps the nRF stateless, and
  there is one owner of every bind decision.
- Every BLE button press is also published as `EVT_BUTTON{BLE_BIND, press}`
  for the app's information.
- Messages to add: `BIND_LIST` (0x05), `BIND_REMOVE` (0x06),
  `EVT_BIND_CHANGED` (0xC7). `BIND_ALLOW` (0x08) exists.

**Done when:** the web app connects over BLE, binds with a 2 s button hold,
and pulling USB mid-race and continuing on BLE gives identical standings
(the required test, SYSTEM_ARCHITECTURE §14 step 6).

### 5.5 Settings (`settings_store`)

Needed by §5.4 (bind list) and §5.1 (to keep the lane config table across
power cycles).

- **Two flash pages at the end of the flash**, used alternately: write the
  new record into the erased page, then erase the old one. A record is
  `{magic, version, length, sequence, payload, CRC32}`. On boot take the
  valid record with the highest sequence; none valid → compiled defaults and
  `ERR_SETTINGS_CORRUPT` once in the log.
- **The C5 erase page is 8 KB** (§10 Q4): the settings are the last two
  pages, 0x0803_C000 and 0x0803_E000 (bank 2). `flash_drv` erases and
  programs these two pages and nothing else.
- **Never write flash during a race.** Writes are deferred: `settings_store`
  marks dirty and writes when no start sequence runs and at most once every
  10 s. A page erase stalls flash reads on the same bank for milliseconds;
  check on the bench that SYNC (hardware) and USB survive it.
- Contents: bind list (8 × {clientId 16 B, name 24 B, LRU u32}), lane config
  table (6 entries), device name, BLE enabled flag, and (record version 2)
  the saved reference of each lane index. Fields are only ever appended: an
  older record loads with the defaults for the newer fields
  (`settingsRecordLoad`); a newer version than the firmware knows is
  treated as corrupt.
- STATUS `settingsDefaults` = 1 when stored data was found at boot but none
  was valid (defaults loaded; the same case as `ERR_SETTINGS_CORRUPT`). A
  new board (both pages erased) is 0.

#### STATUS fields (protocol 0.6)

| Field | Source |
|---|---|
| `bleBridge` | nRF liveness, see §5.4 "Bridge liveness" |
| `bleRssi` | rssi of the latest STATS while `bleBridge` is OK, else 127 |
| `bleCrcErrors`, `bleUartErrors`, `bleDataDropped` | the nRF's counters from its latest STATS (0 before the first) |
| `nrfLinkCrcErrors` | `nrfLinkStats.badFrames`: link frames from the nRF that failed CRC/COBS/length on the STM (saturates) |
| `settingsDefaults` | `settingsStoreLostAtBoot()` |

### 5.6 Robustness (`fault_guard`, `sys_power`, faults)

Port from the hub, in this order:

1. **`fault_guard`:** `.noinit` breadcrumb (the `.noinit` section exists),
   HardFault and unexpected-IRQ handlers that write it and reset,
   boot-loop guard, and the IWDG at about 2 s fed once per loop. The
   reset cause goes into `EVT_BOOT` (`RESET_FAULT` already exists in the
   TOML).
2. **`sys_power`:** only `sysPowerIdle()` (WFI). The control module never
   uses Stop: TIM12 stops in Stop1 and with it the system clock.
3. **Fault inputs:**
   - `USB_FAULT` (PB12, EXTI prio 0): latch; the loop publishes a
     `DBG_LOG` line and counts it. The port protector has already cut VBUS
     to the board; if the board still runs, the fault cleared.
   - `EXT_PWR_FAULT` (PA4): ignore until the power board exists
     (SYSTEM_ARCHITECTURE §12.2 makes it the first reflex).
4. **Option bytes:** a script like the hub's `tools/optprog.py`, written for
   this board. `BOOT_SEL` so the BOOT0 pin is honoured (this board has a
   real pull-down, R201: **do not copy the C542 hub's BOOT_SEL setting**),
   IWDG frozen in Standby. (No bootloader pages to write-protect in v1.)
5. **`docs/ERROR_HANDLING.md`** and **`docs/RELEASING.md`** in the hub's
   style, including the CubeMX regeneration steps (§6).

### 5.7 Bootloader and updates: removed

**No field firmware update in v1** (decided 2026-11-18). The control STM32,
the lane STM32s and the nRF52805 are all flashed over SWD / Tag-Connect. The
bootloader (`ctrl_boot`), the image header, the update writer, UPDATE_* and
BOOT_APP, the second USB PID and the web app updater were built and
host-tested, then removed; they are at git tag `archive/bootloader-update`.
What stays: the app at 0x0800_0000 (the only layout), the settings pages at
the end of flash, `REBOOT{BOOTLOADER}` answered with `ERR_UNSUPPORTED`, and
the reserved protocol values (ids 0xA0–0xAF, `CONTROL_BOOT`, `LANE_BOOT`).

---

## 6. Conventions

They are already in the code. Match them; do not invent new ones.

- **Names:** camelCase functions with the module prefix
  (`laneManagerSetPower`), `_e` enums, `_t` structs, `UPPER_CASE` defines
  with the module prefix.
- **Header comment:** what the module is, what it owns, what it must never
  do, its context (loop or ISR priority). Explanatory, never decorative.
  `host_session.h` and `sys_time.h` are good models.
- **Status codes** are `ctrlStatus_e`, generated from the TOML. Add a code
  in the TOML, in its group of ten, never in C.
- **Error macros:** `CTRL_ASSERT_RETURN` at the failing HAL call,
  `CTRL_RETURN_IF_ERR` in between, `ctrlErrorHandler()` in `ctrl_master`
  only. `HAL_OK` is `0xEAEAEAEA` on the C5: always compare with `== HAL_OK`
  (`ctrlStatusFromHal()`).
- **Knobs** in `ctrl_behavior.h` (debug vs RELEASE switches only), with a
  `#warning` check in `ctrl_master.c` for each one. **Values** in the module
  that uses them.
- **Generated files** (`host_protocol.h`, `golden_cases.h`, the web app's
  `generated.ts`, `golden.json`): never edit. Change the TOML, run
  `python3 protocol/gen_protocol.py`, run both test suites.
- **CubeMX2 regeneration:** CubeMX2 overwrites the root `CMakeLists.txt`.
  Re-add the one line `include(cmake/ctrl_app.cmake)`. Everything else
  (`mx_usb_drd_fs.c` excluded, `-Werror` on `app/`, TinyUSB, version stamp)
  lives in `cmake/ctrl_app.cmake` and survives. Re-check initial GPIO levels
  after every regeneration.
- **Host tests:** `-DRELEASE -Wall -Wextra -Werror`, one broken rule per
  rejection case, mutation-check new suites (`test/README.md`).
- **Every protocol change touches three places in one change:** the TOML,
  the C handler, and the web app (at least `simDevice.ts`, so the simulator
  speaks it too). The TS tests must pass.

---

## 7. Shared contracts

### 7.1 Host protocol

`protocol/host_protocol.toml`, generated into C and TS (SYSTEM_ARCHITECTURE
§7.7). Rules for adding a message:

1. Pick an ID in the right range; never renumber.
2. Requests: every field fixed-size, only the last may be a tail. Events:
   ≤ 24 B payload (§5.2).
3. Give it an `example_request` / `example_response` (or `example`): that
   becomes a golden case on both sides.
4. Regenerate, implement the C handler, update `simDevice.ts`, run
   `test/run_all.sh` and `npm test` in `WebApp/`.

### 7.2 Lane bus: `lane_protocol.h`

LANE_FIRMWARE_GUIDE §5 sketches the register map. **It does not exist yet,
and both trees need it before task C1 / lane step 6 can start.**

- **Write it once, in one change, into both trees:**
  `LaneModule/.../app/config/lane_protocol.h` and
  `ControlModule/.../app/config/lane_protocol.h`, plus a host test in each
  tree that compares the two files byte for byte.
- **It decides:** the register pointer values, every register's packed
  struct, the address scheme (§5.1 proposal), the EVENT_FIFO read semantics
  (§5.1 proposal: up to 4 records, early end), the SCHEDULE action format
  and cancel, the COMMAND codes (with `ENTER_BOOTLOADER` reserved), the
  SYNC_ALIGN payload, the lane STATUS bit fields, and a protocol version.
- It is plain C structs, not a TOML schema: only C reads it.

### 7.3 nRF link: `ble_link_protocol.h`

Defined in `BLE_FIRMWARE_GUIDE.md` §5. Same rule: one header, byte-identical
in `ControlModule/BleFirmware/src/` and `app/config/`, with a copy-check
host test. `frame_codec.c/h` is **not** copied: the BLE build compiles this
tree's file directly.

---

## 8. CubeMX pass (done 2026-11-17)

**Done** in task C0. Items 1–6 are applied; for item 6, the CRC unit is
enabled with its default settings, because the CLI refused the reversal
settings (see the notes). **The items for the next pass (both boards) are
collected in `BRINGUP.md` at the repo root.** The list below is kept as the
record:

1. **I3C1 init "generated only"**, so SCL is never driven before
   `lane_chain` parks it (today it can sit high for a few ms at boot).
2. **PB4 (BOOT_REQ): pull-up** instead of pull-down, so an nRF in reset
   reads as "no request" (BOOT_REQ is unused since the bootloader went).
3. **PB7 (USART1 RX): pull-up.** Today it has no pull, so an nRF in reset
   (its pins float) feeds noise into USART1.
4. **RNG** enabled (generated only), for `rng`.
5. **IWDG** configured, started by `fault_guard`, not by the generated init.
6. **CRC unit** for the image check (optional; software CRC32 works).

After the pass: regenerate, re-add the include line (§6), build both
presets, run the host tests, and update `CONTROL_FIRMWARE_NOTES.md`.

---

## 9. Task cards

One card is one session's job. Each ends in something observable. The
order follows SYSTEM_ARCHITECTURE §14; cards on the same row of "Needs" can
run in parallel.

| Card | Job | Needs | Done when |
|---|---|---|---|
| **C0** | Bench bring-up of what exists (`CONTROL_FIRMWARE_NOTES.md` "On the bench"), and the CubeMX pass (§8) | a board | "TimeTaker" port appears; HELLO, NODE_LIST, buttons and SYNC on the scope check out. **CubeMX half done 2026-11-17; bench half open** (no board attached). |
| **C1** | `lane_protocol.h` in both trees (§7.2), `i3c_ctrl`, enumeration, config table + push, SYNC_ALIGN, polling, health, NODE_LIST with lanes, LANE_SCAN / CONFIG / SAMPLING / LED / STATUS messages | C0; lane steps 4–6 for the lane side | §5.1 "Done when" |
| **C2** | Event pipeline: EVT_CROSSING, raw stream, stream budget, the RESUME-by-unplug test | C1, lane step 5 | §5.2 "Done when" |
| **C3** | `seq_planner` + `sequence_manager` + BTN2 abort | C1, lane step 8 (scheduled LED) | §5.3 "Done when" |
| **C4** | `settings_store` + `rng` | C0 | settings survive a power cycle; a corrupt page falls back to defaults |
| **C5** | `nrf_link`, BLE session, bind gating, `bind_list`, bind policy | C4; BLE tasks B1–B3 | §5.4 "Done when" |
| **C6** | `fault_guard`, IWDG, `sys_power` idle, option-bytes script, ERROR_HANDLING.md, RELEASING.md | C0 | a forced HardFault reboots with `RESET_FAULT` in EVT_BOOT; a hung loop is caught by the IWDG |
| ~~C7~~ | Removed 2026-11-18: no field firmware update in v1 (§5.7) | | |

Each session updates `CONTROL_FIRMWARE_NOTES.md` "Firmware status" at the
end, and this guide's §2 table.

---

## 10. Open questions

Q1–Q3 block task C1; settle them first, from RM0522 and the C5 HAL sources.
**Answers found 2026-11-18** (from the DFP headers, the C5 HAL and the
datasheet DS15125; RM0522 is not in the tree) are under each question.

1. **I3C controller device table.** The STM32 I3C controllers on other
   families (H5, U5) have only **4 device characteristics registers**
   (`I3C_DEVRx`), which decide whether an IBI from a target is
   acknowledged. With six lanes, two would have no slot. Check RM0522 for
   the C5 count and what happens to an IBI from an unlisted target. Polling
   (§5.1) works either way; this only decides how IBI is added.
   **Answer:** 4 slots. `I3C_TypeDef.DEVRX[4]` in `stm32c542xx.h`, HAL
   `IS_I3C_DEVICE(1..4)` / `device_index 0..3`, datasheet "simultaneous
   support up to four targets, when controller". An IBI from a target not in
   DEVRx (or with IBIACK = 0) is NACKed (same IP as the H5; to verify on the
   bench). v1 configures no DEVRx: every IBI is NACKed and counted
   (`i3cCtrlStats.ibis`), all six lanes are polled. Adding IBI later: DEVRx
   for four lanes, or rotate the slots.
2. **Hot-join flow on the C5 HAL:** how the controller reports a hot-join
   request, and how ENTDAA is driven with a chosen address.
   **Answer:** CFGR.HJACK (`HAL_I3C_CTRL_EnableHotJoinAllowed`, called by the
   generated `mx_i3c1_init`) acknowledges the hot-join; with
   `HAL_I3C_CTRL_ActivateNotification(HJ)` the HJF event calls
   `HAL_I3C_NotifyCallback(.., HAL_I3C_CTRL_NOTIFICATION_HJ)`. Then
   `HAL_I3C_CTRL_DynAddrAssign_IT(ONLY_ENTDAA)`: each answering target's
   48-bit payload arrives in `HAL_I3C_CTRL_TgtReqDynAddrCallback` (ISR),
   which answers with `HAL_I3C_CTRL_SetDynAddr(addr)`;
   `HAL_I3C_CTRL_DAACpltCallback` ends it, `HAL_I3C_GetDataCounter()` = targets
   seen. Found on the way: `HAL_I3C_PRIVATE_WITH_ARB_RESTART` is defined as
   `LL_I3C_GENERATE_STOP` alone (message type RELEASE, STOP after every
   message), so `i3c_ctrl` builds private control words itself.
3. **Early read end:** confirm that the C5 I3C *target* can end a private
   read early (T-bit), and that the controller HAL reports the actual
   length. If not, EVENT_FIFO reads a fixed size and carries a count.
   **Answer:** yes on both sides, from the headers: the target preloads its
   transmit count (`I3C_TGTTDR.TGTTDCNT`, set by `HAL_I3C_TGT_Transmit_IT`'s
   size) and ends the read with T = 0 after it; the controller reports the
   early end in `SR.ABT` and the bytes that arrived in `SR.XDCNT`
   (`LL_I3C_GetXferDataCount`), which `i3c_ctrl` hands back as rxLen. The
   frame still completes normally (FCF). `lane_protocol.h` keeps the count
   in the header anyway, and `lane_logic` takes only fully received records.
4. **C5 flash erase-page size** (§5.5, §5.7). Also: does a page erase stall
   execution long enough to disturb USB?
   **Answer:** 8 KB (`FLASH_PAGE_SIZE 0x2000`, 2 banks × 16 pages), 128-bit
   programming, erase 2.0–2.1 ms (DS15125 table 39). Settings sit in bank 2
   (0x0803_C000/0x0803_E000), read-while-write against bank 1, so an erase
   should not stall the code; bench check BENCH_TESTS.md P-3.
5. **RNG on the C542:** the CubeMX clock tree lists an RNG clock, so it
   likely exists. Confirm, then `device_id` takes its bootId from it.
   **Answer:** yes (DS15125 §3.19, NIST SP800-90B TRNG; CubeMX generates
   `mx_rng_init`). `rng` seeds the bootId (`deviceIdMixEntropy`) and the hold.
6. **Lane chain current** with six lanes sampling: LDO heat and PTC margin
   (HARDWARE_REFERENCE §2.3). Measure in C1.
7. **USB VID/PID** for publication (SYSTEM_ARCHITECTURE §8).
8. **Git.** The version stamp and the generated-file checks both want the
   tree in git. Today every build is "dirty".
