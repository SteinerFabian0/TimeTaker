# TimeTaker web app

The race control app for Chrome/Edge. It reaches the control module over
**Web Serial** (USB CDC) or **Web Bluetooth** (the nRF bridge), and it is the
race engine: a pure fold over the device's timestamped events. Layering and plan: `../SYSTEM_ARCHITECTURE.md` §11.

## Run

```
npm test                     # all tests (Node >= 22.18 runs the .ts files directly)
npm run debug                # dev server → http://localhost:5173/debug/
node tools/tt.ts --sim suite all   # bench CLI against the simulator (tools/README.md)
node tools/tt.ts hello             # ... and against the board on /dev/ttyACM*
```

There is no install and no build step. Node strips the TypeScript types
itself: in the tests, and in `tools/serve.mjs` for the browser. That is why
the TypeScript stays within `erasableSyntaxOnly`:

- no `enum` (use `as const` objects), no `namespace`;
- no parameter properties, no `private` constructors (use `#private`
  fields);
- imports carry the `.ts` extension.

## Debug console (`debug/`)

The bring-up window onto the control module:

- **Connect USB** opens Chrome's port picker, which lists only TimeTaker
  devices (VID/PID from `[usb]` in `../protocol/host_protocol.toml`).
- **Simulator** connects to `SimDevice` instead: no hardware needed.
- **Shows** the session (bootId, protocol, live device clock, round trip,
  event position), the node list, every event decoded, and a trace of the
  raw frames.
- **Controls:** LANE_POWER on/off (SYNC starts 30 ms after the rail, which
  is the moment for the scope), NODE_LIST, REBOOT.
- **Simulator panel:** buttons, simulated cars, "drop 3 events" (to watch
  the RESUME recovery), power cycle (to see a new bootId).

**On Linux:** the user needs access to `/dev/ttyACM*`. Being in the `dialout`
group is enough (`id -nG`). Otherwise add a udev rule:
`SUBSYSTEM=="tty", ATTRS{idVendor}=="1209", ATTRS{idProduct}=="0001", TAG+="uaccess"`.

## Layout

Backbone only, no GUI yet (SYSTEM_ARCHITECTURE §11). `race/`, `device/`,
`protocol/`, `store/` import nothing from the DOM or a UI framework.

| Path | What |
|---|---|
| `src/protocol/generated.ts` | **Generated** from `../protocol/host_protocol.toml` (protocol 0.5). Never edit it. |
| `src/protocol/wire.ts`, `codec.ts`, `packet.ts` | Struct encoding, COBS + CRC-16 framing, frame ↔ header + payload |
| `src/util/` | `clock.ts` (Scheduler: real, or `VirtualClock` for tests), `rng.ts`, `bytes.ts` (hex) |
| `src/transport/` | `Transport` interface; `serialTransport.ts` (Web Serial), `bleTransport.ts` (Web Bluetooth NUS, 20 B writes, `getDevices()` reconnect), `simTransport.ts` |
| `src/device/deviceSession.ts` | One connection: HELLO, bind wait (UNBOUND → poll HELLO), requests with timeouts, ERR_NO_SESSION / ERR_NOT_BOUND recovery, events exactly once in evSeq order (gap → RESUME), PING clock, stale detection |
| `src/device/deviceApi.ts` | A typed method per 0.5 request; retries idempotent ones on timeout |
| `src/device/deviceManager.ts` | Transport preference USB > BLE, reconnects, switching without loss, node model |
| `src/device/nodeModel.ts` | Pure node model from NODE_LIST + EVT_LANE_STATE / EVT_BLE_LINK / EVT_SEQ_* (a lane NODE_LIST lists as UPDATING is `joined`: config push running) |
| `src/device/clientId.ts` | clientId in an injectable storage (localStorage / memory) |
| `src/device/simDevice.ts`, `sim/simCars.ts` | **The simulator**: protocol 0.5 and the device behaviour of CONTROL_FIRMWARE_GUIDE §5. Its header lists every rule and assumption [A1–A21]: the reference the firmware is compared against |
| `src/store/eventStore.ts` | Append-only (bootId, evSeq) device log + host actions + races, behind `EventStorage` (`MemoryStorage`; `indexedDbStorage.ts` in the browser) |
| `src/store/raceFile.ts` | Race export/import format `timetaker-race` v1 (documented in the file) |
| `src/race/raceEngine.ts` | **Pure** `fold(config, deviceEvents, hostActions) → RaceState` |
| `src/race/rules/` | `config`, `timeline`, `start`, `laps`, `finish`, `actions`, `standings`: each file documents its rule |
| `src/director/raceDirector.ts` | UI intents → device requests + host actions; adaptive holdoff |
| `src/sync/raceSync.ts` | BroadcastChannel publisher/subscriber (TV window = pure view), `FakeChannelHub` |
| `debug/` | The debug console (plain DOM) |
| `tools/` | `tt.ts` bench CLI + suites, fixture generator, mutation check, dev server: see `tools/README.md` |
| `test/` | `node:test`; `fixtures/races/*.json` are recorded races (regression tests) |

## Tests

`npm test`: 216 tests, about 2 s (seam review 2026-11-18: 50 runs, 36 of
them 12 at a time, no flaky test). Simulated time runs on `VirtualClock`, so a
60 s bind timeout or a 20-lap race takes milliseconds.

- protocol (golden bytes), session, simulator behaviour, device layer
  (manager: USB → BLE → USB without loss), race rules, recorded races,
  director + sync, BLE transport (fake `navigator.bluetooth`), CLI
  (every bench suite against `--sim`, and the real tty path through a socat
  pty pair).
- **Recorded races**: `tools/genFixtures.ts` drives the whole stack against
  the simulator. Each fixture's `expect` comes from the simulator's ground
  truth through an independent oracle, not from the engine. The test also
  re-records every scenario and requires identical bytes, and checks that
  pulling USB mid-race and continuing over BLE gives identical standings.
- **Mutation check** (`node tools/mutate.ts`, 2026-11-18): 35 single-rule
  mutations of the race engine (every RACE_RULES.md rule plus the timeline,
  flags, corrections and ranking). Every one fails at least one test.

## Race settings

The rules are the user's decisions in [`../RACE_RULES.md`](../RACE_RULES.md);
`src/race/rules/config.ts` holds the defaults, each rule file documents its
rule. Settings (main = race setup screen, deeper = advanced page):

| Setting | Config field | Default | Options |
|---|---|---|---|
| False start: what happens (main) | `falseStart.action` | `timePenalty` | `flag`, `timePenalty`, `disqualify` |
| Penalty seconds (main) | `falseStart.penaltyMs` | 5000 | ms |
| How the penalty applies (deeper) | `falseStart.apply` | `addToFirstLap` | `addToTotal`; `stopAndGo` reserved (power board), rejected for now |
| Crash policy (main) | `crashPolicy` | `replacementAllowed` | `crashIsDnf` (host action "crashed / out": `director.crashOut(lane)`) |
| Minimum lap time (deeper) | `minLapMs` | 20000 | ms; a faster lap never counts (no "accept") |
| Attention pulses (deeper) | `startLights.attentionPulses` | 2 | |
| Pulse rise time (deeper) | `startLights.pulseRiseMs` | 1000 | ms |
| Step between lights (deeper) | `startLights.stepMs` | 1000 | ms |
| Hold range (deeper) | `startLights.holdMinMs` / `holdMaxMs` | 200 / 3000 | ms, random hold before lights out |

Fixed rules (not settings): grid behind the line (the first crossing after
lights out starts lap 1's timing); crossings timed before the start sequence
began (T0) are ignored; a car stopped on the line (BLOCKED) is a crossing; a
disqualification applies from its log position, later laps never count and
the lap in progress shows TLF; the race ends when every car still in the race
(not disqualified, retired or crashed out) has finished; ranking: more laps,
lower total (with penalties), then faster best lap, then lower lane.
Internal: `rejectInHoldoff` (the lanes' holdoff flag), `suspectFlags`.

Also: crossings ordered by tEntryUs; flags for suspect timing, lost lane
events, lane outage, reboot gap, blocked, false-start penalty. Corrections
are host actions: ±laps, penalty ms, disqualify, retire, crashed/out, ignore
one crossing, endRace (freezes at its log position), undo. Invalid ones are
ignored with a reason. A reboot mid-race is a flagged timing gap; the lap
spanning it counts with no time. Simulated race cars
(`raceCarProfiles()` in `src/device/sim/simCars.ts`) lap in 30–40 s.

## Contract change requests

Done in protocol 0.3: HELLO_RSP reports `boardType`.

Done in protocol 0.4 (2026-11-18): **no field firmware update in v1.** Every
board is flashed over SWD / Tag-Connect, so the updater (`src/update/`), the
simulator's bootloader, the CLI `update` command and suite and the second USB
PID are gone. The update ids 0xA0–0xAF are reserved; REBOOT{BOOTLOADER}
answers ERR_UNSUPPORTED. The old code is at git tag `archive/bootloader-update`.

Done in protocol 0.5: START_SEQUENCE `pulseRiseMs` / `attentionPulses`
(the attention breathing, RACE_RULES.md "Start lights").

Open:

4. **HELLO_RSP / EVT_BOOT: firmware version of CTRL** would save a NODE_LIST
   round trip (optional).
