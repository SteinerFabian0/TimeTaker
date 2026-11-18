# Host tests

The control module's pure logic, compiled with the laptop's `cc` from the real
`app/` sources. Nothing here touches hardware.

```
test/run_all.sh
```

The exit status is the number of failed suites. Run it before every image
that leaves the bench, after any change to a file a suite covers, and after
every change to `protocol/host_protocol.toml`.

| Suite | Covers | Reference it holds the code to |
|---|---|---|
| generated protocol files | `host_protocol.h`, `golden_cases.h`, the web app's `generated.ts` and `golden.json` match `host_protocol.toml` | `python3 protocol/gen_protocol.py --check` |
| `host_frame_codec` | `frame_codec`: CRC check value, the reference vectors, round trips of every length 1–1200 at five zero densities with random head/body splits, full 254-byte COBS blocks, one case per decoder rule (CRC, truncation, overflow, short, CRC-only, empty frames), resync after garbage, encoder capacity | `gen_protocol.py`'s reference encoder (golden vectors), SYSTEM_ARCHITECTURE §7.2 |
| `host_event_log` | `event_log`: evSeq numbering from 0, ring wrap and the oldest/next window, the largest event fitting, every size rejection, rejections consuming no numbers | SYSTEM_ARCHITECTURE §7.4 |
| `host_session` | `host_session` against a fake transport, real frames both ways: HELLO gate (`ERR_NO_SESSION`), HELLO length/MAJOR checks, history not pushed on HELLO, PING, events in order with evSeq, RESUME replay and its three refusals, backpressure with the response reserve, a lagging cursor skipping overwritten events, the owner hand-off (status forced, error = status only, malformed owner reply), host frames with flags, corrupt frames, reset | SYSTEM_ARCHITECTURE §7.3–7.4 |
| `host_protocol` | The packed structs of `host_protocol.h` against the reference bytes of every TOML example, header + payload through `frame_codec` to the reference frame and back, size limits (a full frame plus the nRF channel byte stays within 254 B on the wire) | `gen_protocol.py`'s reference packer; the web app holds `wire.ts` / `codec.ts` to the same bytes via `golden.json` |

| copy checks | `lane_protocol.h`, `ble_link_protocol.h` byte-identical with the lane / BLE trees | `cmp` |
| `host_session` (2026-11-18 additions) | per-session bind state from the onHello hook, the UNBOUND gate (HELLO/PING only, ERR_NOT_BOUND, owner never called, no events, no streams), binding live, reset to the configured state; deferred responses (completion with the original tag, one at a time -> ERR_BUSY, reset voids it); streams (STREAM flag, dropped beside the reserve) | CONTROL_FIRMWARE_GUIDE §5.4 |
| `host_bind_list` | add/touch/find, LRU eviction at 8, zero id refused, remove, clear, serialise and every deserialise refusal | §5.4 |
| `host_seq_planner` | T0/on[k]/hold/Tout arithmetic, n = min(lights, present), every refusal (lights, step, hold order, lead -> ERR_TOO_LATE, no lane, overflow) | §5.3 |
| `host_settings_record` | CRC-32 check value, record encode/check (magic, version, length, CRC, half-programmed page), two-page selection incl. sequence wrap and corrupt-vs-new board | §5.5 |
| `host_nrf_link` | link frame parsing (DATA 1..244, CTRL short/long/unknown, channels), encoders, credits (reset, take, return, clamp), byte ring, stream budget and the BLE rate | BLE_FIRMWARE_GUIDE §5 |
| `host_lane_logic` | join index + LEFT/release masks, EVENT_FIFO / RAW_FIFO early read ends, EVT_CROSSING mapping, seq gaps, health transitions, "configured", config checks, LANE_STATUS / NODE_LIST mapping | §5.1 |
| `host_update` | image header checks, image CRC with the header skipped, vector plausibility; UPDATE_BEGIN/CHUNK/END/ABORT against a fake flash that enforces erased-only quad-word programs: repeats, overlaps, gaps, header written last, resume, wrong CRC/board, flash failure | §5.7 |

Still to come: `led_pattern` (the lane tree tests it today; port its suite).

Rules the suites follow (same as the hub's and the lane's):

- They are built with `-DRELEASE -Wall -Wextra -Werror`, so they test the
  configuration that ships.
- **One broken rule per rejection case.** Every rejection case breaks exactly
  one rule and is consistent otherwise, so deleting that rule from the source
  fails the case. The frame codec suite was mutation-checked on 2026-11-16:

  | Mutation | Failing checks |
  |---|---|
  | No 0xFF exception | 5 |
  | Big-endian CRC | 10 |
  | No truncation check | 2 |
  | Relaxed overflow guard | 2 |
  | Off-by-one minimum length | 2 |
  | Two swapped struct fields in the header | 2 |

  The host session suite was mutation-checked the same day. Each of these
  mutations was caught, each failing 2–4 checks:
  - no response reserve;
  - no HELLO gate;
  - no bootId check;
  - no log-gap check;
  - host flags accepted;
  - owner status trusted;
  - HELLO replaying history;
  - no lag skip;
  - no MAJOR check.

  The suites added on 2026-11-18 were mutation-checked the same day (one
  mutation of a rule each, all caught): LRU picks the newest (3 failing),
  zero id accepted (3), duplicate accepted on deserialise (2); hold span
  off by one (the suite crashes: division by zero), lead check dropped (2),
  n = lights (2); sequence compare without wrap (2), version check dropped
  (2), corrupt never reported (2); CTRL short-body rule dropped (2), credit
  clamp dropped (2), empty DATA accepted (2); join index miscounted (3),
  partial FIFO record taken (2), FAULT not re-raised on a new error (2);
  header quad-words programmed early (5), resume re-initialises (3), Thumb
  bit not checked (2); UNBOUND gate dropped (4), events pumped while UNBOUND
  (3), second deferral accepted (3).

- Only pure logic lives here. Drivers and timing are bench work.
