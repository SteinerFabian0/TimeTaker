/*
 * Tournament templates (GUI_SPEC §5): plain data and pure functions. DOM-free.
 *
 * A template is written as rules, not as a fixed tree: its format says how
 * the field is split and who advances, and src/tournaments/plan.ts computes
 * the tree for N drivers on L lanes from it (§5.4). The races of every stage
 * come from race presets, referenced by id (deleting a preset that a
 * template uses is refused, §4.1).
 *
 * Every stored template carries schemaVersion; normalizeTemplate() reads any
 * stored shape (older or hand-edited) back into a valid current one.
 */

import { BUILTIN_IDS } from '../presets/preset.ts';
import { DEFAULT_RULES, normalizeRules, type RaceRules } from '../presets/rules.ts';

export const TEMPLATE_SCHEMA_VERSION = 1;

/**
 * single                 everyone in one stage, which is the final (N ≤ L)
 * groupsFinal            groups, the best of each straight into the final
 * groupsRepechageFinal   groups; the best go to the final, the rest race a
 *                        repechage whose winners fill the final's last seats
 *                        (the reference evening)
 */
export type Format = 'single' | 'groupsFinal' | 'groupsRepechageFinal';
export const FORMATS: readonly Format[] = ['single', 'groupsFinal', 'groupsRepechageFinal'];

/** How the field is split into groups: snake by seed (strong drivers spread evenly), or a random draw. */
export type SplitMode = 'snake' | 'random';

/**
 * How a points tie in a stage is broken (§5.2). More points always rank
 * first; for a tie:
 * winsThenBestLap  more race wins, then the best lap (default)
 * bestLap          the best lap only
 * wins             more race wins only; still tied → shared place
 * tiebreakerRace   a tiebreaker race when the tie decides who advances;
 *                  ties that do not matter are broken by winsThenBestLap
 */
export type TieBreak = 'winsThenBestLap' | 'bestLap' | 'wins' | 'tiebreakerRace';
export const TIE_BREAKS: readonly TieBreak[] = ['winsThenBestLap', 'bestLap', 'wins', 'tiebreakerRace'];

export interface TournamentTemplate {
  readonly schemaVersion: number;
  readonly id: string;
  readonly name: string;
  /** Shipped with the app: editable, can be reset to its defaults, never deleted; its format is fixed. */
  readonly builtIn: boolean;
  readonly format: Format;
  readonly split: SplitMode;
  /** Race presets (ids) per kind of stage. `groupPreset` and `repechagePreset` are unused by formats without them. */
  readonly groupPreset: string;
  readonly repechagePreset: string;
  readonly finalPreset: string;
  /**
   * Final seats for repechage winners (groupsRepechageFinal): 'auto' picks
   * about a third of the lanes so every group sends the same number
   * straight to the final (§5.4); a number is the host's choice.
   */
  readonly repechageSeats: 'auto' | number;
  readonly tieBreak: TieBreak;
  /** The race preset of a tiebreaker race (tieBreak 'tiebreakerRace'): always the shipped Tiebreaker (GUI_SPEC §4.1). */
  readonly tiebreakerPreset: string;
  /** Crashes, result basis and false starts: the same for every race of the tournament. */
  readonly rules: RaceRules;
  /**
   * The next best after the final race a small final (when the format and the
   * field leave at least 2 for it). The Grand Prix can still move seats
   * between the final and the small final.
   */
  readonly smallFinal: boolean;
}

/** Most drivers a template is planned for (the preview's stepper). */
export const LIMITS = {
  drivers: { min: 2, max: 48 },
  repechageSeats: { min: 1, max: 5 },
} as const;

/** The lane counts tracks come with (GUI_SPEC §1). */
export const LANE_COUNTS = [2, 4, 6] as const;

// ---- construction -----------------------------------------------------------------

const BASE: Omit<TournamentTemplate, 'id' | 'name' | 'builtIn' | 'format'> = {
  schemaVersion: TEMPLATE_SCHEMA_VERSION,
  split: 'snake',
  groupPreset: BUILTIN_IDS.default,
  repechagePreset: BUILTIN_IDS.short,
  finalPreset: BUILTIN_IDS.default,
  repechageSeats: 'auto',
  tieBreak: 'winsThenBestLap',
  tiebreakerPreset: BUILTIN_IDS.tiebreaker,
  rules: DEFAULT_RULES,
  smallFinal: true,
};

/** Ids of the shipped templates; stable. */
export const BUILTIN_TEMPLATE_IDS = {
  single: 'builtin-single',
  groupsFinal: 'builtin-groups-final',
  groupsRepechageFinal: 'builtin-groups-repechage-final',
} as const;

/**
 * The shipped templates (§5.4), in their default list order. The reference
 * evening first: groups on the default race, the repechage on the short
 * race, the final on the default race. The app translates a name while it
 * is still the shipped (English) one.
 */
export const BUILTIN_TEMPLATES: readonly TournamentTemplate[] = [
  { ...BASE, id: BUILTIN_TEMPLATE_IDS.groupsRepechageFinal, name: 'Groups → Second chance → Final', builtIn: true, format: 'groupsRepechageFinal' },
  { ...BASE, id: BUILTIN_TEMPLATE_IDS.groupsFinal, name: 'Groups → Final', builtIn: true, format: 'groupsFinal' },
  { ...BASE, id: BUILTIN_TEMPLATE_IDS.single, name: 'Single race', builtIn: true, format: 'single' },
];

export function builtInTemplate(id: string): TournamentTemplate | undefined {
  return BUILTIN_TEMPLATES.find(t => t.id === id);
}

/** True when a shipped template still has all its shipped values. */
export function isTemplateAtDefaults(t: TournamentTemplate): boolean {
  const d = builtInTemplate(t.id);
  return !!d && JSON.stringify(normalizeTemplate(t)) === JSON.stringify(normalizeTemplate(d));
}

/** A new template: the reference evening's format, so it is useful right away. */
export function newTemplate(id: string, name: string): TournamentTemplate {
  return { ...BASE, id, name, builtIn: false, format: 'groupsRepechageFinal' };
}

export function duplicateTemplate(t: TournamentTemplate, id: string, name: string): TournamentTemplate {
  return { ...t, id, name, builtIn: false, schemaVersion: TEMPLATE_SCHEMA_VERSION };
}

/** The race presets a template actually uses (for "used by", §4.1). */
export function presetsUsed(t: TournamentTemplate): string[] {
  const ids = [t.finalPreset];
  if (t.format !== 'single') ids.push(t.groupPreset);
  if (t.format === 'groupsRepechageFinal') ids.push(t.repechagePreset);
  if (t.tieBreak === 'tiebreakerRace' && t.format !== 'single') ids.push(t.tiebreakerPreset);
  return [...new Set(ids)];
}

// ---- normalization ------------------------------------------------------------------

const pick = <T extends string>(v: unknown, allowed: readonly T[], fallback: T): T =>
  allowed.includes(v as T) ? v as T : fallback;
const str = (v: unknown, fallback: string): string => typeof v === 'string' && v ? v : fallback;

/**
 * Reads a stored or imported template back into a valid current one.
 * Returns null if it is not a template at all (no id). A stored shipped
 * template keeps builtIn and its shipped format.
 */
export function normalizeTemplate(raw: unknown): TournamentTemplate | null {
  if (!raw || typeof raw !== 'object') return null;
  const r = raw as Record<string, unknown>;
  if (typeof r.id !== 'string' || !r.id) return null;
  const shipped = builtInTemplate(r.id);
  const d = shipped ?? { ...BASE, format: 'groupsRepechageFinal' as Format };
  const seats = r.repechageSeats;
  return {
    schemaVersion: TEMPLATE_SCHEMA_VERSION,
    id: r.id,
    name: typeof r.name === 'string' && r.name.trim() ? r.name : (shipped?.name ?? 'Tournament'),
    builtIn: shipped !== undefined,
    format: shipped ? shipped.format : pick(r.format, FORMATS, d.format),
    split: pick(r.split, ['snake', 'random'] as const, d.split),
    groupPreset: str(r.groupPreset, d.groupPreset),
    repechagePreset: str(r.repechagePreset, d.repechagePreset),
    finalPreset: str(r.finalPreset, d.finalPreset),
    repechageSeats: typeof seats === 'number' && Number.isInteger(seats)
      ? Math.min(LIMITS.repechageSeats.max, Math.max(LIMITS.repechageSeats.min, seats))
      : 'auto',
    tieBreak: pick(r.tieBreak, TIE_BREAKS, d.tieBreak),
    tiebreakerPreset: BUILTIN_IDS.tiebreaker,
    rules: normalizeRules(r.rules),
    smallFinal: typeof r.smallFinal === 'boolean' ? r.smallFinal : d.smallFinal,
  };
}
