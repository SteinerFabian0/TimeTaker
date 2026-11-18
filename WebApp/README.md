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
| `src/protocol/generated.ts` | **Generated** from `../protocol/host_protocol.toml` (protocol 0.2). Never edit it. |
| `src/protocol/wire.ts`, `codec.ts`, `packet.ts` | Struct encoding, COBS + CRC-16 framing, frame ↔ header + payload |
| `src/util/` | `clock.ts` (Scheduler: real, or `VirtualClock` for tests), `rng.ts`, `bytes.ts` (hex, CRC-32) |
| `src/transport/` | `Transport` interface; `serialTransport.ts` (Web Serial), `bleTransport.ts` (Web Bluetooth NUS, 20 B writes, `getDevices()` reconnect), `simTransport.ts` |
| `src/device/deviceSession.ts` | One connection: HELLO, bind wait (UNBOUND → poll HELLO), requests with timeouts, ERR_NO_SESSION / ERR_NOT_BOUND recovery, events exactly once in evSeq order (gap → RESUME), PING clock, stale detection |
| `src/device/deviceApi.ts` | A typed method per 0.2 request; retries idempotent ones on timeout |
| `src/device/deviceManager.ts` | Transport preference USB > BLE, reconnects, switching without loss, node model |
| `src/device/nodeModel.ts` | Pure node model from NODE_LIST + EVT_LANE_STATE / EVT_BLE_LINK / EVT_SEQ_* |
| `src/device/clientId.ts` | clientId in an injectable storage (localStorage / memory) |
| `src/device/simDevice.ts`, `sim/simCars.ts` | **The simulator**: protocol 0.2 and the device behaviour of CONTROL_FIRMWARE_GUIDE §5. Its header lists every rule and assumption [A1–A18]: the reference the firmware is compared against |
| `src/store/eventStore.ts` | Append-only (bootId, evSeq) device log + host actions + races, behind `EventStorage` (`MemoryStorage`; `indexedDbStorage.ts` in the browser) |
| `src/store/raceFile.ts` | Race export/import format `timetaker-race` v1 (documented in the file) |
| `src/race/raceEngine.ts` | **Pure** `fold(config, deviceEvents, hostActions) → RaceState` |
| `src/race/rules/` | `config`, `timeline`, `start`, `laps`, `finish`, `actions`, `standings`: each file documents its rule |
| `src/director/raceDirector.ts` | UI intents → device requests + host actions; adaptive holdoff |
| `src/sync/raceSync.ts` | BroadcastChannel publisher/subscriber (TV window = pure view), `FakeChannelHub` |
| `src/update/firmwareUpdater.ts` | REBOOT{BOOTLOADER} → UPDATE_BEGIN/CHUNK/END → BOOT_APP, resumable and retry-safe |
| `debug/` | The debug console (plain DOM) |
| `tools/` | `tt.ts` bench CLI + suites, fixture generator, mutation check, dev server: see `tools/README.md` |
| `test/` | `node:test`; `fixtures/races/*.json` are recorded races (regression tests) |

## Tests

`npm test`: 182 tests, about 2 s. Simulated time runs on `VirtualClock`, so a
60 s bind timeout or a 20-lap race takes milliseconds.

- protocol (golden bytes), session, simulator behaviour, device layer
  (manager: USB → BLE → USB without loss), race rules, recorded races,
  director + sync, updater, BLE transport (fake `navigator.bluetooth`), CLI
  (every bench suite against `--sim`, and the real tty path through a socat
  pty pair).
- **Recorded races**: `tools/genFixtures.ts` drives the whole stack against
  the simulator. Each fixture's `expect` comes from the simulator's ground
  truth through an independent oracle, not from the engine. The test also
  re-records every scenario and requires identical bytes, and checks that
  pulling USB mid-race and continuing over BLE gives identical standings.
- **Mutation check** (`node tools/mutate.ts`, 2026-11-18): 17 single-rule
  mutations (min-lap, holdoff, ignore, false start, first crossing,
  false-start penalty, tEntryUs order, aborted sequence, practice filter,
  laps finish, time finish, undo, lap adjust, penalties in total, DQ last,
  reboot gap, laneSeq gaps). Every one fails 1–13 tests.

## Race rules (defaults, `src/race/rules/config.ts`)

- Start: the race begins at the first EVT_LIGHTS_OUT after an un-aborted
  EVT_SEQ_STARTED of the same boot. Crossings before the sequence are practice.
- First crossing: `startsTiming` (grid behind the line): the first crossing
  after lights out starts lap 1; `completesLap1` (grid after the line): lap 1
  runs from Tout. Reaction time = first crossing − Tout (includes the run-up).
- False start: a crossing of the start procedure before Tout (logged after
  EVT_SEQ_STARTED or at/after T0). Default action `flag`; `penalty` (5 s) or
  `disqualify` configurable. In `startsTiming` it is the car's start crossing.
- Laps: crossings ordered by tEntryUs; rejected if IN_HOLDOFF
  (`rejectInHoldoff`) or under `minLapMs` (2 s) since the last counted
  crossing; a rejected crossing never moves the lap start. BLOCKED counts at
  its entry time (`blockedCountsAsCrossing`); CLEARED never. Flags: suspect
  timing (NO_LOCK/UNALIGNED/EXTRAPOLATED), lost lane events (laneSeq gap),
  lane outage, reboot gap, blocked, accepted.
- Finish: laps race: first to the lap count takes the flag, the others
  finish on their next counted crossing. Time race: the flag is out at the
  first counted crossing at or after the duration; every car finishes on its
  next one. The race is finished when every started, non-DQ, non-retired car
  has finished, or on `endRace`.
- Corrections are host actions: ±laps (count from the start), penalty ms,
  disqualify, retire, ignore/accept one crossing, endRace (freezes at its log
  position), undo (undo of undo works). Invalid ones are ignored with a reason.
- Standings: classified before DQ, laps desc, total time (race time of the
  last counted crossing + penalties) asc; gap/interval in laps or ms.
- Reboot mid-race: a new bootId is a flagged timing gap; the lap spanning it
  counts with no time; race time continues from the old boot's last event.

## Contract change requests

Done in protocol 0.3: HELLO_RSP reports `boardType` (the updater tells the
bootloader from the app by it; NODE_LIST stays the fallback for 0.2 firmware),
`[usb]` has the bootloader's `pid_boot` (port filter and CLI accept both PIDs),
and UPDATE_CHUNK documents the resume rule (re-sent matching bytes are
acknowledged with the real `nextOffset`).

Open:

4. **HELLO_RSP / EVT_BOOT: firmware version of CTRL** would save a NODE_LIST
   round trip (optional).
