/*
 * The order of the evening (GUI_SPEC §6.1a): which block of races (a
 * "section") runs when. Pure functions, DOM-free.
 *
 * Rounds run left to right. Within a round that has several stages (groups
 * A and B, two second-chance races, final and small final) the stages can
 * take turns: with 2 sections, group A runs its first half, then group B its
 * first half, then A's second half, then B's — so nobody sits through one
 * long block. A section ends on a lane change (with block rotation every
 * driver has driven whole lanes), and a round with a single stage runs in one
 * go. In the finals round the small final comes first, the final last.
 */

import type { Stage, TournamentPlan } from './plan.ts';

export const SECTION_CHOICES = [1, 2, 3] as const;

export interface Section {
  readonly stage: string;
  /** 0-based part of its stage, and how many parts the stage has. */
  readonly part: number;
  readonly parts: number;
  /** Races in this section, and the stage's race index it starts at (0-based). */
  readonly races: number;
  readonly start: number;
}

/**
 * Splits `races` into `sections` parts that end on a multiple of `unit`
 * (races per lane change), as even as possible, earlier parts the bigger.
 */
export function splitRaces(races: number, unit: number, sections: number): number[] {
  if (races <= 0) return [];
  const u = Math.max(1, unit);
  const units = Math.ceil(races / u);
  const n = Math.max(1, Math.min(sections, units));
  const base = Math.floor(units / n), extra = units % n;
  const out: number[] = [];
  let left = races;
  for (let i = 0; i < n; i++) {
    const take = i === n - 1 ? left : Math.min(left, (base + (i < extra ? 1 : 0)) * u);
    out.push(take);
    left -= take;
  }
  return out;
}

/**
 * The sections in running order. `racesOf(s)` and `unitOf(s)`: a stage's
 * races and races per lane change (src/presets: racesFor, and racesPerLane
 * for block rotation or 1 for every-race rotation).
 */
export function runOrder(plan: TournamentPlan, sections: number, racesOf: (s: Stage) => number, unitOf: (s: Stage) => number): Section[] {
  const out: Section[] = [];
  const columns = [...new Set(plan.stages.map(s => s.column))].sort((a, b) => a - b);
  for (const c of columns) {
    const stages = plan.stages.filter(s => s.column === c)
      .sort((a, b) => Number(a.kind === 'final') - Number(b.kind === 'final'));   // the final last
    const n = stages.length > 1 ? sections : 1;
    const parts = stages.map(s => splitRaces(racesOf(s), unitOf(s), n));
    const most = Math.max(0, ...parts.map(p => p.length));
    for (let i = 0; i < most; i++)
      stages.forEach((s, j) => {
        const p = parts[j];
        if (i >= p.length) return;
        out.push({ stage: s.id, part: i, parts: p.length, races: p[i], start: p.slice(0, i).reduce((a, b) => a + b, 0) });
      });
  }
  return out;
}
