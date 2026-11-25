/*
 * The phases of a Grand Prix and their settings for the evening (GUI_SPEC
 * §6.1b). Pure functions, DOM-free.
 *
 * A phase is one round of the tournament tree: the groups, the second-chance
 * races, the finals. All stages of a phase race the same settings (group A
 * and B alike, which is only fair). Before a phase starts the host may pick
 * another race preset for it (the overview's dropdowns, GUI_SPEC §6.1a) and
 * change its length (laps, or a qualifying's minutes) and how often each
 * driver drives each lane; the tournament's preset is the default, and only
 * what differs is stored. Race or qualifying comes from the preset.
 * Crash and false-start rules are not here: they stay fixed for the evening.
 */

import { LIMITS, type RacePreset } from '../presets/preset.ts';
import type { Stage, TournamentPlan } from '../tournaments/plan.ts';

export interface PhaseSettings {
  /** Sections the stages take turns in (src/tournaments/runOrder.ts); default 2. */
  readonly sections?: number;
  /** Another race preset than the tournament's (id); the race fields below change that one. */
  readonly presetId?: string;
  readonly laps?: number;
  readonly durationMs?: number;
  readonly racesPerLane?: number;
}

export const DEFAULT_SECTIONS = 2;

export type PhaseKind = 'groups' | 'second' | 'finals';

export interface Phase {
  /** The tree's column (1 = first round). */
  readonly column: number;
  readonly kind: PhaseKind;
  readonly stages: readonly Stage[];
}

/** The phases of a plan, in running order. */
export function phasesOf(plan: TournamentPlan): Phase[] {
  const cols = [...new Set(plan.stages.map(s => s.column))].sort((a, b) => a - b);
  return cols.map(column => {
    const stages = plan.stages.filter(s => s.column === column);
    const kind: PhaseKind = stages.some(s => s.kind === 'final' || s.kind === 'smallFinal') ? 'finals'
      : stages.some(s => s.kind === 'repechage') ? 'second' : 'groups';
    return { column, kind, stages };
  });
}

const clampInt = (v: unknown, min: number, max: number): number | undefined =>
  typeof v === 'number' && Number.isFinite(v) ? Math.min(max, Math.max(min, Math.round(v))) : undefined;

/** The preset a phase races: the template's, with the evening's changes (clamped to the editor's limits). */
export function applyPhase(p: RacePreset, ph: PhaseSettings | undefined): RacePreset {
  if (!ph) return p;
  const minutes = clampInt(ph.durationMs !== undefined ? ph.durationMs / 60_000 : undefined, LIMITS.minutes.min, LIMITS.minutes.max);
  return {
    ...p,
    laps: clampInt(ph.laps, LIMITS.laps.min, LIMITS.laps.max) ?? p.laps,
    durationMs: minutes !== undefined ? minutes * 60_000 : p.durationMs,
    racesPerLane: clampInt(ph.racesPerLane, LIMITS.racesPerLane.min, LIMITS.racesPerLane.max) ?? p.racesPerLane,
  };
}

/**
 * Sets one field of a phase's settings; a value equal to the preset's (or
 * the default sections) is dropped, so "changed" always means "differs".
 */
export function setPhaseField<K extends keyof PhaseSettings>(ph: PhaseSettings | undefined, p: RacePreset, key: K, value: PhaseSettings[K]): PhaseSettings {
  const next: Record<string, unknown> = { ...ph, [key]: value };
  const base: Record<string, unknown> = { laps: p.laps, durationMs: p.durationMs, racesPerLane: p.racesPerLane, sections: DEFAULT_SECTIONS };
  for (const k of Object.keys(next)) if (next[k] === undefined || next[k] === base[k]) delete next[k];
  return next as PhaseSettings;
}

/** Which race settings of a phase differ from the preset. */
export function changedFields(p: RacePreset, ph: PhaseSettings | undefined): Set<'laps' | 'durationMs' | 'racesPerLane'> {
  const e = applyPhase(p, ph);
  const out = new Set<'laps' | 'durationMs' | 'racesPerLane'>();
  for (const k of ['laps', 'durationMs', 'racesPerLane'] as const) if (e[k] !== p[k]) out.add(k);
  return out;
}

/** The preset a phase races before its own changes: the evening's pick, else the tournament's (`templateId`). */
export function phasePresetId(ph: PhaseSettings | undefined, templateId: string): string {
  return ph?.presetId ?? templateId;
}

/**
 * Picks a race preset for a phase. The phase's changes to the old preset's
 * laps and races per lane are dropped (they belonged to it); the sections
 * stay. Picking the tournament's preset again stores no pick.
 */
export function choosePreset(ph: PhaseSettings | undefined, templateId: string, presetId: string): PhaseSettings {
  return {
    ...(ph?.sections !== undefined ? { sections: ph.sections } : {}),
    ...(presetId !== templateId ? { presetId } : {}),
  };
}
