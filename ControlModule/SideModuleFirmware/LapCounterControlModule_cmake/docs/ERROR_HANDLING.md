# Error handling and fault containment

The control module is the time master of a race. It is always on USB power and
must stay **reachable**: a host that can still read STATUS and update the
firmware is worth more than a perfectly clean reboot. Every unexpected
condition ends in one of:

1. a clean **retry** inside the owning module (a NACKed lane transfer, a busy
   flash, a full queue): never seen by the policy;
2. a **reported** error: `ctrlStatus_e` up to `ctrlErrorHandler()`, kept as
   STATUS `lastError` / `errorCount`, the request answered with the status;
3. a **reboot** through `fault_guard` with a breadcrumb: HardFault, an
   unexpected IRQ, an unexplained NMI, a hung loop (IWDG). The next boot says
   `RESET_FAULT` or `RESET_WATCHDOG` in EVT_BOOT;
4. **parked**: too many reboots of kind 3 in a row.

## The ladder

```
module-local retry                      transient, never surfaces
      │ (still failing)
      ▼
ctrlStatus_e return ──► ctrlErrorHandler()   (ctrl_master.c, the single policy point)
      │                     debug:   CTRL_ASSERT (trap if armed), keep running
      │                     release: record, keep running degraded
      ▼
HardFault / IRQ / NMI / IWDG ──► breadcrumb (.noinit) + reset ──► EVT_BOOT RESET_FAULT/WATCHDOG
      │ (more than FAULT_GUARD_MAX_ABNORMAL in a row)
      ▼
boot-loop guard: parked
      runs on WITHOUT the watchdog, USB reachable, lanes off; the fix is a
      reflash over SWD (v1 has no bootloader and no field update)
```

Unlike the hub there is no Standby to sulk in: Stop and Standby stop TIM12
and with it the system clock.

## Lane bus errors (lane_manager, i3c_ctrl)

- A failed transfer completes with `ERR_LANE_ABSENT` (address NACK),
  `ERR_LANE_NACK` (data NACK), `ERR_TIMEOUT` (5 ms, aborted) or `ERR_I3C`. The
  queue always goes on.
- Three failures in a row on one lane: that lane is **LEFT**. Nobody is
  re-addressed; the lane's hot-join brings it back and it is configured again
  without the host.
- Three errors in a row with SDA low on an idle bus: SCL is clocked as GPIO up
  to 9 times, then I3C1 is set up again. If even that fails, every lane is
  LEFT and the bus stays down until LANE_SCAN or a power cycle.
- A lane whose ID reports another `LANE_PROTO_MAJOR`, or that never confirms
  its config: `EVT_LANE_STATE{FAULT}` and it is not polled for events.

## Faults that are only reported

| Condition | Report |
|---|---|
| USB_FAULT (PB12) edge | EVT_FAULT{USB, ERR_USB_FAULT}, STATUS usbFaults, DBG_LOG |
| EXT_PWR_FAULT (PA4) edge | EVT_FAULT{EXT_POWER, ERR_POWER_FAULT} (nothing is connected yet) |
| Settings page garbage, nothing valid | defaults, lastError ERR_SETTINGS_CORRUPT |
| Double ECC error reading flash (NMI) | counted (breadcrumb `eccNmis`), returned from; the settings CRC refuses the data |
| Option bytes not as tools/optprog.py sets them | lastError ERR_OPTION_BYTES; the firmware still boots |
| RNG failure | fallback word, `rngGet()` returns ERR_HAL; a start sequence never waits on it |

## The watchdog

`faultGuardStartWatchdog()` starts the IWDG at about 2 s (LSI ±30 %: 1.4–2.6 s)
at the end of `ctrlInit()`, `faultGuardTick()` feeds it first thing in every
`ctrlRun()` pass. Nothing in the loop waits longer than a few ms (the longest
blocking calls are a flash quad-word program, ~160 µs worst case, and the bus
recovery, ~100 µs). The IWDG freezes on a debug halt (`HAL_DBGMCU_IWDG_Freeze`).
`CTRL_DISABLE_WATCHDOG` (debug knob) leaves it off.

## Breadcrumb

`faultGuardBreadcrumb` (address in the `.map`) keeps, across resets: the last
fault id (HARDFAULT 1, UNEXPECTED_IRQ 2, FATAL_STATUS 3, NMI 4), stacked PC/LR,
IPSR, CFSR and BFAR/MMFAR, the abnormal-reset count, the raw RCC flags of the
last boot and the ECC NMI count. Read it over SWD after a provoked fault
(BENCH_TESTS.md F-1).
