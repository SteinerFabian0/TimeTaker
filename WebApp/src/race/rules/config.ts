/*
 * Race configuration: defaults and validation.
 *
 * Defaults (each is a decision for the user to review):
 *   firstCrossing          'startsTiming'   grid behind the line
 *   minLapMs               2000             no real lap is faster; a marshal
 *                                           re-slotting a car behind the line
 *                                           makes a crossing 1.5–3.5 s later
 *   rejectInHoldoff        true             the lane's own minimum-lap watch
 *   blockedCountsAsCrossing true            a car stopped ON the line did cross it
 *   falseStart.action      'flag'           shown, no automatic penalty
 *   falseStart.penaltyMs   5000             used with action 'penalty'
 *   suspectFlags           NO_LOCK | UNALIGNED | EXTRAPOLATED
 */

import { LaneEventFlag } from '../../protocol/generated.ts';
import type { RaceConfig } from '../types.ts';

export const DEFAULT_RACE: Omit<RaceConfig, 'id' | 'entries'> = {
  name: 'Race',
  mode: 'laps',
  laps: 20,
  durationMs: 5 * 60_000,
  firstCrossing: 'startsTiming',
  minLapMs: 2000,
  rejectInHoldoff: true,
  blockedCountsAsCrossing: true,
  falseStart: { action: 'flag', penaltyMs: 5000 },
  suspectFlags: LaneEventFlag.NO_LOCK | LaneEventFlag.UNALIGNED | LaneEventFlag.EXTRAPOLATED,
};

export type RaceConfigInput = Partial<Omit<RaceConfig, 'falseStart'>> & Pick<RaceConfig, 'id' | 'entries'>
  & { falseStart?: Partial<RaceConfig['falseStart']> };

/** Fills defaults and checks the result; throws RangeError on nonsense. */
export function resolveConfig(input: RaceConfigInput): RaceConfig {
  const c: RaceConfig = {
    ...DEFAULT_RACE, ...input,
    falseStart: { ...DEFAULT_RACE.falseStart, ...(input.falseStart ?? {}) },
  };
  const problems = validateConfig(c);
  if (problems.length) throw new RangeError(`race config: ${problems.join('; ')}`);
  return c;
}

export function validateConfig(c: RaceConfig): string[] {
  const p: string[] = [];
  if (!c.id) p.push('id missing');
  if (c.mode !== 'laps' && c.mode !== 'time') p.push(`mode ${String(c.mode)}`);
  if (c.mode === 'laps' && !(Number.isInteger(c.laps) && c.laps >= 1)) p.push('laps must be >= 1');
  if (c.mode === 'time' && !(c.durationMs > 0)) p.push('durationMs must be > 0');
  if (c.entries.length === 0) p.push('no entries');
  const lanes = new Set<number>();
  for (const e of c.entries) {
    if (!Number.isInteger(e.lane) || e.lane < 1 || e.lane > 6) p.push(`lane ${e.lane} is not 1..6`);
    if (lanes.has(e.lane)) p.push(`lane ${e.lane} twice`);
    lanes.add(e.lane);
  }
  if (!(c.minLapMs >= 0)) p.push('minLapMs must be >= 0');
  if (!['flag', 'penalty', 'disqualify'].includes(c.falseStart.action)) p.push(`falseStart.action ${c.falseStart.action}`);
  if (c.firstCrossing !== 'startsTiming' && c.firstCrossing !== 'completesLap1') p.push(`firstCrossing ${String(c.firstCrossing)}`);
  return p;
}
