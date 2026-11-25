# TimeTaker web app

The race control app for Chrome/Edge. It reaches the control module over
**Web Serial** (USB CDC) or **Web Bluetooth** (the nRF bridge), and it is the
race engine: a pure fold over the device's timestamped events. Layering and plan: `../SYSTEM_ARCHITECTURE.md` §11.

## Run

```
npm test                     # all tests (Node >= 22.18 runs the .ts files directly)
npm run app                  # dev server → http://localhost:5173/app/  (the GUI)
npm run debug                # dev server → http://localhost:5173/debug/
npm run build                # production build → dist/ (static, offline PWA)
npm run deploy               # fallback publish from here; normally: GitHub → Actions → Deploy app
npm run serve:dist           # serve dist/ locally → http://localhost:5180/app/ (own port: its service worker must not sit on the dev origin)
npm install && npm run typecheck   # type check (the only thing that needs an install)
node tools/tt.ts --sim suite all   # bench CLI against the simulator (tools/README.md)
node tools/tt.ts hello             # ... and against the board on /dev/ttyACM*
```

There is no install and no build step for development. Node strips the
TypeScript types itself: in the tests, and in `tools/serve.mjs` for the
browser. That is why the TypeScript stays within `erasableSyntaxOnly`:

- no `enum` (use `as const` objects), no `namespace`;
- no parameter properties, no `private` constructors (use `#private`
  fields);
- imports carry the `.ts` extension.

Stripping does not check types. `npm run typecheck` does (`tsc` over `src`,
`test`, `tools` with `tsconfig.node.json`, and `app` with
`tsconfig.app.json`); its dev dependencies (TypeScript, the Preact typings
matching `vendor/`) are only for that.

## Ship: build, host, install

`npm run build` (`tools/build.mjs`) writes `dist/`: the modules reachable
from `app/main.ts`, types stripped and imports rewritten to `.js`, the
vendored libraries and fonts, a web app manifest and a service worker
(`dist/app/sw.js`) that precaches the whole build (~1 MB) under a content
hash. After the first visit the app starts and runs with **no network**.
A new build installs in the background and is used from the next start of
the app (close all its windows; a plain reload keeps the running build).

- **Host:** copy `dist/` to any static **HTTPS** host (or a sub-path of one:
  every URL is relative). USB and Bluetooth in the browser need HTTPS or
  localhost; `file://` does not work.
- **Deploy:** on GitHub, Actions → **Deploy app** → Run workflow
  (`.github/workflows/deploy-app.yml`: tests, build, publish; Settings →
  Pages → Source: GitHub Actions). Served at
  https://steinerfabian0.github.io/TimeTaker/app/ (no account needed to use
  it; the repo must stay public). It deploys what is pushed to `main`, so
  push first. `npm run deploy` (`tools/deploy.mjs`, pushes `dist/` to a
  `gh-pages` branch) is the fallback from this laptop; it needs a GitHub
  login in the terminal and Pages set to that branch. Setup sheet for hosts:
  [`HOST_SETUP.md`](HOST_SETUP.md).
- **Install:** open the URL in Chrome or Edge once and use "Install app"
  (address bar). It then has its own window and start-menu / dock entry and
  works offline. An installed app also gets persistent storage more readily.
- **Safety nets in the app:** it asks the browser to keep its storage
  (Settings → About shows whether it may still be cleared: export a backup),
  and holds a screen wake lock while a Grand Prix runs, so the laptop does
  not sleep and drop the link mid-event.

### Platforms

Needs **Chrome or Edge 111+** (an older or other browser gets a notice, not a
blank page). Lap times come from the time taker's own clock, so a slow
laptop shows them later, never wrong.

| Platform | USB | Bluetooth | Notes |
|---|---|---|---|
| Windows 10/11 | yes, no driver (inbox CDC driver) | yes | Edge is preinstalled: nothing to install at all |
| macOS | yes | yes | macOS asks once to let Chrome use Bluetooth |
| ChromeOS | yes | yes | |
| Linux | yes, with `dialout` or the udev rule below | behind a Chrome flag | `chrome://flags/#enable-experimental-web-platform-features` |
| Android | no | yes | layouts are not made for phones |
| iPad / iPhone | no | no | not supported (every iOS browser is WebKit: no Web Serial, no Web Bluetooth) |
| Firefox, Safari | no | no | the app runs, but cannot reach a time taker |

After a reload, USB reconnects by itself. Bluetooth does only where Chrome
offers `navigator.bluetooth.getDevices()` (still behind
`chrome://flags/#enable-web-bluetooth-new-permissions-backend` on most
systems); otherwise press Connect Bluetooth once more (Status).

A hidden or minimised window keeps receiving events (reads are not timers);
Chrome only slows its timers. The heartbeat ignores its own late beats
(`deviceSession.ts`), and in firmware v1 a silent host only changes the panel
LED: no session or event is lost.

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
| `src/protocol/generated.ts` | **Generated** from `../protocol/host_protocol.toml` (protocol 0.6). Never edit it. |
| `src/protocol/wire.ts`, `codec.ts`, `packet.ts` | Struct encoding, COBS + CRC-16 framing, frame ↔ header + payload |
| `src/util/` | `clock.ts` (Scheduler: real, or `VirtualClock` for tests), `rng.ts`, `bytes.ts` (hex) |
| `src/transport/` | `Transport` interface; `serialTransport.ts` (Web Serial), `bleTransport.ts` (Web Bluetooth NUS, 20 B writes, `getDevices()` reconnect), `simTransport.ts` |
| `src/device/deviceSession.ts` | One connection: HELLO, bind wait (UNBOUND → poll HELLO), requests with timeouts, ERR_NO_SESSION / ERR_NOT_BOUND recovery, events exactly once in evSeq order (gap → RESUME), PING clock, stale detection |
| `src/device/deviceApi.ts` | A typed method per 0.6 request (`laneCalibrate(mask, LEARN|FORGET)`); retries idempotent ones on timeout |
| `src/device/deviceManager.ts` | Transport preference USB > BLE, reconnects, switching without loss, node model; `subscribe()` for subsystems |
| `src/device/nodeModel.ts` | Pure node model from NODE_LIST + EVT_LANE_STATE / EVT_BLE_LINK / EVT_SEQ_* / EVT_LANE_CALIBRATED + STATUS (`applyStatus`: BLE bridge). A lane NODE_LIST lists as UPDATING is `joined`; CONFIGURED after FAULT = recovered |
| `src/device/laneFlags.ts` | LANE_STATUS.flags bits (lane_protocol.h LANE_STATUS_*) |
| `src/device/clientId.ts` | clientId in an injectable storage (localStorage / memory) |
| `src/device/simDevice.ts`, `sim/simCars.ts` | **The simulator**: protocol 0.6 and the device behaviour of CONTROL_FIRMWARE_GUIDE §5, a per-lane optical signal, calibration, BLE bridge state, fault knobs. Its header lists every rule and assumption [A1–A25]: the reference the firmware is compared against |
| `src/store/eventStore.ts` | Append-only (bootId, evSeq) device log + host actions + races, behind `EventStorage` (`MemoryStorage`; `indexedDbStorage.ts` in the browser) |
| `src/store/raceFile.ts` | Race export/import format `timetaker-race` v1 (documented in the file) |
| `src/race/raceEngine.ts` | **Pure** `fold(config, deviceEvents, hostActions) → RaceState` |
| `src/race/rules/` | `config`, `timeline`, `start`, `laps`, `finish`, `actions`, `standings`: each file documents its rule |
| `src/director/raceDirector.ts` | UI intents → device requests + host actions; adaptive holdoff; `active()` (race/start under way) |
| `src/health/health.ts`, `monitor.ts` | **Status & diagnostics**: pure health rules → `HealthReport`; the poller (STATUS, the single LANE_STATUS reader, lane history, calibration). See below |
| `src/sync/raceSync.ts` | BroadcastChannel publisher/subscriber (TV window = pure view), `FakeChannelHub` |
| `app/` | **The GUI** (Preact + htm + signals, no build step). Spec: [`GUI_SPEC.md`](GUI_SPEC.md). `shell/` rail + areas + router, `pages/`, `ui/` shared blocks, `i18n/` (en, de), `styles.css` tokens and the slanted look |
| `vendor/` | Vendored, unmodified: Preact 10.29.8, htm 3.1.1, @preact/signals 1.3.4 (+ core 1.14.5), mapped by the import map in `app/index.html`; fonts Chakra Petch and Titillium Web (OFL) |
| `debug/` | The debug console (plain DOM) |
| `tools/` | `tt.ts` bench CLI + suites, fixture generator, mutation check, dev server: see `tools/README.md` |
| `test/` | `node:test`; `fixtures/races/*.json` are recorded races (regression tests) |

## Tests

`npm test`: 410 tests (2026-11-25), about 2 s (seam review 2026-11-18: 50 runs, 36 of
them 12 at a time, no flaky test; 2026-11-19: 4 consecutive green runs after
the 0.6 diagnostics work). Simulated time runs on `VirtualClock`, so a
60 s bind timeout or a 20-lap race takes milliseconds.

- protocol (golden bytes), session, simulator behaviour, device layer
  (manager: USB → BLE → USB without loss), race rules, recorded races,
  director + sync, BLE transport (fake `navigator.bluetooth`), health rules
  (every threshold edge), the health monitor (rates, gating, single
  LANE_STATUS poller, history, calibration incl. timeout and refusal), the
  simulator's 0.6 signal and fault knobs → checks, CLI
  (every bench suite against `--sim`, and the real tty path through a socat
  pty pair).
- **Recorded races**: `tools/genFixtures.ts` drives the whole stack against
  the simulator. Each fixture's `expect` comes from the simulator's ground
  truth through an independent oracle, not from the engine. The test also
  re-records every scenario and requires identical bytes, and checks that
  pulling USB mid-race and continuing over BLE gives identical standings.
- **Mutation check** (`node tools/mutate.ts`, 2026-11-19): 64 single-rule
  mutations: 34 of the race engine (every RACE_RULES.md rule plus the
  timeline, flags, corrections and ranking) and 30 of the health rules,
  monitor, node model and simulator diagnostics (threshold edges, gates,
  single poller, timeout, window reset, reference seeding). Every one fails
  at least one test.

## Status & diagnostics backend

The contract the status page is built on (`src/health/`). No DOM.

### HealthReport (`evaluate(inputs, thresholds)`, pure)

```ts
HealthReport {
  overall: 'ok' | 'warn' | 'fail'          // worst check; 'fail' while the control module is offline
  control: { online, checks: Check[] }      // online: session open and STATUS fresh (< statusStaleMs)
  ble:     { online, connected, checks }    // online: nRF alive (STATUS.bleBridge != ABSENT); connected: a computer over BLE (bleLink UP)
  lanes:   [{ index: 1..6, online, checks }] // always 6; online: on the bus (node model)
}
Check { id: string, level: 'ok' | 'warn' | 'fail' | 'unknown', message: string, value: number | string | boolean | null }
```

Every part lists every check of its part, always in catalogue order (an
offline part's checks are `unknown`), so the GUI can keep fixed rows.
`CHECKS` exports the catalogue `{ id, part, title, description }`;
`DEFAULT_THRESHOLDS` holds every limit in one object (deeper settings later).
Edges: upper bounds `value >= warn → warn, >= fail → fail`; lower bounds
`value < warn → warn, < fail → fail`.

| Part | Check id | Rule (default thresholds) |
|---|---|---|
| control | `ctrl.link` | session open, STATUS answers and is ≤ 6 s old, else fail |
| | `ctrl.settings` | `settingsDefaults` = 1 → warn (settings lost at boot) |
| | `ctrl.lanePower` | lane rail off → warn |
| | `ctrl.usbFaults` | ≥ 1 warn, ≥ 3 fail |
| | `ctrl.errors` | errorCount ≥ 1 warn (names lastError) |
| | `ctrl.loopTime` | loopMaxMs ≥ 5 warn, ≥ 10 fail |
| | `ctrl.eventLog` | events overwritten before the app read them → fail; ≥ 100 behind → warn |
| BLE chip | `ble.bridge` | bleBridge ABSENT → fail |
| | `ble.version` | VERSION_MISMATCH → fail |
| | `ble.rssi` | with a BLE connection: < −80 dBm warn, < −90 fail; none / 127 → unknown |
| | `ble.linkCrc` | bleCrcErrors + nrfLinkCrcErrors ≥ 1 warn, ≥ 50 fail |
| | `ble.uart` | ≥ 1 warn, ≥ 50 fail |
| | `ble.dropped` | ≥ 1 warn, ≥ 100 fail |
| lane | `lane.present` | absent: fail if seen this session / in the race (rail on), else unknown |
| | `lane.fault` | FAULT (until CONFIGURED) → fail; LANE_STATUS.lastError → warn |
| | `lane.sensor` | SENSOR_OK clear → fail |
| | `lane.sync` | SYNC lost / not locked → fail; not aligned → warn |
| | `lane.config` | CONFIG_VALID clear → fail (warn while the push runs) |
| | `lane.calibrated` | CALIBRATED clear (learning) → warn |
| | `lane.savedReference` | a FAILED / timed-out calibration this session fail, NOT_SAVED warn; else `LANE_STATUS.savedReference`: reference == savedReference ok (running on the saved calibration), savedReference 0 warn (nothing saved), different warn (recalibrate); no LANE_STATUS yet: the session outcome, else unknown |
| | `lane.signal` | baseline < 1000 warn, < 400 fail (misaligned / dirty); baseline or rawMax ≥ 60000 warn, ≥ 65000 fail (saturated: lower IR current) |
| | `lane.drift` | baseline / reference < 0.85 warn, < 0.70 fail (clean the strip, recalibrate); > 1.20 warn |
| | `lane.noise` | noise / (baseline − thrLow) ≥ 0.20 warn, ≥ 0.33 fail |
| | `lane.blocked` | BLOCKED now → warn |
| | `lane.droppedEvents` / `lane.badWrites` / `lane.lateActions` | ≥ 1 warn (FIFO_OVERFLOW too) |
| | `lane.reset` | last reset WATCHDOG / BROWNOUT / FAULT → warn |

### HealthMonitor (`src/health/monitor.ts`)

```ts
const mon = HealthMonitor.attach(manager, { scheduler, busy: () => director.active() });  // started
mon.subscribe((u) => ...)   // u.type: 'sample' {lane, sample} | 'marker' {lane, marker} | 'report' {report}
                            //         | 'calibration' {lane, outcome} | 'connection' {connected}
mon.report()                // HealthReport from the latest data, no traffic
await mon.runSelfCheck()    // STATUS + NODE_LIST + every present lane fresh, then the report
await mon.calibrate([1, 3]) // or 'all': LEARN (lanes free!) → [{lane, result, reference, noise, sysUs, bootId, timedOut?}]
await mon.forget('all')     // drop saved references → 'forgotten' per index (present or not)
mon.samples(lane)           // [{tMs, raw, rawMin, rawMax, baseline, thrLow, thrHigh, reference, noise, flags}], oldest first
mon.markers(lane)           // [{tMs, lane, kind: 'calibration'|'joined'|'configured'|'left'|'fault', label, calibration?}]
mon.busy(), mon.laneRateHz(), mon.lastLaneStatus(lane), mon.nodes, mon.status, mon.stop()
```

- **Poll rates** (`MONITOR_DEFAULTS`): STATUS every 2 s; NODE_LIST only on a
  change (connect, boot, a lane event, lanesPresent differs). LANE_STATUS per
  present lane 4 Hz idle; 0.2 Hz while `busy()` (the director's race is
  starting / running / finishing, or the device reports seqRunning);
  `busyLaneRateHz: 0` switches it off. History 60 s per lane.
- **The single LANE_STATUS reader**: rawMin/rawMax are "since the previous
  LANE_STATUS of that lane" and every read resets them, so nothing else in
  the app may send LANE_STATUS while the monitor runs (tt's `lane status`
  is a bench tool). At most one request per lane in flight.
- **Graph per lane**: plot `raw` with the `rawMin..rawMax` band, `baseline`,
  `thrLow`/`thrHigh` and `reference` against `tMs` (host ms); draw
  `markers`. After a LEARN the lane uses the new reference at once (the
  reference line steps within one poll); the `calibration` marker comes with
  EVT_LANE_CALIBRATED, which follows the device's deferred flash write (up
  to ~10 s). The first sample after a calibration covers a shorter min/max
  window (the device read the signal itself).
- **Calibration**: refused (`MonitorBusyError`) while `busy()`; one at a
  time; no event within `calibrateTimeoutMs` (15 s) → `failed`, `timedOut`.
  LEARN of an absent lane → `failed` without asking. Outcomes also land in
  the node model (`lanes[i].calibration`) and so in `lane.savedReference`.
- **TODO** `highRate(lane)`: a short LANE_RAW_STREAM burst for a full-rate trace.

CLI: `tt status`, `tt sensors`, `tt calibrate LANES|all [--forget]`, suite `health`.

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
lights out is the start, not a lap; lap 1 is timed from lights out); crossings timed before the start sequence
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

Done in protocol 0.6: STATUS bleBridge / RSSI / link counters /
settingsDefaults, LANE_STATUS reference / rawMin / rawMax, LANE_CALIBRATE
{action} with EVT_LANE_CALIBRATED (the status page, `src/health/`).

Open:

4. **HELLO_RSP / EVT_BOOT: firmware version of CTRL** would save a NODE_LIST
   round trip (optional).
5. ~~Is a saved reference stored for a lane?~~ Closed: LANE_STATUS carries
   `savedReference` (the value the control module keeps for that lane index).
