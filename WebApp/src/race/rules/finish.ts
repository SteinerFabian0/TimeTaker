/*
 * The chequered flag.
 *
 * Laps race: the first car (in time order) whose lap count — counted laps
 *   plus manual adjustments — reaches config.laps takes the flag and
 *   finishes. From then on every other car finishes on its next counted
 *   crossing, whatever its lap count.
 * Time race: once a counted crossing happens at race time >= durationMs, the
 *   flag is out; that car and every other car finish on their next counted
 *   crossing at or after the time. (The time itself never finishes a car: a
 *   car is only classified at the line.)
 * After its finish a car's crossings are recorded as 'afterFinish'.
 * The race is finished when every car still running (not disqualified, not
 * retired, started) has finished, or an endRace action freezes it.
 */

import type { RaceConfig } from '../types.ts';

/** Does this counted crossing take the flag / finish the car? */
export function finishesCar(config: RaceConfig, flagOut: boolean, totalLaps: number, raceMs: number): boolean {
  if (flagOut) return true;
  if (config.mode === 'laps') return totalLaps >= config.laps;
  return raceMs >= config.durationMs;
}

/** The time race's flag is out by the clock even before anyone crosses (display only). */
export function timeIsUp(config: RaceConfig, raceMs: number | null): boolean {
  return config.mode === 'time' && raceMs !== null && raceMs >= config.durationMs;
}
