# TimeTaker — Race rules (decided 2026-11-18)

The user's decisions for the web app's race engine (`WebApp/src/race/`).
Every item marked **setting** is a value the host changes in the web app;
"main" settings appear on the race setup screen, "deeper" settings in an
advanced page. The GUI comes later; the engine and the director already
take these as configuration.

## Start

- **Grid behind the line.** Cars are placed behind the start/finish line. The
  first crossing after lights out **starts lap 1's timing**; it is not a lap.
- **The host starts the countdown** only when no car is moving. Crossings
  before the start sequence began are ignored (grid positioning, practice).
- **Start lights** (executed by the lanes on the shared clock): when the host
  presses start, all lane LEDs breathe in unison (raised sine, off→full 1 s,
  full→off 1 s), twice. On the last fall every light goes dark except the
  lane at the far end of the chain. Then one more lane lights every second
  toward lane 1. With all lit, a random hold of **0.2–3.0 s** (F1 style),
  then all go out at the same instant: the race starts.
  Settings (deeper): number of attention pulses (2), pulse rise time
  (1000 ms), step between lights (1000 ms), hold range (200–3000 ms).

## False start

- A crossing after the start sequence began and before lights out.
- **Setting (main): what happens.** Options: flag only, **time penalty**
  (default), disqualify.
- **Setting (main): penalty seconds** (default 5 s).
- **Setting (deeper): how the penalty applies.** v1: **added to the first
  lap time** (default) or added to the total. Future, with the power board:
  **stop-and-go** — the car is physically held for the penalty time.

## Laps

- **Setting (deeper): minimum lap time**, default **20 s**. Any lap faster
  than this is discarded, always. There is no manual "accept anyway".
- A car stopped on the line counts as a crossing (starts the next lap).
- Lap times use the device timestamps only.

## Crashes

- **Setting (main): crash policy.**
  - **Replacement allowed:** a marshal puts the car back; the lap is just
    long. Nothing to enter.
  - **Crash = DNF:** the host marks the car "crashed / out" in the web app
    (until the power board can detect it). The car is removed from the
    list of cars the race waits for.
- **The race ends when every car still in the race has finished** (in both
  policies; the policy only decides whether a crashed car leaves that list).

## Disqualification

- Applies **from the moment it is given**: laps after it never count.
- **The lap in progress is aborted** and shown as "TLF" (no time).
- Future, with the power board: the car loses throttle control and is driven
  at minimum speed to the start/finish line, then for a configurable run-on
  time (**setting, deeper**, e.g. 1.5 s) to a convenient pick-up point.

## Finish and ranking

- Laps race: the first car to complete the lap count takes the flag; every
  other car finishes on its next counted crossing. Time race: each car
  finishes on its first counted crossing after the duration.
- Ranking: more laps first, then lower total time (including penalties).
- **Exact tie** (same total to the µs): the car with the **faster best lap**
  wins; if that is equal too, the lower lane number.

## Future (power board, SYSTEM_ARCHITECTURE §12)

- Stop-and-go penalty for false starts.
- Disqualified car driven home at minimum speed plus run-on time.
- Automatic crash detection (current loss) feeding the crash policy.
