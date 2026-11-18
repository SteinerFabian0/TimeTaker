/*
 * Race configuration: defaults and validation. The settings and their
 * defaults are the user's decisions in RACE_RULES.md (repo root):
 *
 *   main settings
 *     falseStart.action      'timePenalty'    flag | timePenalty | disqualify
 *     falseStart.penaltyMs   5000
 *     crashPolicy            'replacementAllowed'   | 'crashIsDnf'
 *   deeper settings
 *     falseStart.apply       'addToFirstLap'  | 'addToTotal'; 'stopAndGo' is a
 *                                             documented future value (power
 *                                             board) and is rejected for now
 *     minLapMs               20000            any faster lap is discarded, always
 *     startLights            2 attention pulses, rise 1000 ms, step 1000 ms,
 *                            hold 200–3000 ms (the director sends these)
 *   internal (not in RACE_RULES.md)
 *     rejectInHoldoff        true             the lane's own minimum-lap watch
 *     suspectFlags           NO_LOCK | UNALIGNED | EXTRAPOLATED
 */

import { LaneEventFlag } from '../../protocol/generated.ts';
import type { RaceConfig, StartLightsConfig } from '../types.ts';

export const DEFAULT_START_LIGHTS: StartLightsConfig = {
  attentionPulses: 2, pulseRiseMs: 1000, stepMs: 1000, holdMinMs: 200, holdMaxMs: 3000,
};

export const DEFAULT_RACE: Omit<RaceConfig, 'id' | 'entries'> = {
  name: 'Race',
  mode: 'laps',
  laps: 20,
  durationMs: 5 * 60_000,
  minLapMs: 20_000,
  rejectInHoldoff: true,
  falseStart: { action: 'timePenalty', penaltyMs: 5000, apply: 'addToFirstLap' },
  crashPolicy: 'replacementAllowed',
  startLights: DEFAULT_START_LIGHTS,
  suspectFlags: LaneEventFlag.NO_LOCK | LaneEventFlag.UNALIGNED | LaneEventFlag.EXTRAPOLATED,
};

export type RaceConfigInput = Partial<Omit<RaceConfig, 'falseStart' | 'startLights'>> & Pick<RaceConfig, 'id' | 'entries'>
  & { falseStart?: Partial<RaceConfig['falseStart']>; startLights?: Partial<StartLightsConfig> };

/** Fills defaults and checks the result; throws RangeError on nonsense. */
export function resolveConfig(input: RaceConfigInput): RaceConfig {
  const c: RaceConfig = {
    ...DEFAULT_RACE, ...input,
    falseStart: { ...DEFAULT_RACE.falseStart, ...(input.falseStart ?? {}) },
    startLights: { ...DEFAULT_START_LIGHTS, ...(input.startLights ?? {}) },
  };
  const problems = validateConfig(c);
  if (problems.length) throw new RangeError(`race config: ${problems.join('; ')}`);
  return c;
}

/** The START_SEQUENCE fields are u16 (attentionPulses u8). */
const nonNegInt = (x: number, max = 0xffff): boolean => Number.isInteger(x) && x >= 0 && x <= max;

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
  const fs = c.falseStart;
  if (!['flag', 'timePenalty', 'disqualify'].includes(fs.action)) p.push(`falseStart.action ${fs.action}`);
  if (!(fs.penaltyMs >= 0)) p.push('falseStart.penaltyMs must be >= 0');
  if (fs.apply === 'stopAndGo') p.push('falseStart.apply stopAndGo needs the power board (not supported yet)');
  else if (fs.apply !== 'addToFirstLap' && fs.apply !== 'addToTotal') p.push(`falseStart.apply ${String(fs.apply)}`);
  if (c.crashPolicy !== 'replacementAllowed' && c.crashPolicy !== 'crashIsDnf') p.push(`crashPolicy ${String(c.crashPolicy)}`);
  const sl = c.startLights;
  if (!nonNegInt(sl.attentionPulses, 0xff)) p.push('startLights.attentionPulses must be an integer 0..255');
  if (!(nonNegInt(sl.pulseRiseMs) && sl.pulseRiseMs > 0)) p.push('startLights.pulseRiseMs must be 1..65535');
  if (!(nonNegInt(sl.stepMs) && sl.stepMs > 0)) p.push('startLights.stepMs must be 1..65535');
  if (!(nonNegInt(sl.holdMinMs) && nonNegInt(sl.holdMaxMs) && sl.holdMinMs <= sl.holdMaxMs)) p.push('startLights hold range must be 0 <= holdMinMs <= holdMaxMs <= 65535');
  return p;
}
