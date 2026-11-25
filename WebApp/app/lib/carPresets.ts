// The car preset library as app state (GUI_SPEC §5a): named sets of cars
// (per lane: type, colour, number), stored as the setting "carPresets" in
// the library database, in the host's order. A Grand Prix copies one into
// its Cars step. The data and its rules are in src/grandPrix/cars.ts.
import { signal } from '@preact/signals';
import { normalizeCarPresets, normalizeCars, carIn, CAR_PRESET_NAME_MAX, type CarPreset, type CarSet, type CarSetup } from '../../src/grandPrix/cars.ts';
import { uniqueName } from '../../src/presets/preset.ts';
import { openLibraryStorage, type LibraryStorage } from '../../src/store/libraryStorage.ts';
import { t } from '../i18n/i18n.ts';

const KEY = 'carPresets';

export const carPresets = signal<CarPreset[]>([]);
export const carPresetsLoaded = signal(false);
export const carPresetsPersistent = signal(true);
export const selectedCarPresetId = signal<string | null>(null);

let storage: LibraryStorage | null = null;
const ready = openLibraryStorage().then(async s => {
  storage = s;
  carPresetsPersistent.value = s.persistent;
  carPresets.value = normalizeCarPresets(await s.getSetting(KEY));
}).catch(() => { carPresetsPersistent.value = false; })
  .finally(() => { carPresetsLoaded.value = true; });

function store(list: CarPreset[]): void {
  carPresets.value = list;
  void ready.then(() => storage?.putSetting(KEY, list)).catch(() => { carPresetsPersistent.value = false; });
}

function newId(): string {
  return 'c-' + (crypto.randomUUID?.() ?? Date.now().toString(36) + Math.random().toString(36).slice(2));
}

const names = () => carPresets.value.map(p => p.name);

/** A new preset after `after` (or at the end): the given cars, or the default cars. */
export function createCarPreset(cars: CarSet = {}, name = t('cp.newName'), after?: string): CarPreset {
  const p: CarPreset = { id: newId(), name: uniqueName(name.slice(0, CAR_PRESET_NAME_MAX), names()), cars: normalizeCars(cars) };
  const list = [...carPresets.value];
  const i = after ? list.findIndex(x => x.id === after) : -1;
  list.splice(i < 0 ? list.length : i + 1, 0, p);
  store(list);
  selectedCarPresetId.value = p.id;
  return p;
}

export function duplicateCarPreset(src: CarPreset): CarPreset {
  return createCarPreset(src.cars, src.name, src.id);
}

/** Renames a preset; false when the name is empty or taken (nothing changes). */
export function renameCarPreset(id: string, raw: string): boolean {
  const name = raw.trim().slice(0, CAR_PRESET_NAME_MAX);
  if (!name || carPresets.value.some(p => p.id !== id && p.name.toLocaleLowerCase() === name.toLocaleLowerCase())) return false;
  store(carPresets.value.map(p => p.id === id ? { ...p, name } : p));
  return true;
}

export function setPresetCar(id: string, lane: number, patch: Partial<CarSetup>): void {
  store(carPresets.value.map(p => p.id === id
    ? { ...p, cars: normalizeCars({ ...p.cars, [lane]: { ...carIn(p.cars, lane), ...patch } }) } : p));
}

export function deleteCarPreset(id: string): void {
  const list = carPresets.value;
  const i = list.findIndex(p => p.id === id);
  if (i < 0) return;
  if (selectedCarPresetId.value === id) selectedCarPresetId.value = (list[i + 1] ?? list[i - 1])?.id ?? null;
  store(list.filter(p => p.id !== id));
}

export function moveCarPreset(from: number, to: number): void {
  if (from === to) return;
  const list = [...carPresets.value];
  const [p] = list.splice(from, 1);
  list.splice(to, 0, p);
  store(list);
}

/** Adds imported car presets at the end (ids and names already unique, app/lib/library.ts). */
export function addCarPresets(list: readonly CarPreset[]): void {
  if (list.length) store([...carPresets.value, ...list]);
}
