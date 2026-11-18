# Host tests

The lane's pure logic, compiled with the laptop's `cc` from the real `app/`
sources. Nothing here touches hardware; `shim/` stands in for the HAL, and each
suite fakes the modules below the one it tests.

```
test/run_all.sh
```

Exit status is the number of failed suites. Run it before every image that
leaves the bench, and after any change to the files a suite covers.

| Suite | Covers | Reference it holds the code to |
|---|---|---|
| `host_sync_time` | `sync_time`: SYNC edges to mainboard µs under ±1 % HSI drift, TIM2 wrap, glitches, missed edges, long gaps, loss/extrapolation, a glitch as first anchor, an unannounced period change, alignment across an edge, the inverse mapping | guide §3.3; the scheme in `sync_time.h` |
| `host_led` | `led_pattern` (gamma 2.2, blink clock) and `indicator_led` against a fake TIM1 that refuses double starts/stops like the C5 HAL, incl. a switch that fails halfway | guide §3.7, §8.4 |
| `host_detect` | `lane_detect` on synthetic traces: learning, clean pass (entry/exit interpolated to the µs), merged pass, two cars, back-to-back, glitch below minDwell, hand -> BLOCKED/CLEARED, holdoff flagging (never dropping, no window extension), 100 Hz watch rate, saturation, 40 % baseline drift, TIM2 wrap, config rejects | guide §3.5, `lane_detect.h` |
| `host_event_fifo` | `event_fifo`: EVENT_FIFO read/pop rule (partial reads pop only fully clocked records, never more than offered), overflow keeps the oldest and still spends the seq, saturating drop count, a deep FIFO (more than 255 queued: `remaining` saturates at `LANE_FIFO_COUNT_MAX`, the ring wraps in order); RAW_FIFO chunks, grid breaks, partial pops, overflow gap in chunkSeq | `lane_protocol.h` EVENT_FIFO / RAW_FIFO |
| `host_schedule` | `schedule_queue`: time order, equal times FIFO, late fire with lateness, cancel-all, full, 64-bit start sequence | guide §3.7 |
| `host_regs` | `lane_regs`: every bad length, read-only/unknown register and out-of-range field rejected and counted (one rule per case), CONFIG atomic, ENTER_BOOTLOADER and deep sleep rejected; `image_crc` CRC32 check value | guide §5, `lane_protocol.h` |
| `host_chain` | `chain_logic`: debounce, CHAIN_OUT = address AND CHAIN_IN, fall forgets the address, hot-join backoff and cap, RSTDAA hold, re-plug joins at once | guide §3.2 |

`run_all.sh` also byte-compares `lane_protocol.h` with the control module's
copy.

**Mutation check:** `test/mutate.py` breaks each guarded rule in a temporary
copy of `app/` (87 mutations) and expects the guarding suite to fail. Run it
after changing a suite or a rule; add a mutation with every new rule.

Rules the suites follow (same as the hub's):

- Built with `-DRELEASE -Wall -Wextra -Werror`: they test the configuration
  that ships.
- The shim's `HAL_OK` is the C5 HAL's real `0xEAEAEAEA`, never 0.
- Every rejection case breaks exactly one rule and is consistent otherwise, so
  deleting that rule from the source fails the case. Keep it that way when
  adding cases.
- Only pure logic lives here. Drivers, timing and anything the optics decide
  are bench work.
