/*
 * Ranking, gaps and intervals.
 *
 * Order:
 *   1. classified cars (running / finished / retired / crashed) before
 *      disqualified;
 *   2. more laps (counted + adjustments) first;
 *   3. cars that have not started yet last among the classified;
 *   4. on equal laps, the lower total time first: race time of the last
 *      counted crossing plus penalties (who got there first, after penalties);
 *   5. exact tie (same total to the µs, RACE_RULES.md): the faster best lap
 *      (a car with no timed lap is slower), then the lower lane number.
 * Retired and crashed cars keep their laps and rank among the others by the
 * same rule.
 * Gap = to the leader, interval = to the car ahead: laps behind, or ms on the
 * same lap.
 */

import type { CarResult, Standing } from '../types.ts';
import { roundUs } from './timeline.ts';

const classified = (c: CarResult): boolean => c.status !== 'disqualified';

export function compareCars(a: CarResult, b: CarResult): number {
  if (classified(a) !== classified(b)) return classified(a) ? -1 : 1;
  if (a.laps !== b.laps) return b.laps - a.laps;
  if ((a.status === 'grid') !== (b.status === 'grid')) return a.status === 'grid' ? 1 : -1;
  const ta = a.totalMs ?? Number.POSITIVE_INFINITY;
  const tb = b.totalMs ?? Number.POSITIVE_INFINITY;
  if (ta !== tb) return ta - tb;
  const ba = a.bestLapMs ?? Number.POSITIVE_INFINITY;
  const bb = b.bestLapMs ?? Number.POSITIVE_INFINITY;
  if (ba !== bb) return ba - bb;
  return a.lane - b.lane;
}

function diff(a: CarResult, b: CarResult): { laps: number; ms: number | null } {
  const laps = a.laps - b.laps;
  if (laps !== 0) return { laps, ms: null };
  return { laps: 0, ms: a.totalMs !== null && b.totalMs !== null ? roundUs(b.totalMs - a.totalMs) : null };
}

export function rank(cars: readonly CarResult[]): Standing[] {
  const sorted = [...cars].sort(compareCars);
  const leader = sorted[0];
  return sorted.map((c, i) => ({
    position: i + 1,
    lane: c.lane,
    driver: c.driver,
    status: c.status,
    laps: c.laps,
    totalMs: c.totalMs,
    gap: i === 0 || !leader ? null : diff(leader, c),
    interval: i === 0 ? null : diff(sorted[i - 1]!, c),
    bestLapMs: c.bestLapMs,
    lastLapMs: c.lastLapMs,
  }));
}
