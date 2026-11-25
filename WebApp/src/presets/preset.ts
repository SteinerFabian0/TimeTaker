/*
 * Race presets (GUI_SPEC §4): plain data and pure functions. DOM-free.
 *
 * A preset is the recipe for the races of one stage: how long a race is, how
 * often each driver drives each used lane, and how places become points.
 * Crashes and false starts are not here: they are the tournament's rules
 * (src/presets/rules.ts), the same for the whole evening. The race engine's
 * per-race settings (RaceConfig) come from the preset, the rules and the
 * track's timing (start lights, minimum lap time) with toRaceConfig().
 *
 * Every stored preset carries schemaVersion; normalizePreset() reads any
 * stored shape (older or hand-edited) back into a valid current one.
 */

import type { RaceConfig, RaceEntry } from '../race/types.ts';
import type { RaceRules } from './rules.ts';
import type { RaceConfigInput } from '../race/rules/config.ts';
import { trackRaceConfig, type TrackTiming } from '../track/trackTiming.ts';

export const PRESET_SCHEMA_VERSION = 2;

/**
 * The three kinds of preset (GUI_SPEC §4.1, decided 2026-11-24). Fixed when
 * a preset is made (a duplicate keeps it); listed apart, never interchangeable:
 * qualifying  timed sessions (`durationMs`), rotating over the lanes like
 *             races; each session ranked by fastest lap, points after it
 * race        ends after `laps`; first to finish wins
 * tiebreaker  a race among the tied drivers that only orders them (no
 *             points); one, shipped; runs only when a tournament breaks
 *             ties with a race
 */
export type PresetKind = 'qualifying' | 'race' | 'tiebreaker';
export const PRESET_KINDS: readonly PresetKind[] = ['qualifying', 'race', 'tiebreaker'];

/** Places that score: one per lane (tracks have up to 6 lanes). */
export const MAX_PLACES = 6;

/**
 * block      k races on a lane, then every driver moves on (the reference evening)
 * everyRace  drivers move to the next lane after every race
 */
export type Rotation = 'block' | 'everyRace';

export interface RacePreset {
  readonly schemaVersion: number;
  readonly id: string;
  readonly name: string;
  /** Shipped with the app: editable, can be reset to its defaults, never deleted. */
  readonly builtIn: boolean;
  readonly kind: PresetKind;
  /** Follows the kind: a qualifying runs on time, races and tiebreakers on laps. */
  readonly mode: 'laps' | 'time';
  readonly laps: number;
  /**
   * Untimed laps before the race. Stored with the preset; how the race
   * handling runs them is decided later (not passed to the race engine yet).
   */
  readonly warmupLaps: number;
  readonly durationMs: number;
  /** k: each driver drives every used lane k times. */
  readonly racesPerLane: number;
  readonly rotation: Rotation;
  /** Points for 1st … 6th place (always MAX_PLACES long). */
  readonly points: readonly number[];
}

// ---- limits (the editor's steppers use the same ones) --------------------------

export const LIMITS = {
  laps: { min: 1, max: 99 },
  warmupLaps: { min: 0, max: 9 },
  minutes: { min: 1, max: 120 },
  racesPerLane: { min: 1, max: 9 },
  points: { min: 0, max: 99 },
  drivers: { min: 2, max: 6 },
} as const;

// ---- points tables ----------------------------------------------------------------

export const POINTS_TABLES = {
  linear: [6, 5, 4, 3, 2, 1],
  wide: [10, 7, 5, 3, 1, 0],
  f1: [25, 18, 15, 12, 10, 8],
} as const satisfies Record<string, readonly number[]>;

export type PointsTableId = keyof typeof POINTS_TABLES;

/** Which named table `points` equals, or null for a custom one. */
export function pointsTableOf(points: readonly number[]): PointsTableId | null {
  for (const [id, t] of Object.entries(POINTS_TABLES) as [PointsTableId, readonly number[]][])
    if (t.length === points.length && t.every((v, i) => v === points[i])) return id;
  return null;
}

/** Points for a place (1-based) in a field of `fieldSize`; places beyond the field are unused (GUI_SPEC §4.4). */
export function pointsFor(preset: Pick<RacePreset, 'points'>, place: number, fieldSize = MAX_PLACES): number {
  if (!Number.isInteger(place) || place < 1 || place > Math.min(fieldSize, preset.points.length)) return 0;
  return preset.points[place - 1];
}

// ---- construction -----------------------------------------------------------------

const BASE: Omit<RacePreset, 'id' | 'name' | 'builtIn'> = {
  schemaVersion: PRESET_SCHEMA_VERSION,
  kind: 'race',
  mode: 'laps',
  laps: 5,
  warmupLaps: 1,
  durationMs: 5 * 60_000,
  racesPerLane: 1,
  rotation: 'block',
  points: POINTS_TABLES.linear,
};

/** Ids of the shipped presets; stable, so templates can refer to them. */
export const BUILTIN_IDS = {
  short: 'builtin-short', default: 'builtin-default', qualifying: 'builtin-qualifying', tiebreaker: 'builtin-tiebreaker',
} as const;

export const isQualifying = (p: Pick<RacePreset, 'kind'>): boolean => p.kind === 'qualifying';

const modeOf = (kind: PresetKind): RacePreset['mode'] => kind === 'qualifying' ? 'time' : 'laps';

/**
 * The tiebreaker preset is special (GUI_SPEC §4.1): it only runs when a
 * tournament breaks a tie with a race, so it is never offered as a stage's
 * preset and cannot be duplicated. It stays editable.
 */
export const isTiebreaker = (id: string): boolean => id === BUILTIN_IDS.tiebreaker;

/**
 * The shipped presets (GUI_SPEC §1, §4.1, §5.2), in their default list
 * order: the reference evening's group race (A), the losers-bracket race
 * (B), a qualifying, and the tiebreaker (listed apart, §4.1). The host may edit them and reset them to these values. The app
 * translates a name while it is still the shipped (English) one.
 */
export const BUILTIN_PRESETS: readonly RacePreset[] = [
  { ...BASE, id: BUILTIN_IDS.default, name: 'Default race', builtIn: true, racesPerLane: 3, laps: 5, warmupLaps: 2 },
  { ...BASE, id: BUILTIN_IDS.short, name: 'Short race', builtIn: true, racesPerLane: 2, laps: 3, warmupLaps: 1 },
  { ...BASE, id: BUILTIN_IDS.qualifying, name: 'Qualifying', builtIn: true, kind: 'qualifying', mode: 'time', durationMs: 5 * 60_000,
    racesPerLane: 1, warmupLaps: 2, points: POINTS_TABLES.wide },
  { ...BASE, id: BUILTIN_IDS.tiebreaker, name: 'Tiebreaker', builtIn: true, kind: 'tiebreaker', racesPerLane: 1, laps: 4, warmupLaps: 1 },
];

export function builtInDefault(id: string): RacePreset | undefined {
  return BUILTIN_PRESETS.find(p => p.id === id);
}

/** True when a shipped preset still has all its shipped values. */
export function isAtDefaults(p: RacePreset): boolean {
  const d = builtInDefault(p.id);
  return !!d && JSON.stringify(normalizePreset(p)) === JSON.stringify(normalizePreset(d));
}

/** A new preset of a kind (qualifying or race; there is only the one tiebreaker). */
export function newPreset(id: string, name: string, kind: Exclude<PresetKind, 'tiebreaker'> = 'race'): RacePreset {
  return kind === 'qualifying'
    ? { ...BASE, id, name, builtIn: false, kind, mode: 'time', racesPerLane: 1, warmupLaps: 2, points: POINTS_TABLES.wide }
    : { ...BASE, id, name, builtIn: false };
}

/** An editable copy (built-in presets are duplicated to edit). */
export function duplicatePreset(p: RacePreset, id: string, name: string): RacePreset {
  return { ...p, id, name, builtIn: false, schemaVersion: PRESET_SCHEMA_VERSION };
}

/**
 * `base`, or `base N` with the smallest N ≥ 2 that no preset in `taken` uses
 * (case-insensitive, trimmed).
 */
export function uniqueName(base: string, taken: Iterable<string>): string {
  const used = new Set([...taken].map(n => n.trim().toLocaleLowerCase()));
  const b = base.trim();
  if (!used.has(b.toLocaleLowerCase())) return b;
  for (let n = 2; ; n++) if (!used.has(`${b} ${n}`.toLocaleLowerCase())) return `${b} ${n}`;
}

// ---- validation and normalization ---------------------------------------------------

export type PresetProblem =
  | 'nameEmpty' | 'nameTaken' | 'laps' | 'warmupLaps' | 'duration' | 'racesPerLane' | 'points' | 'enum';

const inRange = (x: unknown, min: number, max: number): boolean =>
  typeof x === 'number' && Number.isFinite(x) && x >= min && x <= max;
const intIn = (x: unknown, min: number, max: number): boolean => Number.isInteger(x) && inRange(x, min, max);

/** Everything wrong with a preset; [] when it is valid. `others`: the other presets (for unique names). */
export function validatePreset(p: RacePreset, others: readonly RacePreset[] = []): PresetProblem[] {
  const out: PresetProblem[] = [];
  const name = p.name.trim();
  if (!name) out.push('nameEmpty');
  else if (others.some(o => o.id !== p.id && o.name.trim().toLocaleLowerCase() === name.toLocaleLowerCase())) out.push('nameTaken');
  if (!PRESET_KINDS.includes(p.kind) || p.mode !== modeOf(p.kind) || !['block', 'everyRace'].includes(p.rotation)) out.push('enum');
  if (!intIn(p.laps, LIMITS.laps.min, LIMITS.laps.max)) out.push('laps');
  if (!intIn(p.warmupLaps, LIMITS.warmupLaps.min, LIMITS.warmupLaps.max)) out.push('warmupLaps');
  if (!intIn(p.durationMs, LIMITS.minutes.min * 60_000, LIMITS.minutes.max * 60_000)) out.push('duration');
  if (!intIn(p.racesPerLane, LIMITS.racesPerLane.min, LIMITS.racesPerLane.max)) out.push('racesPerLane');
  if (p.points.length !== MAX_PLACES || !p.points.every(v => intIn(v, LIMITS.points.min, LIMITS.points.max))) out.push('points');
  return out;
}

const pick = <T extends string>(v: unknown, allowed: readonly T[], fallback: T): T =>
  allowed.includes(v as T) ? v as T : fallback;
const num = (v: unknown, min: number, max: number, fallback: number, int = true): number => {
  if (typeof v !== 'number' || !Number.isFinite(v)) return fallback;
  const x = Math.min(max, Math.max(min, v));
  return int ? Math.round(x) : x;
};

/**
 * Reads a stored or imported preset back into a valid current one: missing
 * or out-of-range fields take the default (or are clamped). Returns null if
 * it is not a preset at all (no id). A stored shipped preset (the host's
 * edits of it) keeps builtIn; its missing fields take the shipped values.
 */
export function normalizePreset(raw: unknown): RacePreset | null {
  if (!raw || typeof raw !== 'object') return null;
  const r = raw as Record<string, any>;
  if (typeof r.id !== 'string' || !r.id) return null;
  const shipped = builtInDefault(r.id);
  const d = shipped ?? BASE;
  // A shipped preset keeps its kind; older stored ones (no kind) were a qualifying when they ran on time.
  const kind: PresetKind = shipped ? shipped.kind
    : r.kind === 'qualifying' || r.kind === 'race' ? r.kind
    : r.mode === 'time' ? 'qualifying' : 'race';
  const points = Array.isArray(r.points)
    ? Array.from({ length: MAX_PLACES }, (_, i) => num(r.points[i], LIMITS.points.min, LIMITS.points.max, 0))
    : [...d.points];
  return {
    schemaVersion: PRESET_SCHEMA_VERSION,
    id: r.id,
    name: typeof r.name === 'string' && r.name.trim() ? r.name : (builtInDefault(r.id)?.name ?? 'Preset'),
    builtIn: builtInDefault(r.id) !== undefined,
    kind,
    mode: modeOf(kind),
    laps: num(r.laps, LIMITS.laps.min, LIMITS.laps.max, d.laps),
    warmupLaps: num(r.warmupLaps, LIMITS.warmupLaps.min, LIMITS.warmupLaps.max, d.warmupLaps),
    durationMs: num(r.durationMs, LIMITS.minutes.min * 60_000, LIMITS.minutes.max * 60_000, d.durationMs),
    racesPerLane: num(r.racesPerLane, LIMITS.racesPerLane.min, LIMITS.racesPerLane.max, d.racesPerLane),
    rotation: pick(r.rotation, ['block', 'everyRace'] as const, d.rotation),
    points,
  };
}

// ---- what a preset means for a group -------------------------------------------------

/**
 * Races for a group of `drivers` (GUI_SPEC §4.2, §4.4): every driver drives
 * every used lane k times, and a group uses as many lanes as it has drivers
 * (never more than `lanes`; the rest stay empty). A qualifying rotates the
 * same way, one timed session per race. A group larger than the track does
 * not fit: 0. The races themselves: raceSchedule() in schedule.ts.
 */
export function racesFor(p: Pick<RacePreset, 'racesPerLane'>, drivers: number, lanes = MAX_PLACES): number {
  if (!Number.isInteger(drivers) || drivers < 1 || drivers > lanes) return 0;
  return p.racesPerLane * drivers;
}

/**
 * Whether race `i` (0-based) of a group starts with a warm-up: only when the
 * drivers sit down cold or have just changed lanes. Cold: the group's first
 * race, or the first of a section (`sectionStart`; another group raced in
 * between). Lane change: block rotation moves every driver after k races on
 * a lane, every-race rotation after each race (src/presets/schedule.ts).
 * A single driver never changes lanes. Racing one lane 3 times in a row: one
 * warm-up, before the first.
 */
export function warmupBefore(p: Pick<RacePreset, 'racesPerLane' | 'rotation'>, drivers: number, i: number, sectionStart = 0): boolean {
  if (i <= 0 || i === sectionStart) return true;
  if (drivers < 2) return false;
  return p.rotation === 'everyRace' || i % Math.max(1, p.racesPerLane) === 0;
}

/** How many of the races `start` … `start + count − 1` start with a warm-up (a section starting at `start`). */
export function warmupsIn(p: Pick<RacePreset, 'racesPerLane' | 'rotation'>, drivers: number, start: number, count: number): number {
  let n = 0;
  for (let i = start; i < start + count; i++) if (warmupBefore(p, drivers, i, start)) n++;
  return n;
}

/** One race's estimated length without its warm-up: the race and the changeover. */
export function raceEstimateMs(p: Pick<RacePreset, 'mode' | 'laps' | 'durationMs'>, avgLapMs: number, changeoverMs: number): number {
  return (p.mode === 'laps' ? p.laps * avgLapMs : p.durationMs) + changeoverMs;
}

type EstimateFields = Pick<RacePreset, 'mode' | 'laps' | 'durationMs' | 'warmupLaps' | 'racesPerLane' | 'rotation'>;

/**
 * A run of `count` races of a group, from race `start` (a section, or the
 * whole group): races × (race + changeover), plus warm-up laps × average lap
 * for each race that starts with a warm-up (warmupBefore).
 */
export function racesEstimateMs(p: EstimateFields, drivers: number, start: number, count: number, avgLapMs: number, changeoverMs: number): number {
  return count * raceEstimateMs(p, avgLapMs, changeoverMs) + warmupsIn(p, drivers, start, count) * p.warmupLaps * avgLapMs;
}

/** A group's estimated duration: all its races (racesEstimateMs). */
export function groupEstimateMs(p: EstimateFields, drivers: number, avgLapMs: number, changeoverMs: number, lanes = MAX_PLACES): number {
  return racesEstimateMs(p, drivers, 0, racesFor(p, drivers, lanes), avgLapMs, changeoverMs);
}

/**
 * The race engine's config for one race of this preset under the
 * tournament's rules, on a track with these timing settings (warm-up laps
 * are not part of it yet).
 */
export function toRaceConfig(p: RacePreset, rules: RaceRules, track: TrackTiming, id: string, entries: readonly RaceEntry[], name = p.name): RaceConfigInput {
  const c: Partial<RaceConfig> & Pick<RaceConfig, 'id' | 'entries'> = {
    id, name, entries,
    mode: p.mode, laps: p.laps, durationMs: p.durationMs,
    crashPolicy: rules.crashPolicy,
    falseStart: { ...rules.falseStart },
    ...trackRaceConfig(track),
  };
  return c;
}
