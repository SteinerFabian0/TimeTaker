// The preset library as app state: the shipped presets (with the host's
// edits, if any) plus the user's own, loaded from IndexedDB, saved on every
// change. An edited shipped preset is stored under its own id; resetting it
// deletes that record. The list order is the host's (drag and drop), stored
// as the setting "presetOrder". The data and its rules are in src/presets
// (DOM-free); this file only holds and persists it.
import { signal, computed } from '@preact/signals';
import {
  BUILTIN_PRESETS, builtInDefault, duplicatePreset, isTiebreaker, newPreset, uniqueName, type PresetKind, type RacePreset,
} from '../../src/presets/preset.ts';
import { applyOrder, insertAfter, moveId } from '../../src/presets/order.ts';
import { openLibraryStorage, type LibraryStorage } from '../../src/store/libraryStorage.ts';
import { t, type Key } from '../i18n/i18n.ts';

/** Everything stored: the user's presets and edited shipped ones. */
const userPresets = signal<RacePreset[]>([]);
/** The host's list order (ids); empty until the list is first reordered or added to. */
const order = signal<string[]>([]);
const ORDER_KEY = 'presetOrder';
export const presetsLoaded = signal(false);
/** False when the browser blocks storage: presets live until the page closes. */
export const presetsPersistent = signal(true);

let storage: LibraryStorage | null = null;
const ready = openLibraryStorage().then(async s => {
  storage = s;
  presetsPersistent.value = s.persistent;
  userPresets.value = await s.list();
  const stored = await s.getSetting(ORDER_KEY);
  if (Array.isArray(stored)) order.value = stored.filter((x): x is string => typeof x === 'string');
}).catch(() => { presetsPersistent.value = false; })
  .finally(() => { presetsLoaded.value = true; });

/** Display name: a shipped preset is translated while it keeps its shipped name. */
export function presetName(p: RacePreset): string {
  return p.builtIn && p.name === builtInDefault(p.id)?.name ? t(`preset.builtin.${p.id}` as Key) : p.name;
}

const byName = (a: RacePreset, b: RacePreset) => a.name.localeCompare(b.name, undefined, { sensitivity: 'base', numeric: true });

/**
 * Every preset in the host's order. Without one (or for presets it does
 * not know yet): shipped first in shipped order, then the user's by name.
 * The page lists them by kind (qualifying, race, tiebreaker), each kind in
 * this order.
 */
const ordered = computed<RacePreset[]>(() => {
  const stored = new Map(userPresets.value.map(p => [p.id, p]));
  return applyOrder([
    ...BUILTIN_PRESETS.map(b => stored.get(b.id) ?? b),
    ...userPresets.value.filter(p => !p.builtIn).sort(byName),
  ], order.value);
});

export function presetsOfKind(kind: PresetKind): RacePreset[] {
  return ordered.value.filter(p => p.kind === kind);
}

/** The presets a stage can race: the qualifyings, then the races (never the tiebreaker, GUI_SPEC §4.1). */
export const stagePresets = computed<RacePreset[]>(() => [...presetsOfKind('qualifying'), ...presetsOfKind('race')]);

/** The tiebreaker preset (with the host's edits). */
export const tiebreakerPreset = computed<RacePreset>(() => presetsOfKind('tiebreaker')[0]);

/** All presets, kind by kind. */
export const presets = computed<RacePreset[]>(() => [...stagePresets.value, ...presetsOfKind('tiebreaker')]);

function setOrder(ids: string[]): void {
  order.value = ids;
  void ready.then(() => storage?.putSetting(ORDER_KEY, ids)).catch(() => {});
}

/** Moves the preset at position `from` to `to` within its kind's list (drag and drop, Alt+arrows). */
export function movePreset(kind: PresetKind, from: number, to: number): void {
  if (from === to) return;
  const moved = moveId(presetsOfKind(kind).map(p => p.id), from, to);
  setOrder(presets.value.map(p => p.kind === kind ? moved.shift()! : p.id));
}

export const selectedPresetId = signal<string>(BUILTIN_PRESETS[0].id);

function newId(): string {
  return 'p-' + (crypto.randomUUID?.() ?? Date.now().toString(36) + Math.random().toString(36).slice(2));
}

function takenNames(): string[] {
  return presets.value.map(p => [p.name, presetName(p)]).flat();
}

async function save(p: RacePreset): Promise<void> {
  await ready;
  try { await storage?.put(p); } catch { presetsPersistent.value = false; }
}

/** A new qualifying or race preset, at the end of its kind's list. */
export function createPreset(kind: Exclude<PresetKind, 'tiebreaker'>): RacePreset {
  const p = newPreset(newId(), uniqueName(t(kind === 'qualifying' ? 'preset.newQualifying' : 'preset.newName'), takenNames()), kind);
  const same = presetsOfKind(kind);
  setOrder(insertAfter(presets.value.map(x => x.id), p.id, same.length ? same[same.length - 1].id : null));
  userPresets.value = [...userPresets.value, p];
  selectedPresetId.value = p.id;
  void save(p);
  return p;
}

/** A copy right below its source; the tiebreaker cannot be duplicated (null). */
export function duplicate(p: RacePreset): RacePreset | null {
  if (isTiebreaker(p.id)) return null;
  const d = duplicatePreset(p, newId(), uniqueName(presetName(p), takenNames()));
  setOrder(insertAfter(presets.value.map(x => x.id), d.id, p.id));    // right below its source
  userPresets.value = [...userPresets.value, d];
  selectedPresetId.value = d.id;
  void save(d);
  return d;
}

/** Applies a change to a preset; a shipped one is stored from its first change on. */
export function updatePreset(id: string, patch: Partial<RacePreset>): void {
  const old = presets.value.find(p => p.id === id);
  if (!old) return;
  const changed: RacePreset = { ...old, ...patch, id, builtIn: old.builtIn };
  userPresets.value = userPresets.value.some(p => p.id === id)
    ? userPresets.value.map(p => p.id === id ? changed : p)
    : [...userPresets.value, changed];
  void save(changed);
}

/**
 * A changed shipped preset: its changes become a new preset of the same kind
 * (right below it, selected), and the shipped one goes back to its defaults.
 */
export function saveAsNew(p: RacePreset): RacePreset | null {
  if (!p.builtIn) return null;
  const d = duplicate(p);
  if (d) resetPreset(p.id);
  return d;
}

/** A shipped preset back to its shipped values. */
export function resetPreset(id: string): void {
  if (!builtInDefault(id)) return;
  userPresets.value = userPresets.value.filter(p => p.id !== id);
  void ready.then(() => storage?.delete(id)).catch(() => {});
}

/** Set by app/lib/tournaments.ts (it imports this file, so not the other way round). */
let usedByProvider: (id: string) => string[] = () => [];
export function setUsedByProvider(f: (id: string) => string[]): void { usedByProvider = f; }

/**
 * Names of the tournament templates that use a preset (deleting it is
 * refused while any does, GUI_SPEC §4.1).
 */
export function usedBy(id: string): string[] {
  return usedByProvider(id);
}

export function deletePreset(id: string): void {
  const list = presets.value;
  const i = list.findIndex(p => p.id === id);
  if (i < 0 || list[i].builtIn || usedBy(id).length) return;
  userPresets.value = userPresets.value.filter(p => p.id !== id);
  if (order.value.includes(id)) setOrder(order.value.filter(x => x !== id));
  if (selectedPresetId.value === id) selectedPresetId.value = (list[i + 1] ?? list[i - 1]).id;
  void ready.then(() => storage?.delete(id)).catch(() => {});
}

/** Adds imported presets (ids and names already unique, app/lib/library.ts). */
export function addPresets(list: readonly RacePreset[]): void {
  if (!list.length) return;
  userPresets.value = [...userPresets.value, ...list];
  for (const p of list) void save(p);
}

/** The presets as the app shows them, and the names taken by translated shipped ones. */
export function presetNames(): string[] { return takenNames(); }
