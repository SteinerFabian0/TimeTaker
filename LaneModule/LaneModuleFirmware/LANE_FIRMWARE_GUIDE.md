# Lane Module Firmware — Guide for the firmware session

Written 2026-11-10 at the end of the hardware session, with the full hardware
and CubeMX context in hand. This is the brief for whoever writes the lane module
firmware. Read it end to end before writing code.

**Sources of truth, in this order:**

1. The KiCad schematic in `../LaneModulePCB/` (ERC clean, matches the PCB that
   goes to JLC).
2. The CubeMX2 project `LapCounterLaneModule.ioc2` in this folder (fully set up,
   generates and links; see §4).
3. `../../SYSTEM_ARCHITECTURE.md` decides what the lane does and what it
   leaves to the control module and the web app. It changed §3.1, §3.5–3.7
   and §5 on 2026-11-16. Where it and this guide disagree, it wins.
   `../../HARDWARE_REFERENCE.md` has the hardware gist of both boards.
4. This guide.
5. The project summary doc ("Rundenzähler & Zeitnahme – Märklin Sprint
   6-spurig", claude.ai/artifact/7516F3ijtD7JXM8eD558Xi). It predates several
   hardware decisions: it still describes an I²C bus, a different connector
   pinout and SYNC as an external timer clock. Use it for the *why*: the race
   flow, the detection idea and the timing goals. Do not use it for pins or
   buses.

**Reference firmware:** the Yuhka hub firmware (same MCU family, almost
production ready) at
`~/Desktop/ATWU_Firmware/hub_STM32C542_MainboardV3/hub_STM32C542_MainboardV3_cmake/`.
Copy its structure, conventions and hard-won C5 lessons (§8). Do not copy its
features.

---

## 1. What the lane module is

The system has six identical lane boards, one per lane of a 6-lane Märklin
Sprint track. Each board looks straight down from about 15 cm at a patch of
retroreflective foil with a Vishay VCNL4040. A passing car interrupts the
reflection.

The lane boards plug into each other edge to edge in a chain. A mainboard (also
an STM32C542) sits at one end and supplies:

- 3.3 V for every board,
- the shared I3C bus,
- the SYNC timebase signal,
- the CHAIN enumeration line,
- the shared BUS_INT line.

The mainboard talks USB to a laptop web app that runs the race.

**Goal of this firmware:** a solid, robust lane node that the mainboard fully
controls. The lane module decides nothing about the race. It:

- samples its sensor **when and how fast the mainboard says**, on the shared
  timebase;
- detects car passes and timestamps them in **mainboard time**;
- queues the events and tells the mainboard;
- drives its LED (the start light above the lane) **exactly when** the
  mainboard says: on/off, brightness, blink, and scheduled at a tick for the
  F1-style start;
- sleeps and wakes **on mainboard command**, individually or all together;
- never hangs. Every fault becomes a retry, a clean reboot or a reported error.
  An independent watchdog comes later (§7).

Everything is parameterised over the bus. No per-board flashing, no per-board
config in flash. The same image runs on all six boards.

---

## 2. Hardware facts

| Item | Value |
|---|---|
| MCU | STM32C542CCU6, UFQFPN48. The schematic symbol says STM32C531CCU6: same die and pinout, only the crypto/CAN features differ. CubeMX targets the C542. |
| Clock | No crystal (PH0/PH1 carry the sensor I²C). HSI/PSI at 144 MHz. Up to about ±1 % drift, which is why SYNC exists. |
| Supply | 3.3 V from the mainboard LDO, passed down the chain. 100 µF electrolytic (C8) at the connector. No regulator on the board. |
| Debug | Tag-Connect TC2030-NL: SWDIO PA13, SWCLK PA14, SWO PB3, NRST. SW1 is the reset button. |
| BOOT0 | PH2-BOOT0 has a 10 k pull-down (R1) and test pad TP1. **Not floating**, unlike the Yuhka C542 board (see §7, option bytes). |

### Pin map (CubeMX labels = schematic nets)

| Pin | Port | CubeMX label | Schematic net | Function |
|---|---|---|---|---|
| 5 | PH0 | SENS_SDA | SENS_SDA | I2C1_SDA (AF4), VCNL4040, 2.2 k pull-up on board |
| 6 | PH1 | SENS_SCL | SENS_SCL | I2C1_SCL (AF4), 2.2 k pull-up on board |
| 10 | PA0 | SENS_INT | SENS_INT | VCNL4040 INT, open-drain active low, 10 k pull-up (R5). EXTI0, also WKUP1. |
| 11 | PA1 | CHAIN_OUT | CHAIN_OUT | Enumeration output to the next lane (J2 pin 2) |
| 12 | PA2 | BUS_INT | BUS_INT | Shared open-drain line on all boards and the mainboard (connector pin 9). Also WKUP2 and TIM2_CH3. |
| 15 | PA5 | SYNC | SYNC | TIM2_CH1 (AF1) input capture. Also TIM2_ETR (AF14). |
| 25 | PB12 | CHAIN_IN | CHAIN_IN | Enumeration input from the upstream lane or mainboard (J3 pin 2). EXTI12. |
| 29 | PA8 | BUS_SDA | BUS_SDA | I3C1_SDA (AF2). No pull-up on the lane; the mainboard provides it. |
| 30 | PA9 | BUS_SCL | BUS_SCL | I3C1_SCL (AF2), push-pull from the mainboard |
| 40 | PB4 | CS | CS | SPI breakout chip select, GPIO. NJTRST at reset with pull-up, so CS idles high through boot. |
| 41 | PB5 | MOSI | MOSI | SPI1_MOSI (AF5), breakout pad |
| 42 | PB6 | MISO | MISO | SPI1_MISO (AF6), breakout pad |
| 43 | PB7 | SCLK | SCLK | SPI1_SCK (**AF6**, see §8 about the stale AF5) |
| 45 | PB8 | LED_PWM | STATUS_LED | TIM1_CH1 (AF1) through 82 Ω into a red 5 mm LED to GND. About 13–15 mA at 100 % duty. |
| 34/37/39 | PA13/PA14/PB3 | — | SWDIO/SWCLK/SWO | Debug |

All 20 unconnected pins are excluded in CubeMX.

### Lane connectors (identical pinout, signals pass straight through)

| Pin | Signal | Pin | Signal |
|---|---|---|---|
| 1 | DVDD_3V3 | 2 | CHAIN (IN on J3, OUT on J2) |
| 3 | BUS_SCL | 4 | GND |
| 5 | BUS_SDA | 6 | GND |
| 7 | SYNC | 8 | GND |
| 9 | BUS_INT | 10 | GND |

The connectors sit as follows:

- **J3 IN:** male right-angle header, right board edge.
- **J2 OUT:** female right-angle socket, left board edge.

The mainboard feeds the IN header of the first lane. Lane 1 is the one nearest
the mainboard.

### Sensor (VCNL4040M3OE)

- **Address:** 7-bit 0x60, on its own local I²C bus. It is the only device
  there.
- **Sensor supply:** VDD comes through an RC filter (22 Ω / 4.7 µF), local net
  VDD_SENS.
- **IR LED supply:** the IRED anode is fed **unfiltered** from 3.3 V, with its
  own 4.7 µF (C6). IR pulses reach 200 mA.
- **Operating mode:** force mode (PS_AF, PS_TRIG) is intended. The MCU decides
  the exact sampling instant.
- **Open bench measurement:** PS conversion time at the shortest integration
  time, and so the reachable sample rate. The project goal is ≥ 500 Hz per lane.
  A car spends about 15–30 ms under the sensor.

---

## 3. System behaviour the firmware implements

### 3.1 Lane states

```
           power-up / reset
                  │
                  ▼
   ┌──────── UNENUMERATED ◄──────────── CHAIN_IN falls, RSTDAA, bus timeout
   │      CHAIN_OUT low, LED off,       or mainboard "release"
   │      sensor shut down,                ▲
   │      I3C target without address       │
   │              │ CHAIN_IN high          │
   │              ▼                        │
   │      JOINING: Hot-Join request ───────┤ (retry with backoff,
   │              │ dynamic address        │  bounded)
   │              ▼ assigned (ENTDAA)      │
   │      ENUMERATED: CHAIN_OUT high ──────┘
   │      waiting for config
   │              │ mainboard pushes config + "start"
   │              ▼
   │      IDLE ◄────► SAMPLING (mode ON or RAW, with optional holdoff; §3.5)
   │        │  ▲
   │        ▼  │ wake command / BUS_INT / sensor INT
   └────► SLEEP (per-lane low power, still addressable, §3.6)
```

Mainboard commands drive every transition. The only autonomous ones are the
CHAIN-driven join and leave, and fault handling.

### 3.2 Enumeration with CHAIN + I3C hot-join

Every lane runs the same image and has the same I3C provisioned ID. Physical
position comes **only** from the chain:

1. At boot, CHAIN_OUT is driven low. CubeMX sets the initial state RESET.
2. The mainboard raises CHAIN_IN of lane 1.
3. A lane whose CHAIN_IN is high and that has no dynamic address sends an I3C
   **hot-join** (enabled in CubeMX). Only this one lane is on the bus asking, so
   identical provisioned IDs never collide in DAA.
4. The mainboard runs ENTDAA and assigns the next dynamic address. Lane index n
   gets a fixed address scheme, agreed in the shared contract (§5).
5. Once addressed, the lane sets **CHAIN_OUT high**. The next lane now sees
   CHAIN_IN high and repeats from step 3.
6. The mainboard stops when no new hot-join arrives within a timeout. The count
   it reached is the number of lanes. A gap shows where the chain broke.
7. The mainboard writes the lane index (1…6) into the lane (LANE_ID register)
   and pushes the configuration.

Rules that keep the chain consistent:

- **CHAIN_OUT = (has dynamic address) AND (CHAIN_IN high).** Re-evaluate on
  every change. EXTI12 fires on both edges for this.
- **CHAIN_IN falls:** the upstream lane reset or was unplugged. Drop CHAIN_OUT
  immediately, which cascades down. Forget the address and lane index, go to
  UNENUMERATED, and wait for CHAIN_IN to rise again.
- **RSTDAA from the mainboard:** same as above. A mainboard reset or app
  "re-scan" re-runs the whole chain.
- **Debounce CHAIN_IN** over a few ms. It is a slow logic line through pin
  headers.
- **In Standby:** CHAIN_OUT must keep its level. CubeMX enables GPIO retention
  in Standby, but PB4 and the SWD pins are excluded from retention. Without
  retention, a sleeping lane would make every downstream lane re-enumerate.

### 3.3 Timebase (SYNC)

All timestamps must live on **one system time axis owned by the mainboard**.
The lane's own HSI is only good for interpolating between SYNC edges.

**CubeMX configuration (already done):**

- TIM2 is the only 32-bit timer. It runs free at 1 MHz: 144 MHz with prescaler
  143, full 32-bit period, so it wraps about every 71 min.
- CH1 input-captures the **rising** SYNC edge on PA5 in hardware.
- Input filter FDIV1_N8: 8 samples at 144 MHz, about 56 ns. Rejects spikes on
  the long chain.
- TIM2 interrupt priority 1.

Proposed scheme, to agree with the mainboard firmware:

- **SYNC is a low-rate square wave, about 1 kHz.** The doc's 10 kHz would cost
  10 k capture IRQs per second for no gain, because the 1 µs local timer
  interpolates. If 10 kHz is kept, use the capture prescaler (/8) to divide the
  IRQ rate.
- **Each captured edge pairs** a mainboard tick index k with a local capture
  value c_k.
- **An event at local time t** with c_k ≤ t < c_(k+1) maps to mainboard time
  `T = k·P_sync + (t − c_k) · P_sync / (c_(k+1) − c_k)`. Until c_(k+1) exists,
  use the last measured period. This cancels the lane's HSI drift every period.
- **Epoch / tick 0:** the mainboard marks one SYNC edge, for example with a
  stretched or omitted pulse. The lanes detect it by also capturing the falling
  edge (polarity RISING_FALLING) and measuring the high time. Alternatively it
  writes "the next SYNC edge is tick K" well inside one 1 ms period. Pick one in
  the contract. Either way **no lane resets its counter**: the mapping does it.
- **SYNC loss** (no edge for several periods) is reported in STATUS. Events
  keep being stamped with local extrapolation and are flagged as such.

If the bench proves a simpler scheme sufficient, TIM2 can instead be **clocked
by SYNC** directly: external clock mode 1 on TI1FP1, or ETR mode 2 via PA5 AF14.
This is the doc's original idea. It changes only TIM2's clock source in CubeMX;
the pin stays the same.

### 3.4 Sampling ("the mainboard controls and times the polling")

- **Schedule:** sampling runs on a schedule set by the mainboard: period
  (in SYNC-derived µs) and **phase offset per lane**. With phases staggered,
  six 200 mA IR pulses never land on the shared 3.3 V chain at the same time.
  The lane fires the force-mode trigger from a TIM2 compare (CH2–CH4 are free;
  a compare interrupt is enough), so the sampling instant is deterministic.
- **Timestamp:** the timestamp of a sample is the trigger instant plus a fixed,
  bench-measured conversion latency, converted with §3.3.
- **Read-out:** read the PS result over I2C1 at 400 kHz, interrupt-driven
  (I2C1 EV/ERR priority 3). Do not block the main loop on I²C.
- **Raw mode:** the mainboard can switch a lane to "raw stream" for the app's
  diagnostics view: every sample, or every Nth sample, as (time, value) into
  the FIFO.
- **Single-shot sample:** the mainboard can request one sample at any time.

### 3.5 Detection

The algorithm is from the doc; this is pure logic and must be host-tested:

1. Track the baseline (bare foil) with a slow IIR filter, updated only while the
   lane is free.
2. A car is present when the signal falls below threshold LOW. Hysteresis holds
   until it rises back above threshold HIGH. Both thresholds are relative to the
   baseline.
3. Interpolate the **entry** instant (the signal falling through LOW) linearly
   between the two samples around it. This `tEntry` is the timing reference:
   the car's nose on the line.
4. **Merge gap:** if the signal rises above HIGH and falls below LOW again
   within `mergeGapUs` (about 30 ms), it is the same car (body/wing gaps). Set
   the `MERGED` flag.
5. **Emit on exit:** push one event when the car has left (above HIGH for
   longer than the merge gap). It carries `tEntry`, the dwell time (entry to
   exit), the depth (signal minimum relative to baseline) and flags. If the
   signal stays below LOW for `blockTimeoutUs` (about 500 ms), push a
   `BLOCKED` event instead: a car parked on the line, or a hand. Push
   `CLEARED` when it lifts.
6. **Holdoff: flag, never drop.** For `holdoffUs` after an event, every new
   event gets the `IN_HOLDOFF` flag. Optionally the sample period changes to
   `holdoffSamplePeriodUs` (for example 100 Hz instead of 1 kHz) to save IR
   pulses. The lane never deletes a detection. Whether a fast re-crossing is a
   lap is the web app's decision (`SYSTEM_ARCHITECTURE.md` §2.4). The default
   is a holdoff of 0, off.
7. Calibrate on command: measure the baseline on an empty track and derive the
   thresholds.

All parameters come from the mainboard:

- thresholds and hysteresis,
- merge gap, block timeout, holdoff and holdoff sample period,
- sample period and phase,
- IR current (PS_MS LED_I) and integration time.

**The lane knows no race states.** It has only sampling modes:

| Mode | Meaning |
|---|---|
| `OFF` | Sensor in shutdown |
| `ON` | Detect on the CONFIG schedule |
| `RAW` | `ON`, plus a decimated `(t, value)` diagnostics stream |

False starts, "the first crossing is not a lap", and minimum-lap validation
are all judged in the web app. It compares crossing times with the
`LIGHTS_OUT` time that the control module reports. The lane only has to be
sampling before lights-out, which the start sequence arranges.

**Event record (draft; `lane_protocol.h` is the authority), 16 B:**

| Field | Type | Meaning |
|---|---|---|
| seq | u16 | Per-lane counter, so the mainboard sees gaps |
| type | u8 | `CROSSING`, `BLOCKED`, `CLEARED`, `SENSOR_FAULT` |
| flags | u8 | `NO_LOCK`, `UNALIGNED`, `EXTRAPOLATED` (sync), `IN_HOLDOFF`, `MERGED`, `SATURATED` |
| dwell | u16 | Entry to exit, in units of 10 µs |
| depth | u16 | Signal minimum relative to baseline |
| tEntryUs | u64 | Mainboard time of the entry |

**Later, optional: event snapshots.** Keep about 32 samples before and 32
after each of the last 8 events, for the mainboard to fetch by `seq`. Detection
can then be tuned at the venue without streaming everything.

### 3.6 Events to the mainboard

- **Event FIFO:** a ring buffer of events, ≥ 32 deep. On overflow, keep the
  oldest, count the drops and report the count in STATUS.
- **Notification:** an **I3C In-Band Interrupt** with a 1-byte payload (event
  type / pending count). Enabled in CubeMX: IBI, payload 1 byte. The mainboard
  then reads the FIFO.
- **Polling:** the mainboard may also simply poll STATUS. Never depend on the
  IBI alone.

**BUS_INT** is a shared open-drain wire. CubeMX configures it as an open-drain
output, released (high) at boot. With IBI available, the recommended use is
**mainboard → all lanes**, decided in the contract:

- **Wake-all:** WKUP2 for Standby, EXTI2 for Stop.
- Optionally a **hardware start instant**: PA2 is also TIM2_CH3, so a falling
  edge can be captured in hardware on every lane at the same moment.

Lanes then only read it. Do not have lanes pull it, except as a deliberate
fallback for when IBI is not working.

### 3.7 LED (start light)

- **Hardware:** TIM1_CH1 on PB8, 1 kHz PWM: prescaler 143, period 999, so duty
  goes 0–1000 in 0.1 % steps. Starts at 0 % (dark). Compare preload is on.
- **Commands:** off / on / brightness / blink (period, duty, count) /
  "apply at tick T".
- **Scheduled actions are mandatory for the start sequence.** The mainboard
  writes "LED off at mainboard tick T" to all lanes ahead of time. Each lane
  executes it from a TIM2 compare, so all six lights go out together, to within
  one local timer tick, regardless of bus latency. This is how "lights out =
  race start" stays fair.
- **Each lane holds a small queue** of at least 4 pending actions, sorted by
  due time, because one start sequence writes "on at T1" and "off at Tout"
  together. The mainboard can **cancel** all pending actions (start aborted).
  A cancelled or past action never fires late; a late-armed one fires at once
  and reports its lateness.
- **Perceived brightness is not linear in duty:** use a gamma table, as the hub
  notes.
- **Pin ownership:** only the LED module touches TIM1 and PB8. The pin is in
  alternate-function mode, so a GPIO write does nothing.

### 3.8 Sleep and power ("sleep individual modules")

The system runs from USB power, so sleep is about **heat, IR and LED load and
quiet**, not battery life. Offer graded levels, all mainboard-commanded:

| Level | What stops | Wake | Lane addressable? |
|---|---|---|---|
| IDLE | Sensor in shutdown (PS_SD), LED off, core WFI between interrupts | any IRQ | yes, instantly |
| NAP (Stop1) | Core and most clocks; SRAM and registers kept | I3C (**verify**, §9), BUS_INT/EXTI2, SENS_INT/EXTI0, timed wake | only if I3C can wake from Stop |
| STANDBY | Everything; wake = reset | WKUP1 (SENS_INT), WKUP2 (BUS_INT) | no; it re-enumerates after wake |

- **Per-lane sleep = IDLE** by default: fully responsive, nearly all the gain.
- **NAP:** use only once it is proven that a sleeping lane still answers or can
  be woken individually.
- **Standby:** a whole-system, broadcast-style decision.
- **Watch mode:** a nice extension. The sensor runs autonomously at low duty
  cycle with its own threshold INT, so a passing car wakes a sleeping lane via
  SENS_INT. EXTI0 is already configured for the falling edge at priority 0.
- **CubeMX already generates** `mx_pwr_enter_low_power()` (Stop1, WFI) and
  `mx_pwr_exit_low_power()` (re-runs `mx_rcc_init()`). Treat them as a starting
  point only. The hub's `sys_power.c` shows everything else a correct Stop1
  round-trip needs (§8).

---

## 4. CubeMX state (done; do not redo, just know it)

Configured and saved 2026-11-10. The code generated from it compiles and links.

| Block | Setting |
|---|---|
| RCC | SYSCLK 144 MHz, clock tree validated with no errors |
| I2C1 | Sensor bus. 400 kHz, EV/ERR interrupts, priority 3. `get_last_errors` on. |
| I3C1 | **Target** mode. IBI on with a 1-byte payload, hot-join on. Max read/write length 64. EV/ERR interrupts, priority 2. `get_last_errors` on. |
| TIM2 | Timebase and SYNC capture, see §3.3 |
| TIM1 | LED PWM, see §3.7. Output routed to PB8 (`chx_gpio`). |
| GPIO PA0 | SENS_INT: input, no pull, EXTI0 falling edge, priority **0** (highest; detection-related) |
| GPIO PB12 | CHAIN_IN: input, **pull-down**, EXTI12 both edges, priority 6 |
| GPIO PA1 | CHAIN_OUT: push-pull output, initial **low** |
| GPIO PA2 | BUS_INT: **open-drain** output, initial **released (high)** |
| GPIO PB4 | CS: push-pull output, initial **high** |
| SPI1 | Master, 4.5 Mbit/s (suits wires on pads). Generated but **not called** at startup. |
| PWR | WKUP1 (PA0) low polarity. GPIO retention in Standby. Stop1 runtime helpers. WKUP2 deliberately **not** configured at boot (§8). |
| NVIC | 4 bits preemption, 0 sub. SysTick 15 (lowest). |
| MPU / ICACHE | CubeMX defaults (ICACHE on, MPU on as required with ICACHE) |
| Pinout | Every used signal pinned. The 20 unconnected pins excluded. |

**Interrupt priority plan (keep it):**

| Prio | IRQ | Why |
|---|---|---|
| 0 | EXTI0 (SENS_INT) | Anything stamped in software here needs the least jitter |
| 1 | TIM2 (SYNC capture, sample trigger and scheduled LED compares) | Capture is in hardware; the ISR is bookkeeping but must not lag a period |
| 2 | I3C1 EV/ERR | Respond to the mainboard |
| 3 | I2C1 EV/ERR | Sensor transfers |
| 6 | EXTI12 (CHAIN_IN) | Enumeration only |
| 15 | SysTick | HAL timebase |

**Not configured yet:**

- **IWDG:** a later stage, see §7. When added, follow the hub's
  `docs/RELEASING.md` "After a CubeMX2 regeneration" notes.
- **RTC, LPTIM1:** the hub uses LPTIM1 on LSI as its Stop1 wake timer. Add the
  same if timed NAPs are needed.
- **SPI1 use:** spare breakout for later experiments.

---

## 5. Bus protocol: shared contract with the mainboard firmware

Write this as **one header, `lane_protocol.h`, kept byte-identical** in the lane
and mainboard trees. The hub does exactly this with `shared_config.h` and has a
host test checking the two copies are identical. Version it with a protocol
version register.

**The control side** of this contract is in
`../../ControlModule/SideModuleFirmware/CONTROL_FIRMWARE_GUIDE.md` §5.1 and
§7.2 (2026-11-17). It adds proposals the header must settle: the dynamic
address scheme (0x10 + lane index), EVENT_FIFO reads that drain up to 4
records and end early when fewer are queued, and polling as the first
collection method with IBI added later. The header does not exist yet;
write it once, into both trees, before lane step 6 and control task C1.

Proposed shape: I3C private transfers with a 1-byte register pointer, then data.
All multi-byte values are little-endian.

| Reg | Dir | Content |
|---|---|---|
| ID | R | Board type, image kind (APP or BOOT, the latter reserved for a later I3C bootloader), firmware version + git rev, image CRC32, protocol version, MCU UID (96 bit) |
| LANE_ID | R/W | Lane index assigned by the mainboard after enumeration (0 = unassigned) |
| STATUS | R | State (§3.1), FIFO count, dropped events, SYNC ok/lost, sensor ok, last error code, reset cause, fault breadcrumb id, uptime |
| EVENT_FIFO | R | 16-byte event records (§3.5). Reading pops. |
| SIGNAL | R | Latest raw value, baseline, thresholds in use (diagnostics) |
| CONFIG | R/W | Sample period and phase, IR current, integration time, thresholds, hysteresis, merge gap, block timeout, holdoff + holdoff sample period, raw-stream decimation |
| LED | W | Mode, brightness, blink parameters, optional apply-at-tick |
| SCHEDULE | W | "At mainboard time T do X" (LED state, sampling mode), queued (≥ 4), plus cancel-all |
| COMMAND | W | Calibrate, sampling mode (OFF / ON / RAW), sleep level, release chain, reset. **`ENTER_BOOTLOADER` reserved** for the later I3C lane bootloader (`SYSTEM_ARCHITECTURE.md` §10.2). |
| SYNC_ALIGN | W | Epoch message: tick of the mainboard's last SYNC edge + µs since that edge (§9) |

Rules:

- **Validate everything.** A bad register, length or value gets a NACK or an
  error flag in STATUS. It never causes undefined behaviour.
- **Writes to CONFIG are atomic:** apply them as a whole after the transfer, not
  byte by byte.
- **No persistence:** after every enumeration the mainboard pushes CONFIG. The
  lane boots with safe compiled defaults: sensor off, LED off.

---

## 6. Code structure and conventions (mirror the Yuhka hub)

Generate the CMake project from the `.ioc2` into this folder. CubeMX2 creates
`main.c`, `generated/`, `user_modifiable/`, `cmake/`, `CMakeLists.txt`.
`main.c` only calls `mx_system_init()`, then `laneInit()` and
`while (1) laneRun();`. This is the hub's pattern with `hubInit()` and
`hubRun()` in `app/coordinators/hub_master.c`.

```
app/
  config/       lane_behavior.h   knobs only (debug vs RELEASE switches), like hub_behavior.h
                lane_status.h     laneStatus_e, like hub_status.h
                lane_protocol.h   shared with the mainboard (§5)
  coordinators/ lane_master.c/h   init order, super-loop, error policy, state machine
                sense_manager.c/h sampler → vcnl4040 → lane_detect → event_fifo; holdoff, raw stream, calibration
                bus_manager.c/h   lane_bus + lane_chain; turns register writes into calls on the
                                  sense manager, the LED and the scheduled-action queue
  modules/      indicator_led     port of the hub driver to TIM1/PB8
                sys_power         port of the hub's Stop1/Standby/WFI handling
                fault_guard       port of the hub's watchdog/breadcrumb/boot-loop guard
                vcnl4040          sensor driver (I2C1, interrupt-driven)
                lane_sampler      TIM2 compare-driven trigger schedule
                sync_time         SYNC capture to mainboard-time mapping
                lane_detect       pure detection logic (host-tested)
                event_fifo        pure ring buffer (host-tested)
                lane_bus          I3C target: register map, IBI, hot-join
                lane_chain        CHAIN_IN/CHAIN_OUT state
test/           host tests, same rules as the hub's test/README.md
docs/           ERROR_HANDLING.md, PROTOCOL.md (or point at lane_protocol.h), RELEASING.md
```

Conventions taken from the hub. Read a few hub files first to match the feel:

- **Names:** functions are camelCase with the module prefix (`indicatorLedSet`).
  Enums end in `_e`, structs in `_t`. One owner per peripheral and pin,
  documented in the module header.
- **Header comments:** each module header opens with a block comment stating
  what the module owns, what it must never do, and its low-power behaviour.
  Comment density is explanatory, never decorative.
- **Status codes:** every fallible function returns `laneStatus_e`. `LANE_OK` is
  0, errors are negative, grouped in tens with gaps, and values stay stable
  across versions.
- **Error macros:** `LANE_ASSERT`, `LANE_ASSERT_RETURN` and
  `LANE_RETURN_IF_ERR`, with the same semantics as the hub's (see
  `hub_status.h`).
- **Knobs and values:** behaviour knobs live in `lane_behavior.h`; values live
  in the module that uses them.
- **Version stamping:** copy the hub's `cmake/version.cmake` and
  `version_stamp.cmake` (`MAJOR.MINOR.PATCH+gitrev`), and expose the string in
  the ID register.
- **Build:** use the toolchain bundled with STM32Cube:
  `~/.local/share/stm32cube/bundles/{cmake,ninja,gnu-tools-for-stm32}`. Preset
  `debug_GCC_STM32C542CCU6`.

---

## 7. Robustness, error handling, watchdog

Adopt the hub's policy (`docs/ERROR_HANDLING.md`, `app/modules/fault_guard.h`)
with one big advantage: **the lane has a channel back.** Faults are reported in
STATUS, so the app can show "lane 4: sensor I²C failing" instead of guessing.

**Escalation ladder:**

1. **Module-local retry.** For example, a sensor NAK: retry, then reinit the
   sensor, then reinit I2C1. Bus recovery: clock out a stuck SDA with up to 9
   SCL pulses.
2. **`laneErrorHandler()` in `lane_master.c`** is the single policy point:
   - debug builds trap with `LANE_ASSERT`;
   - release builds report in STATUS and degrade, for example "sensor dead, lane
     reports fault".
   - Use `faultGuardFatal()` only for truly unrecoverable states.
3. **`faultGuardFatal()`** writes the `.noinit` breadcrumb and resets cleanly.
   After reboot the lane re-enumerates and STATUS carries the reset cause and
   breadcrumb.
4. **Boot-loop guard:** too many abnormal resets lead to a parked state: LED
   off, sensor off, still enumerable, STATUS says why. Never hot-loop.

**Bus robustness:**

- **Hot-join:** if the mainboard does not answer, retry with exponential
  backoff.
- **Bus watchdog:** no valid traffic for N seconds while enumerated means
  sensor and LED go to safe defaults. Keep the address and keep answering.
- **I3C errors:** clear them via the ERR interrupt and never stall the target
  FIFO.

**Watchdog (later stage, but design for it now):**

- **IWDG, about 1–2 s,** fed once per `laneRun()` iteration. The lane has no
  long blocking operations; keep it that way. Any wait longer than about 10 ms
  is a state-machine step, not a loop.
- **Option bytes:** follow the hub's `tools/optprog.py` approach. Set
  `IWDG_STDBY = 0` so the watchdog is frozen in Standby. Leave `IWDG_STOP` per
  the NAP design.
- **Do not copy `BOOT_SEL`/`BOOT0` from the C542 hub script:** the lane has a
  real BOOT0 pull-down (R1), like the hub's C531 boards. The hub script says
  "do not cross-copy".
- **HardFault and unexpected-IRQ handlers** write the breadcrumb and reset, as
  in the hub.

---

## 8. Lessons from the hub to apply directly

These came from bench time on the same MCU family. Read the named files.

1. **An armed WKUP pin shadows its GPIO input.** Source: `sys_power.h`,
   `sysPowerStopInit()`. The generated `mx_pwr_init()` **enables WKUP1 (PA0,
   SENS_INT) at boot**, which can break the runtime SENS_INT read and EXTI0.
   - At init, call `HAL_PWR_LP_DisableWakeupPin(...)`, exactly as the hub's
     `sysPowerStopInit()` does.
   - Arm wake pins only at the edge of Standby, as in
     `sysPowerStandbyAttempt()`.
   - WKUP2 (PA2, BUS_INT) is deliberately not configured in CubeMX; configure
     and arm it at Standby entry.
2. **Stop1 round-trip** (`sysPowerStopFor()`):
   - Suspend the HAL tick and compensate `uwTick` with the real slept time.
   - Restore only the clock taps that went down. Stop1 wakes on HSIS only;
     PSIS (SYSCLK) and PSIK (I2C1 kernel) must come back.
   - Re-select the SYSCLK source.
   - Mask the wake EXTI lines outside the Stop window.
   - Keep SWD alive in debug builds with `LL_DBGMCU_EnableDebugLowPowerMode`,
     and never with `DBG_STANDBY`.
3. **Standby entry** (`sysPowerEnterStandby()`):
   - It is the single choke point.
   - Refuse and handle the case where a wake line is already asserted.
   - Clear stale wake flags.
   - Mark the transition for `fault_guard`.
   - Disable SysTick and clear all NVIC enables and pending bits before
     `HAL_PWR_EnterStandbyMode()`.
4. **The LED driver** (`indicator_led.c/h`) is the template for the lane LED:
   - Adopt the CubeMX period and duty at init.
   - On/off = start/stop channel plus counter. Brightness survives off/on.
   - When stopping, disable the channel first so the pin parks at idle low.
   - Only flip the state flag after the HAL call succeeded, because the new HAL
     traps on starting an already-running timer or stopping an idle one.
   - **Difference:** the lane uses TIM1, an advanced timer, not TIM15. Check
     that the HAL OC start enables the main output (MOE / BDTR). If not, the
     pin stays dark.
5. **C5 HAL `HAL_OK` is `0xEAEAEAEA`, not 0.** Always compare `== HAL_OK`, and
   make any host-test shim use the real value (`test/README.md`).
6. **After a CubeMX2 regeneration**, see hub `docs/RELEASING.md`:
   - the root `CMakeLists.txt` is overwritten, so restore the hand-edited one
     from git;
   - re-check initial GPIO levels and that `mx_system.c` still calls what the
     app relies on.
7. **Host tests** (`test/run_all.sh`, built with `-DRELEASE -Wall -Wextra
   -Werror`): every rejection case breaks exactly one rule. Lane candidates:
   `lane_detect`, `event_fifo`, the `sync_time` mapping, the register and
   command parser, and the `lane_protocol.h` copy check.

**CubeMX quirk:** the saved `.ioc2` shows PB7 SPI1_SCK as "AF5". That value is
stale and unused. Generated code correctly uses `HAL_GPIO_AF_6` (verified by
generating and building). Ignore it in the pinout view.

---

## 9. Open questions to settle on the bench or in RM0522 / the datasheet

The C531 datasheet is local at `~/Documents/Yuhka_Docs/stm32c531cc.pdf`; it has
the same die and peripherals as the C542.

`[x]` resolved, `[~]` partly answered or implemented pending agreement,
`[ ]` open.

- [~] **I3C target wake-up from Stop.** Can an address match wake the C542 from
      Stop1? This decides whether NAP can be per-lane.
      **Datasheet answer (DS15125 rev 2, §3.24, 2026-11-10):** as a target the
      I3C wakes from Stop only *on a reset pattern* or *on a missed start*,
      not on an address match. So any START on the bus wakes every napping
      lane, and the transfer that woke it is lost (not acknowledged). NAP
      therefore cannot be woken individually and cannot answer the first
      access: keep per-lane sleep = IDLE (§3.8). NAP is only useful as a
      whole-bus quiet period, and the mainboard must expect a retry after
      waking. Confirm the missed-start flow in RM0522 and on the bench.
- [~] **I3C target details on STM32C5:** hot-join behaviour after RSTDAA, the
      IBI-with-payload flow, MRL/MWL handling, and whether a static address
      (SETDASA) is available as a fallback to hot-join.
      **Partly answered by the datasheet (Table 7):** static address is
      *not* supported when target, so there is **no SETDASA fallback**:
      CHAIN + hot-join + ENTDAA is the only enumeration path. IBI payload is
      programmable up to 4 bytes (1 is configured). Hot-join after RSTDAA,
      MRL/MWL: still RM0522 / bench.
- [ ] **VCNL4040 force-mode conversion time** at the shortest PS_IT, the
      reachable sample rate, the IR current needed at 15 cm, and saturation on
      the foil.
- [x] **TIM1 MOE** handling in the C5 HAL OC start (§8.4). **Resolved
      (stm32c5xx_hal_tim.c):** `HAL_TIM_OC_StartChannel()` calls
      `LL_TIM_EnableAllOutputs()` (sets MOE) on every break-capable instance,
      TIM1 included; `HAL_TIM_OC_StopChannel()` clears MOE once no channel is
      left enabled. `indicator_led` relies on both; nothing extra needed.
- [x] **SYNC frequency and epoch-marker scheme.** **Accepted for the control
      side as-is** on 2026-11-16 (`SYSTEM_ARCHITECTURE.md` §3). The control
      module's TIM12 already runs SYNC at 1 kHz from its crystal. Lane side
      implemented (`sync_time`, host-tested):
      - **1 kHz** (`SYNC_TIME_DEFAULT_PERIOD_US`), settable 100 µs–100 ms; a
        period change over the bus resets lock and alignment.
      - **Epoch by bus message, no marked edge:** the mainboard sends
        *(tick of its last SYNC edge, µs since that edge)*, measured just
        before sending. The lane resolves which of its own edges that was, so
        the message may straddle an edge; its latency only has to stay below
        P/2 (500 µs). No falling-edge capture, no stretched pulse, TIM2_CH2/3
        stay free. The mainboard can repeat it any time as a consistency
        check (the lane counts any shift).
      - Every mapped time carries flags: NO_LOCK, UNALIGNED, EXTRAPOLATED.
      It goes into `lane_protocol.h` (§5) as the SYNC_ALIGN register.
- [ ] **Bench I3C controller** until the mainboard exists: a NUCLEO-C562RE or
      NUCLEO-C542RC. CubeMX2 has templates for both. Alternatively a second
      lane board running controller-mode test firmware.

---

## 10. Suggested bring-up order

Each step should end in something observable on the bench:

1. Generate the CMake project, build, flash over the Tag-Connect, and run an
   empty loop. Add `laneInit()`/`laneRun()`, `lane_status.h` and
   `lane_behavior.h`.
2. **LED:** port `indicator_led` to TIM1. Test brightness, blink and gamma.
3. **TIM2 timebase:** free-running, then SYNC capture fed from a function
   generator or the Nucleo. Test the mapping math in host tests first.
4. **VCNL4040 driver:** ID read, force-mode sample on a TIM2 compare schedule.
   Measure latency and rate. Stream to SWO/ITM (PB3) in debug builds.
5. **Detection and event FIFO,** host-tested, then on the test straight with
   real cars.
6. **I3C target** against the bench controller: register map, IBI, hot-join.
7. **CHAIN enumeration** with two or more boards. Test unplugging and resetting
   one in the middle.
8. **Scheduled LED actions:** all lanes go dark at the same tick. Check with a
   scope on two boards.
9. **Sleep levels:** IDLE, then NAP and Standby once §9 is answered.
10. **`fault_guard`:** breadcrumb, boot-loop guard, then the IWDG and the option
    bytes script.

---

## 11. Firmware status

Updated 2026-11-10 after the first firmware pass (bring-up steps 1–3, built
and host-tested; **not yet flashed or bench-tested**).

The CMake project is `LapCounterLaneModule_cmake/` (generated from the
`.ioc2` by the hardware session; unchanged by CubeMX since). Build, release
and regeneration notes: `LapCounterLaneModule_cmake/docs/RELEASING.md`. Host
tests: `LapCounterLaneModule_cmake/test/run_all.sh` (2 suites, all passing).

| Step | State | Where |
|---|---|---|
| 1 Skeleton | Done: `main.c` → `laneInit()` / `laneRun()`, `lane_status.h`, `lane_behavior.h`, version stamp, debug + release presets, strict warnings on `app/` | `app/coordinators/lane_master.c`, `cmake/lane_app.cmake` |
| 2 LED | Done: TIM1_CH1 driver, gamma 2.2 levels 0–1000, non-blocking blink, ISR-safe on/off for scheduled actions; debug builds blink 4× at boot | `app/modules/indicator_led.*`, `led_pattern.*` |
| 3 Timebase | Done: TIM2 owner (free-running 1 µs, SYNC capture IRQ, two one-shot compare alarms CH2/CH4, CH3 reserved for BUS_INT), SYNC mapping as above | `app/modules/lane_timer.*`, `sync_time.*` |

Decisions taken in this pass:

- **Root `CMakeLists.txt` holds one added line** (`include(cmake/lane_app.cmake)`);
  everything else lives in that file, because CubeMX2 overwrites the root
  file and this tree is not in git (unlike the hub, where it is restored
  from git).
- **LED level is a compiled default (full), not the CubeMX boot duty**, which
  is 0 by design (dark at boot). The mainboard sets the level.
- **LED switch tracks channel and counter separately.** The hub's driver
  flips one flag after both calls; if the channel start succeeds and the
  counter start fails, its retry re-starts a running channel, which the C5
  HAL traps on. The lane driver finishes the half-done switch instead.
  Worth back-porting to the hub.
- **Alarms use LL register access**, not the HAL channel state machine, so
  they can be re-armed from the ISR. A due time that has already passed
  fires at once (software compare event) instead of 71 min later.
- **Errors** go through `laneErrorHandler()`: recorded as last error and
  count (for STATUS) and asserted in debug. No reboot path yet:
  `fault_guard` is step 10.

Next: step 4 (VCNL4040). Before relying on SENS_INT, apply §8.1: disable
WKUP1 at init (the generated `mx_pwr_init()` arms it at boot), which belongs
with the `sys_power` port.
