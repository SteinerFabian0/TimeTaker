# WebApp tools

| File | What |
|---|---|
| `tt.ts` | Bench CLI: the host protocol to the control module on `/dev/ttyACM*`, or to the simulator (`--sim`) |
| `lib/bench.ts` | Bench context: session, event log, reconnect with resume point, operator prompts, event formatting |
| `lib/benchSuites.ts` | Scripted bench suites (below) |
| `lib/ttyTransport.ts` | `Transport` on a tty (`stty raw`, libuv tty read, blocking write) and the sysfs port list |
| `lib/raceScenarios.ts` | Recorded-race scenarios + the ground-truth oracle for the fixtures |
| `genFixtures.ts` | Re-records `test/fixtures/races/*.json` (only on purpose) |
| `mutate.ts` | Mutation check of the race rules and the health / diagnostics rules (one broken rule per case) |
| `serve.mjs` | Dev server for the debug console |

## `tt` — bench client

```
export PATH=~/.local/bin:$PATH
node tools/tt.ts ports                         # which ttyACM is the TimeTaker (VID 1209, product "TimeTaker…")
node tools/tt.ts hello                         # finds the port itself; or --port /dev/ttyACM0
node tools/tt.ts nodes | settings
node tools/tt.ts status                        # health report: every part, every check (--raw: the STATUS fields)
node tools/tt.ts sensors --seconds 10          # live per-lane line: raw [min..max] baseline thresholds reference noise drift
node tools/tt.ts calibrate all                 # lanes free! learn + save each reference (SAVED can take 10 s)
node tools/tt.ts calibrate 1,3 --forget        # drop the saved references
node tools/tt.ts lane power on                 # then: watch
node tools/tt.ts watch --seconds 30            # every event, decoded, with device time
node tools/tt.ts lane config get 1
node tools/tt.ts lane config set all holdoffUs=2000000 --save
node tools/tt.ts lane sampling 1,2 on | lane led all blink 1000 | lane status 3
node tools/tt.ts lane raw 1 4 --seconds 2      # raw samples, decimation 4
node tools/tt.ts lane scan | lane calibrate all [forget]   # raw request; outcomes come as events
node tools/tt.ts start 6 1000 200 3000         # lights stepMs holdMin holdMax; abort
node tools/tt.ts bind list | bind allow 60 | bind remove <32 hex> | bind clear
node tools/tt.ts reboot                        # REBOOT{APP}; firmware goes on over SWD (no field update in v1)
node tools/tt.ts suite all                     # or: suite usb,lanes
```

`--sim` runs the same against `SimDevice` (virtual time; `watch` and `lane raw`
in real time, `--cars` powers the lanes and lets cars lap). `--json` prints
machine-readable output. The tool HELLOs as `CLIENT_TOOL`, so it never binds
itself for BLE. Exit code 1 on any error or failed check.

Linux access: be in `dialout` (see `../README.md`).

## Bench suites

They follow the firmware's "Done when" steps. On hardware the operator steps
are printed and the tool waits for Enter; with `--sim` the simulator does them.

| Suite | Checks | Matches |
|---|---|---|
| `usb` | HELLO (major, USB, TRUSTED, bootId), PING echo/RTT, NODE_LIST CTRL, STATUS log window, unknown id → ERR_UNSUPPORTED, wrong length → ERR_BAD_LENGTH, REBOOT{BOOTLOADER} and the reserved update id 0xA0 → ERR_UNSUPPORTED, replay from EVT_BOOT | C0 bench step 3 |
| `lanes` | JOINED→CONFIGURED in chain order, NODE_LIST lanes + versions, config round trip, LANE_STATUS per lane, absent lane → ERR_LANE_ABSENT, LANE_SCAN, *unplug ribbon 2→3* → 3..n LEFT, replug → re-join in order, power off → LEFT all | CONTROL guide §5.1 Done when |
| `crossings` | *a hand through every lane twice*: consecutive laneSeq, plausible tEntryUs and dwell; raw stream chunkSeq | §5.2 |
| `resume` | *crossings running, pull USB, plug back*: every evSeq once, in order; crossings from the outage arrive | §5.2 / SYSTEM_ARCHITECTURE §7.4 required test |
| `start` | 3 sequences: T0 lead ≥ 150 ms, ERR_SEQ_RUNNING, LED refused, no early LIGHTS_OUT, hold in range; abort idle → ERR_INVALID_STATE; abort → SEQ_ABORTED{HOST} | §5.3 (the scope part stays manual) |
| `health` | 0.6 STATUS fields, health self check (control, BLE chip, lanes 1..n online; sensor/SYNC/config/fault ok), LANE_STATUS rawMin ≤ raw ≤ rawMax and the window reset on read, *lanes free*: calibrate all → SAVED ≈ baseline, forget → FORGOTTEN, recalibrate, LANE_CALIBRATE during a sequence → ERR_SEQ_RUNNING | protocol 0.6 / status page |
| `binding` | a web app's USB HELLO binds it, BIND_ALLOW/0, BIND_LIST window, BIND_REMOVE, the EVT_BIND_CHANGED sequence; BLE UNBOUND → ERR_NOT_BOUND → bound live (sim only: Node has no Web Bluetooth) | §5.4 |

## Fixtures and mutation check

```
node tools/genFixtures.ts       # re-record (the fixture test checks they replay and re-record identically)
node tools/mutate.ts            # every rule mutation (race + health) must fail a test
```
