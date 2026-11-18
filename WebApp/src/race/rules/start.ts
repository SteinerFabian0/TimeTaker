/*
 * The start: false starts and reaction times, judged against Tout from
 * EVT_LIGHTS_OUT (CONTROL guide §5.3: the device never reveals Tout early).
 *
 * - A crossing of the start procedure (see timeline) with tEntryUs < Tout is
 *   a FALSE START. Its reaction time is negative (crossing − Tout).
 *   * startsTiming: the false-start crossing is the car's start crossing
 *     (the car is past the line; lap 1 ends at its next crossing).
 *   * completesLap1: it is ignored for counting; laps run from Tout.
 *   A second crossing before Tout is rejected ('beforeStart').
 * - Otherwise the car's first crossing at or after Tout gives its reaction
 *   time: crossing − Tout. With the grid behind the line this includes the
 *   run-up to the line, so it is a "reaction + run-up" time.
 * - The false-start action: 'flag' (shown only), 'penalty' (penaltyMs added
 *   to the race time), 'disqualify'.
 */

import type { RaceConfig } from '../types.ts';

export type StartJudgement =
  | { readonly kind: 'falseStart'; readonly reactionMs: number; readonly isStartCrossing: boolean }
  | { readonly kind: 'beforeStart' }
  | { readonly kind: 'start'; readonly reactionMs: number; readonly isStartCrossing: boolean }
  | { readonly kind: 'race' };

/**
 * Judges a car's crossing in the start boot.
 * @param hadPreStart the car already crossed before Tout
 * @param hasStart the car has its start crossing (or, in completesLap1, any crossing after Tout)
 */
export function judgeStart(config: RaceConfig, tUs: number, toutUs: number, hadPreStart: boolean, hasStart: boolean): StartJudgement {
  const reactionMs = (tUs - toutUs) / 1000;
  const startsTiming = config.firstCrossing === 'startsTiming';
  if (tUs < toutUs) {
    if (hadPreStart) return { kind: 'beforeStart' };
    return { kind: 'falseStart', reactionMs, isStartCrossing: startsTiming };
  }
  if (!hasStart && !hadPreStart) return { kind: 'start', reactionMs, isStartCrossing: startsTiming };
  return { kind: 'race' };
}

export function falseStartPenaltyMs(config: RaceConfig): number {
  return config.falseStart.action === 'penalty' ? config.falseStart.penaltyMs : 0;
}
