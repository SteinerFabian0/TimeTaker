/*
 * Race rules (GUI_SPEC §5.2a): crashes, the result basis and false starts.
 * They belong to a tournament template, not to a race preset, so they stay
 * the same for every race of an evening (the players would be confused if
 * a false start cost 5 s in the groups and only a flag in the final). Pure
 * data and functions, DOM-free.
 */

import type { CrashPolicy, FalseStartAction } from '../race/types.ts';

/**
 * Who is last when cars crash out (crash policy 'crashIsDnf' only):
 * distance   by how far each car got (needs the Power Pack)
 * crashOrder the first car out is last
 */
export type ResultBasis = 'distance' | 'crashOrder';

export interface RaceRules {
  readonly crashPolicy: CrashPolicy;
  readonly resultBasis: ResultBasis;
  readonly falseStart: {
    readonly action: FalseStartAction;
    readonly penaltyMs: number;
    readonly apply: 'addToFirstLap' | 'addToTotal';
  };
}

export const RULE_LIMITS = { penaltySeconds: { min: 0, max: 60 } } as const;

export const DEFAULT_RULES: RaceRules = {
  crashPolicy: 'crashIsDnf',
  resultBasis: 'crashOrder',
  falseStart: { action: 'timePenalty', penaltyMs: 5000, apply: 'addToFirstLap' },
};

const pick = <T extends string>(v: unknown, allowed: readonly T[], fallback: T): T =>
  allowed.includes(v as T) ? v as T : fallback;

/** Reads stored rules back into valid ones; missing or bad fields take the defaults. */
export function normalizeRules(raw: unknown): RaceRules {
  const r = (raw && typeof raw === 'object' ? raw : {}) as Record<string, any>;
  const fs = (r.falseStart && typeof r.falseStart === 'object') ? r.falseStart : {};
  const d = DEFAULT_RULES;
  const pen = typeof fs.penaltyMs === 'number' && Number.isFinite(fs.penaltyMs)
    ? Math.min(RULE_LIMITS.penaltySeconds.max * 1000, Math.max(RULE_LIMITS.penaltySeconds.min * 1000, fs.penaltyMs))
    : d.falseStart.penaltyMs;
  return {
    crashPolicy: pick(r.crashPolicy, ['crashIsDnf', 'replacementAllowed'] as const, d.crashPolicy),
    resultBasis: pick(r.resultBasis, ['distance', 'crashOrder'] as const, d.resultBasis),
    falseStart: {
      action: pick(fs.action, ['flag', 'timePenalty'] as const, d.falseStart.action),
      penaltyMs: pen,
      apply: pick(fs.apply, ['addToFirstLap', 'addToTotal'] as const, d.falseStart.apply),
    },
  };
}

/**
 * The result basis that actually applies: 'distance' needs the Power Pack
 * (GUI_SPEC §4.3, §9) and only matters when a crash puts the car out.
 */
export function effectiveResultBasis(r: Pick<RaceRules, 'crashPolicy' | 'resultBasis'>, powerPack: boolean): ResultBasis | null {
  if (r.crashPolicy !== 'crashIsDnf') return null;
  return r.resultBasis === 'distance' && !powerPack ? 'crashOrder' : r.resultBasis;
}
