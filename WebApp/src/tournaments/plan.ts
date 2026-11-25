/*
 * The tree of a tournament for N drivers on L lanes (GUI_SPEC §5.4): pure
 * functions, DOM-free. planTournament() turns a template's rules into
 * stages with seats and the routes between them; the Tournaments page shows
 * it as the bracket preview, Grand Prix will run it.
 *
 * Columns are rounds, left to right: 0 the field (entry), then groups,
 * repechage, final. Every seat of a stage says where its driver comes from:
 * a seed of the field, or a rank of an earlier stage.
 *
 * "Groups → Repechage → Final" (§5.4):
 *   1. G = ceil(N / L) groups, sizes as even as possible (snake by seed).
 *   2. The final has L seats; R of them are for repechage winners.
 *   3. Each group sends (L − R) / G straight to the final.
 *   4. The rest race the repechage: M = ceil(rest / L) groups, crossed over
 *      by rank (snake); R / M winners of each go to the final.
 *
 * Options for one evening (the Grand Prix, not the template):
 *   finalSeats  a final with F < L seats: planned as above with F in place
 *               of L; its last seats go to the small final instead
 *   smallFinal  the next best race a small final: the places right after
 *               the final's from the round that feeds it, by rank, at most L
 */

import type { TournamentTemplate } from './template.ts';

export type StageKind = 'group' | 'repechage' | 'final' | 'smallFinal';

export interface PlanOptions {
  /** Seats in the final (2 … lanes); default the lanes. */
  readonly finalSeats?: number;
  /** Race a small final for the next best. */
  readonly smallFinal?: boolean;
}

export type Source =
  | { readonly kind: 'seed'; readonly seed: number }                       // 1-based
  | { readonly kind: 'rank'; readonly stage: string; readonly rank: number };

export interface Stage {
  readonly id: string;
  readonly kind: StageKind;
  /** 0-based within its kind: group A = 0, B = 1 … */
  readonly index: number;
  readonly column: number;
  readonly presetId: string;
  readonly seats: readonly Source[];
}

/** Drivers moving from the field or a stage to a stage; `ranks` empty for the field. */
export interface Route {
  readonly from: string;           // 'entry' or a stage id
  readonly to: string;
  readonly ranks: readonly number[];
}

export type PlanProblem =
  | { readonly code: 'tooFewDrivers'; readonly level: 'error' }
  | { readonly code: 'singleTooBig'; readonly level: 'error'; readonly drivers: number; readonly lanes: number }
  | { readonly code: 'tooManyGroups'; readonly level: 'error'; readonly groups: number; readonly lanes: number }
  | { readonly code: 'noRepechage'; readonly level: 'error'; readonly groups: number; readonly lanes: number }
  | { readonly code: 'repechageSeats'; readonly level: 'warn'; readonly wanted: number; readonly used: number; readonly valid: readonly number[] }
  | { readonly code: 'repechageTooBig'; readonly level: 'error'; readonly rest: number; readonly seats: number }
  | { readonly code: 'stageTooBig'; readonly level: 'error'; readonly stage: string; readonly size: number; readonly lanes: number }
  | { readonly code: 'routedTwice'; readonly level: 'error'; readonly stage: string; readonly rank: number }
  | { readonly code: 'finalEmptySeats'; readonly level: 'warn'; readonly seats: number; readonly lanes: number };

export interface TournamentPlan {
  readonly drivers: number;
  readonly lanes: number;
  readonly stages: readonly Stage[];
  readonly routes: readonly Route[];
  /** The stage whose result is the event result; null when nothing could be planned. */
  readonly finalStage: string | null;
  /** Seats the final has (the lanes unless the evening chose fewer). */
  readonly finalSeats: number;
  /**
   * Drivers who could race a small final (the next best, at most the lanes);
   * 0 when the format has none (one race, or the group is the final).
   */
  readonly smallFinalSize: number;
  /** Final seats for repechage winners actually used (0 without a repechage). */
  readonly repechageSeats: number;
  /** True when the field is small enough that the only stage is the final. */
  readonly groupIsFinal: boolean;
  readonly problems: readonly PlanProblem[];
}

// ---- helpers ------------------------------------------------------------------------

/**
 * Deals items into k piles in snake order (1 2 … k k … 2 1 1 2 …), so the
 * first items (the strongest seeds, the best ranks) spread evenly and the
 * piles' sizes differ by at most one.
 */
export function snakeDeal<T>(items: readonly T[], k: number): T[][] {
  const piles: T[][] = Array.from({ length: Math.max(1, k) }, () => []);
  items.forEach((x, i) => {
    const round = Math.floor(i / piles.length), pos = i % piles.length;
    piles[round % 2 === 0 ? pos : piles.length - 1 - pos].push(x);
  });
  return piles;
}

/**
 * Final seats for repechage winners that let all G groups send the same
 * number straight to an L-seat final: R with (L − R) divisible by G, at
 * least one direct seat per group, 1 ≤ R < L. Ascending.
 */
export function validRepechageSeats(lanes: number, groups: number): number[] {
  const out: number[] = [];
  for (let r = 1; r < lanes; r++) if ((lanes - r) % groups === 0 && (lanes - r) / groups >= 1) out.push(r);
  return out;
}

/** The 'auto' choice: the valid R nearest to a third of the lanes (the smaller on a tie); null if none. */
export function autoRepechageSeats(lanes: number, groups: number): number | null {
  const valid = validRepechageSeats(lanes, groups);
  if (!valid.length) return null;
  return valid.reduce((best, r) => Math.abs(r - lanes / 3) < Math.abs(best - lanes / 3) ? r : best);
}

const rankSeats = (stages: readonly Stage[], perStage: number, firstRank = 1): Source[] => {
  const out: Source[] = [];
  for (let r = firstRank; r < firstRank + perStage; r++)
    for (const s of stages) if (r <= s.seats.length) out.push({ kind: 'rank', stage: s.id, rank: r });
  return out;
};

function groupsOf(t: TournamentTemplate, drivers: number, count: number): Stage[] {
  const seeds = Array.from({ length: drivers }, (_, i) => i + 1);
  // A random split is drawn when the event starts; the preview deals by seed either way.
  return snakeDeal(seeds, count).map((pile, i): Stage => ({
    id: `G${i + 1}`, kind: 'group', index: i, column: 1, presetId: t.groupPreset,
    seats: pile.map(seed => ({ kind: 'seed', seed })),
  }));
}

function finalOf(t: TournamentTemplate, column: number, seats: Source[]): Stage {
  return { id: 'F', kind: 'final', index: 0, column, presetId: t.finalPreset, seats };
}

/** Routes derived from the seats: one per (source stage → stage), ranks ascending. */
function routesOf(stages: readonly Stage[]): Route[] {
  const out: Route[] = [];
  for (const s of stages) {
    if (s.seats.some(x => x.kind === 'seed')) out.push({ from: 'entry', to: s.id, ranks: [] });
    const by = new Map<string, number[]>();
    for (const x of s.seats) if (x.kind === 'rank') by.set(x.stage, [...(by.get(x.stage) ?? []), x.rank]);
    for (const [from, ranks] of by) out.push({ from, to: s.id, ranks: ranks.sort((a, b) => a - b) });
  }
  return out;
}

/** Problems any plan can have, whatever made it: too big a stage, a rank sent twice, a final with empty seats. */
export function checkPlan(stages: readonly Stage[], lanes: number, drivers: number, finalSeats = lanes): PlanProblem[] {
  const out: PlanProblem[] = [];
  for (const s of stages) if (s.seats.length > lanes) out.push({ code: 'stageTooBig', level: 'error', stage: s.id, size: s.seats.length, lanes });
  const seen = new Set<string>();
  for (const s of stages) for (const x of s.seats) {
    const key = x.kind === 'seed' ? `seed:${x.seed}` : `${x.stage}:${x.rank}`;
    if (seen.has(key)) out.push({ code: 'routedTwice', level: 'error', stage: x.kind === 'rank' ? x.stage : 'entry', rank: x.kind === 'rank' ? x.rank : x.seed });
    seen.add(key);
  }
  const final = stages.find(s => s.kind === 'final');
  if (final && final.seats.length < Math.min(finalSeats, drivers))
    out.push({ code: 'finalEmptySeats', level: 'warn', seats: final.seats.length, lanes });
  return out;
}

// ---- the plan -------------------------------------------------------------------------

export function planTournament(t: TournamentTemplate, drivers: number, lanes: number, opts: PlanOptions = {}): TournamentPlan {
  const F = Math.min(lanes, Math.max(2, Math.round(opts.finalSeats ?? lanes)));
  const empty = (problems: PlanProblem[]): TournamentPlan =>
    ({ drivers, lanes, stages: [], routes: [], finalStage: null, finalSeats: F, smallFinalSize: 0, repechageSeats: 0, groupIsFinal: false, problems });
  /** With a small final for the next best from `feeders` (the round before the final), each sending its first `sent` places on. */
  const done = (stages: Stage[], repechageSeats: number, problems: PlanProblem[] = [], feeders: Stage[] = [], sent = 0): TournamentPlan => {
    const rest = feeders.length ? rankSeats(feeders, Math.max(...feeders.map(f => f.seats.length)) - sent, sent + 1).slice(0, lanes) : [];
    const small = opts.smallFinal && rest.length >= 2
      ? [{ id: 'K', kind: 'smallFinal', index: 0, column: stages[stages.length - 1].column, presetId: t.finalPreset, seats: rest } as Stage]
      : [];
    const all = [...stages, ...small];
    return {
      drivers, lanes, stages: all, routes: routesOf(all), finalStage: all.find(s => s.kind === 'final')?.id ?? null,
      finalSeats: F, smallFinalSize: rest.length >= 2 ? rest.length : 0,
      repechageSeats, groupIsFinal: all.length === 1, problems: [...problems, ...checkPlan(all, lanes, drivers, F)],
    };
  };

  if (!Number.isInteger(drivers) || drivers < 2) return empty([{ code: 'tooFewDrivers', level: 'error' }]);
  const seedsAll: Source[] = Array.from({ length: drivers }, (_, i) => ({ kind: 'seed', seed: i + 1 }));

  if (t.format === 'single') {
    if (drivers > lanes) return empty([{ code: 'singleTooBig', level: 'error', drivers, lanes }]);
    return done([finalOf(t, 1, seedsAll)], 0);
  }

  const G = Math.ceil(drivers / lanes);
  if (G === 1) return done([finalOf(t, 1, seedsAll)], 0);            // the group is the final
  if (G > lanes) return empty([{ code: 'tooManyGroups', level: 'error', groups: G, lanes }]);
  const groups = groupsOf(t, drivers, G);
  const smallest = Math.min(...groups.map(g => g.seats.length));

  if (t.format === 'groupsFinal') {
    const q = Math.min(Math.floor(F / G), smallest);
    return done([...groups, finalOf(t, 2, rankSeats(groups, q))], 0, [], groups, q);
  }

  // groupsRepechageFinal
  const problems: PlanProblem[] = [];
  const valid = validRepechageSeats(F, G);
  const auto = autoRepechageSeats(F, G);
  if (auto === null) return empty([{ code: 'noRepechage', level: 'error', groups: G, lanes: F }]);
  /** The repechage for R final seats, or null if it does not work out. */
  const repechage = (R: number) => {
    const q = (F - R) / G;
    const pool: Source[] = rankSeats(groups, Math.max(...groups.map(g => g.seats.length)) - q, q + 1);
    // Repechage groups: as few as fit on the track, each sending the same number of
    // winners and keeping at least one driver more than it sends.
    let M = Math.max(1, Math.ceil(pool.length / lanes));
    while (M <= R && (R % M !== 0 || Math.floor(pool.length / M) <= R / M)) M++;
    const ok = q < smallest && M <= R && Math.ceil(pool.length / M) <= lanes;
    return { R, q, pool, M, ok };
  };
  let r = repechage(auto);
  if (t.repechageSeats !== 'auto') {
    if (valid.includes(t.repechageSeats)) r = repechage(t.repechageSeats);
    else problems.push({ code: 'repechageSeats', level: 'warn', wanted: t.repechageSeats, used: auto, valid });
  } else if (!r.ok) {
    // 'auto': the next valid choice by closeness to a third of the final that works.
    const tries = [...valid].sort((a, b) => Math.abs(a - F / 3) - Math.abs(b - F / 3) || a - b).map(repechage);
    r = tries.find(x => x.ok) ?? r;
  }
  if (!r.ok) return empty([...problems, { code: 'repechageTooBig', level: 'error', rest: r.pool.length, seats: r.R }]);

  const reps = snakeDeal(r.pool, r.M).map((seats, i): Stage => ({
    id: `R${i + 1}`, kind: 'repechage', index: i, column: 2, presetId: t.repechagePreset, seats,
  }));
  const final = finalOf(t, 3, [...rankSeats(groups, r.q), ...rankSeats(reps, r.R / r.M)]);
  return done([...groups, ...reps, final], r.R, problems, reps, r.R / r.M);
}

// ---- totals ---------------------------------------------------------------------------

export interface StageEstimate { readonly races: number; readonly ms: number }

/**
 * Races and estimated time of every stage and in all, with a function that
 * gives one stage's (from its preset and size; src/presets/preset.ts has
 * racesFor and groupEstimateMs). Stages run one after another: one track.
 */
export function planTotals(plan: TournamentPlan, stageEstimate: (s: Stage) => StageEstimate):
  { perStage: Map<string, StageEstimate>; races: number; ms: number } {
  const perStage = new Map(plan.stages.map(s => [s.id, stageEstimate(s)]));
  let races = 0, ms = 0;
  for (const e of perStage.values()) { races += e.races; ms += e.ms; }
  return { perStage, races, ms };
}
