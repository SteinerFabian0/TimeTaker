/*
 * The library as a file (GUI_SPEC §11 Data): race presets, tournament
 * templates and car presets in one JSON file, to share with a colleague or
 * keep as a backup.
 *
 *   { format: "timetaker-library", version: 1, exportedAt, app,
 *     presets: RacePreset[], templates: TournamentTemplate[], carPresets: CarPreset[] }
 *
 * Exported: the host's own presets and templates, the shipped ones only when
 * edited, and every car preset. Not exported: track timing, lane colours and
 * other settings of this track or browser.
 *
 * Importing never overwrites or deletes anything. planImport() decides per
 * item:
 *   - already here (same id, same content)  → skipped
 *   - new id                                → added as it is
 *   - same id, different content            → added as a copy (new id)
 *   - an edited shipped preset or template  → added as a copy of it; the
 *     tiebreaker is skipped instead (there is only one)
 * A name that is taken gets " 2", " 3" … Templates follow their presets to
 * new ids; a template whose preset is in neither the file nor the library
 * gets the default preset for that stage.
 */

import { normalizePreset, uniqueName, isTiebreaker, builtInDefault, type RacePreset } from '../presets/preset.ts';
import { normalizeTemplate, newTemplate, builtInTemplate, type TournamentTemplate } from '../tournaments/template.ts';
import { normalizeCarPresets, CAR_PRESET_NAME_MAX, type CarPreset } from '../grandPrix/cars.ts';

export const LIBRARY_FILE_FORMAT = 'timetaker-library';
export const LIBRARY_FILE_VERSION = 1;

export interface Library {
  readonly presets: readonly RacePreset[];
  readonly templates: readonly TournamentTemplate[];
  readonly carPresets: readonly CarPreset[];
}

export interface LibraryFile extends Library {
  readonly format: typeof LIBRARY_FILE_FORMAT;
  readonly version: number;
  /** ISO time of the export. */
  readonly exportedAt: string;
  /** The app version that wrote it (informational). */
  readonly app: string;
}

/** The file of a library: `lib` holds what should go in (the caller leaves out unedited shipped items). */
export function makeLibraryFile(lib: Library, now: Date, app: string): LibraryFile {
  return {
    format: LIBRARY_FILE_FORMAT, version: LIBRARY_FILE_VERSION, exportedAt: now.toISOString(), app,
    presets: lib.presets.map(p => structuredClone(p)),
    templates: lib.templates.map(x => structuredClone(x)),
    carPresets: lib.carPresets.map(c => structuredClone(c)),
  };
}

/** A file name for an export on that day: timetaker-library-2026-11-25.json (local date). */
export function libraryFileName(now: Date): string {
  const p = (n: number) => String(n).padStart(2, '0');
  return `timetaker-library-${now.getFullYear()}-${p(now.getMonth() + 1)}-${p(now.getDate())}.json`;
}

export type ParseError = 'json' | 'format' | 'version';
export type ParseResult = { ok: true; file: LibraryFile } | { ok: false; error: ParseError };

/**
 * Reads a library file. Unreadable items are skipped (each list is
 * normalized like stored data); a missing list is empty.
 */
export function parseLibraryFile(text: string): ParseResult {
  let raw: unknown;
  try { raw = JSON.parse(text); } catch { return { ok: false, error: 'json' }; }
  if (!raw || typeof raw !== 'object' || (raw as Record<string, unknown>).format !== LIBRARY_FILE_FORMAT) {
    return { ok: false, error: 'format' };
  }
  const r = raw as Record<string, unknown>;
  if (typeof r.version !== 'number' || r.version > LIBRARY_FILE_VERSION) return { ok: false, error: 'version' };
  const list = (v: unknown): unknown[] => Array.isArray(v) ? v : [];
  return {
    ok: true,
    file: {
      format: LIBRARY_FILE_FORMAT,
      version: r.version,
      exportedAt: typeof r.exportedAt === 'string' ? r.exportedAt : '',
      app: typeof r.app === 'string' ? r.app : '',
      presets: unique(list(r.presets).map(normalizePreset)),
      templates: unique(list(r.templates).map(normalizeTemplate)),
      carPresets: normalizeCarPresets(r.carPresets),
    },
  };
}

/** Non-null items, the first of each id. */
function unique<T extends { id: string }>(xs: (T | null)[]): T[] {
  const seen = new Set<string>();
  return xs.filter((x): x is T => x !== null && !seen.has(x.id) && (seen.add(x.id), true));
}

export interface ImportPlan {
  /** To add, in file order (ids and names already made unique). */
  readonly presets: RacePreset[];
  readonly templates: TournamentTemplate[];
  readonly carPresets: CarPreset[];
  /** Items left out because the library already has them (or, for the tiebreaker, cannot have two). */
  readonly skipped: number;
  /** Items added under another name than the file's. */
  readonly renamed: number;
}

export interface ImportOptions {
  /** A fresh id per kind: presets 'p-…', templates 't-…', car presets 'c-…'. */
  newId: (kind: 'preset' | 'template' | 'car') => string;
  /** Further taken names, e.g. the translated names of shipped items. */
  takenPresetNames?: readonly string[];
  takenTemplateNames?: readonly string[];
}

const same = (a: unknown, b: unknown): boolean => JSON.stringify(a) === JSON.stringify(b);
const sameWithout = <T extends object>(a: T, b: T, ...keys: (keyof T)[]): boolean => {
  const strip = (x: T) => { const c: Record<string, unknown> = { ...x }; for (const k of keys) delete c[k as string]; return c; };
  return same(strip(a), strip(b));
};

/** What importing `file` into `current` (the whole library as the app shows it) adds. */
export function planImport(file: Library, current: Library, opts: ImportOptions): ImportPlan {
  let skipped = 0, renamed = 0;

  // ---- presets
  const presetNames = [...current.presets.map(p => p.name), ...(opts.takenPresetNames ?? [])];
  const presetIds = new Map(current.presets.map(p => [p.id, p]));
  const presetMap = new Map<string, string>();        // file id → id in the library after the import
  const presets: RacePreset[] = [];
  for (const p of file.presets) {
    const here = presetIds.get(p.id);
    if (here && same(normalizePreset(here), normalizePreset(p))) { presetMap.set(p.id, p.id); skipped++; continue; }
    if (here && isTiebreaker(p.id)) { presetMap.set(p.id, p.id); skipped++; continue; }
    const id = here || builtInDefault(p.id) ? opts.newId('preset') : p.id;
    const name = uniqueName(p.name, presetNames);
    if (name !== p.name.trim()) renamed++;
    const added: RacePreset = { ...p, id, name, builtIn: false };
    presets.push(added);
    presetNames.push(name);
    presetIds.set(id, added);
    presetMap.set(p.id, id);
  }

  // ---- templates (their presets first moved to the ids above)
  const fallback = newTemplate('', '');
  const known = (id: string) => presetIds.has(id) || builtInDefault(id) !== undefined;
  const remap = (id: string, slot: 'groupPreset' | 'repechagePreset' | 'finalPreset'): string => {
    const to = presetMap.get(id) ?? id;
    return known(to) ? to : fallback[slot];
  };
  const tplNames = [...current.templates.map(x => x.name), ...(opts.takenTemplateNames ?? [])];
  const tplIds = new Map(current.templates.map(x => [x.id, x]));
  const templates: TournamentTemplate[] = [];
  for (const raw of file.templates) {
    const x: TournamentTemplate = {
      ...raw,
      groupPreset: remap(raw.groupPreset, 'groupPreset'),
      repechagePreset: remap(raw.repechagePreset, 'repechagePreset'),
      finalPreset: remap(raw.finalPreset, 'finalPreset'),
    };
    const here = tplIds.get(x.id);
    if (here && same(normalizeTemplate(here), normalizeTemplate(x))) { skipped++; continue; }
    const id = here || builtInTemplate(x.id) ? opts.newId('template') : x.id;
    const name = uniqueName(x.name, tplNames);
    if (name !== x.name.trim()) renamed++;
    const added: TournamentTemplate = { ...x, id, name, builtIn: false };
    templates.push(added);
    tplNames.push(name);
    tplIds.set(id, added);
  }

  // ---- car presets
  const carNames = current.carPresets.map(c => c.name);
  const carIds = new Map(current.carPresets.map(c => [c.id, c]));
  const carPresets: CarPreset[] = [];
  for (const c of file.carPresets) {
    const here = carIds.get(c.id);
    if (here && same(here, c)) { skipped++; continue; }
    // The same cars under the same name with another id: also already here.
    if (current.carPresets.some(o => sameWithout(o, c, 'id'))) { skipped++; continue; }
    const id = here ? opts.newId('car') : c.id;
    const name = uniqueName(c.name, carNames).slice(0, CAR_PRESET_NAME_MAX);
    if (name !== c.name) renamed++;
    const added: CarPreset = { ...c, id, name };
    carPresets.push(added);
    carNames.push(name);
    carIds.set(id, added);
  }

  return { presets, templates, carPresets, skipped, renamed };
}
