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

Still to come (guide §8.7): `lane_detect`, `event_fifo`, the register and
command parser, and the `lane_protocol.h` byte-compare against the mainboard's
copy.

Rules the suites follow (same as the hub's):

- Built with `-DRELEASE -Wall -Wextra -Werror`: they test the configuration
  that ships.
- The shim's `HAL_OK` is the C5 HAL's real `0xEAEAEAEA`, never 0.
- Every rejection case breaks exactly one rule and is consistent otherwise, so
  deleting that rule from the source fails the case. Keep it that way when
  adding cases.
- Only pure logic lives here. Drivers, timing and anything the optics decide
  are bench work.
