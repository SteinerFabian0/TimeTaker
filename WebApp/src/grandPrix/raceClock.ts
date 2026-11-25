/*
 * What each lane's lap clock shows on the race page (GUI_SPEC §6.1b). Pure,
 * DOM-free: the page passes the race state (raceEngine.fold) and the device
 * time now.
 *
 *   idle      the lights are not out yet (0.000)
 *   live      the lap in progress, counting up from the car's last lap's end;
 *             lap 1 runs from lights out (the start crossing, right after it,
 *             does not stop or restart it)
 *   last      a lap just ended: its time, held for LAST_LAP_HOLD_MS, then
 *             the clock goes live again
 *   finished  the car took the flag: its last lap, held
 */

import type { CarResult } from '../race/types.ts';

/** How long a finished lap's time stays on screen before the clock runs live again. */
export const LAST_LAP_HOLD_MS = 5000;

export type LapClock =
  | { readonly kind: 'idle' }
  | { readonly kind: 'live'; readonly ms: number }
  | { readonly kind: 'last' | 'finished'; readonly ms: number | null; readonly n: number };

export function lapClock(car: CarResult | undefined, toutUs: number | null, nowUs: number): LapClock {
  if (!car || toutUs === null) return { kind: 'idle' };
  const last = car.lapTimes.at(-1);
  if (car.status === 'finished' && last) return { kind: 'finished', ms: last.ms, n: last.n };
  if (last && nowUs - last.endUs < LAST_LAP_HOLD_MS * 1000) return { kind: 'last', ms: last.ms, n: last.n };
  const from = last ? last.endUs : toutUs;
  return { kind: 'live', ms: Math.max(0, (nowUs - from) / 1000) };
}

/**
 * The lap a car is on, F1 style (like the warm-up): 0 on the grid, 1 from its
 * start crossing, n + 1 after n laps; never past the race's laps (null: a
 * time race). After the flag, or out of the race, its completed laps.
 */
export function lapOn(car: CarResult | undefined, laps: number | null): number {
  if (!car || car.status === 'grid') return 0;
  if (car.status !== 'running') return car.laps;
  return laps === null ? car.laps + 1 : Math.min(laps, car.laps + 1);
}

/** A lap time at full width, minutes:seconds.hundredths: "0:23.45", "1:02.34" (cut, never rounded up; at most 9:59.99). */
export function formatLap(ms: number): string {
  const t = Math.floor(Math.max(0, ms) / 10);       // hundredths
  if (t >= 60000) return '9:59.99';
  const m = Math.floor(t / 6000);
  const sec = Math.floor((t % 6000) / 100);
  const cs = t % 100;
  return `${m}:${String(sec).padStart(2, '0')}.${String(cs).padStart(2, '0')}`;
}

/**
 * The lanes whose light is red in a lapped race: the car has started its last
 * lap (or finished). A car is on lap `laps + 1` once it crossed the start line.
 */
export function finalLapLanes(cars: readonly CarResult[], laps: number): number[] {
  return cars.filter(c => c.status === 'finished'
    || (c.status === 'running' && c.laps >= laps - 1)).map(c => c.lane);
}
