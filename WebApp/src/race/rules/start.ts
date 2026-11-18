/*
 * The start (RACE_RULES.md "Start", "False start"): judged against Tout from
 * EVT_LIGHTS_OUT (CONTROL guide §5.3: the device never reveals Tout early).
 *
 * - The grid is behind the line: a car's first crossing after lights out
 *   starts lap 1's timing; it is not a lap. Its reaction time is
 *   crossing − Tout (so it includes the run-up to the line).
 * - Crossings timed before the start sequence began (T0) never get here:
 *   they are grid positioning / practice (timeline).
 * - A crossing at or after T0 and before Tout is a FALSE START. It is the
 *   car's start crossing (the car is past the line; lap 1 ends at its next
 *   crossing); its reaction time is negative. A second crossing before Tout
 *   is rejected ('beforeStart').
 * - What happens (falseStart.action):
 *     flag         shown only
 *     timePenalty  penaltyMs, added to the first lap time (apply
 *                  'addToFirstLap', the default) or to the total only
 *                  ('addToTotal'); either way it is in the total time
 *   A false start never disqualifies (the host can still disqualify a car
 *   by hand, which is a separate race action).
 */

import type { RaceConfig } from '../types.ts';

export type StartJudgement =
  | { readonly kind: 'falseStart'; readonly reactionMs: number }
  | { readonly kind: 'beforeStart' }
  | { readonly kind: 'start'; readonly reactionMs: number }
  | { readonly kind: 'race' };

/**
 * Judges a car's crossing in the start boot.
 * @param hadPreStart the car already crossed before Tout
 * @param hasStart the car has its start crossing
 */
export function judgeStart(tUs: number, toutUs: number, hadPreStart: boolean, hasStart: boolean): StartJudgement {
  const reactionMs = (tUs - toutUs) / 1000;
  if (tUs < toutUs) {
    if (hadPreStart) return { kind: 'beforeStart' };
    return { kind: 'falseStart', reactionMs };
  }
  if (!hasStart && !hadPreStart) return { kind: 'start', reactionMs };
  return { kind: 'race' };
}

/** The false-start time penalty, ms (0 unless the action is timePenalty). */
export function falseStartPenaltyMs(config: RaceConfig): number {
  return config.falseStart.action === 'timePenalty' ? config.falseStart.penaltyMs : 0;
}

/** The part of the false-start penalty that goes into the first lap's time. */
export function firstLapPenaltyMs(config: RaceConfig): number {
  return config.falseStart.apply === 'addToFirstLap' ? falseStartPenaltyMs(config) : 0;
}
