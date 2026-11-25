// The Grand Prix being set up (GUI_SPEC §6.1): which tournament, who drives
// (in seed order: fastest first), the group draw, and which step the host is
// on. Kept in this browser (localStorage), so a reload does not lose the
// names; guarded like prefs.ts, the app works without it.
import { signal, effect } from '@preact/signals';
import type { PhaseSettings } from '../../src/grandPrix/phase.ts';
import { normalizeCars, laneColorVars as carVars, DEFAULT_CAR, type CarSet, type CarSetup } from '../../src/grandPrix/cars.ts';

/** template → drivers → cars → groups (the overview) → round (one phase of the tree after the other). */
export type GpStep = 'template' | 'drivers' | 'cars' | 'groups' | 'round';
export const GP_STEPS: readonly GpStep[] = ['template', 'drivers', 'cars', 'groups', 'round'];
/** Within a round: its settings page, then its race page. */
export type RoundView = 'setup' | 'race';

export interface GpDriver { readonly id: string; readonly name: string }

const KEY = 'tt.gp.setup';

interface Saved {
  step?: unknown; templateId?: unknown; drivers?: unknown; draw?: unknown; finalSeats?: unknown;
  started?: unknown; cars?: unknown; round?: unknown; view?: unknown; section?: unknown; phases?: unknown; seating?: unknown; locked?: unknown; done?: unknown;
}
function load(): Saved {
  try { return JSON.parse(localStorage.getItem(KEY) ?? '{}') ?? {}; } catch { return {}; }
}
const saved = load();
const obj = (v: unknown) => v && typeof v === 'object' && !Array.isArray(v) ? v as Record<string, any> : {};

let seq = 0;
export const newDriverId = () => `d${Date.now().toString(36)}${(seq++).toString(36)}`;

export const gpStep = signal<GpStep>(GP_STEPS.includes(saved.step as GpStep) ? saved.step as GpStep : 'template');
export const gpTemplateId = signal<string | null>(typeof saved.templateId === 'string' ? saved.templateId : null);
export const gpDrivers = signal<GpDriver[]>(Array.isArray(saved.drivers)
  ? saved.drivers.filter((d: any) => d && typeof d.name === 'string').map((d: any) => ({ id: newDriverId(), name: d.name }))
  : []);
/** A random group draw: driver names in the drawn order; null when not drawn (or the field changed). */
export const gpDraw = signal<string[] | null>(Array.isArray(saved.draw) ? saved.draw.filter((x: unknown) => typeof x === 'string') : null);

// The evening's own choices (never stored in the template); null / empty = the defaults.
/** Seats in the final; null: as many as the track has lanes. */
export const gpFinalSeats = signal<number | null>(typeof saved.finalSeats === 'number' ? saved.finalSeats : null);
/** "Start Grand Prix" was pressed: the rounds are open. */
export const gpStarted = signal<boolean>(saved.started === true);
/** The round (tree column) on screen, and its page. */
export const gpRound = signal<number>(typeof saved.round === 'number' ? saved.round : 1);
export const gpView = signal<RoundView>(saved.view === 'race' ? 'race' : 'setup');
/** On the race page: which section of the round (src/tournaments/runOrder.ts), 0-based. */
export const gpSection = signal<number>(typeof saved.section === 'number' ? saved.section : 0);
/** Each round's settings for tonight, by column (src/grandPrix/phase.ts). */
export const gpPhases = signal<Record<string, PhaseSettings>>(obj(saved.phases));
/** Where each driver of a stage starts (lane per driver index), by stage id; missing: lanes 1, 2 … */
export const gpSeating = signal<Record<string, number[]>>(obj(saved.seating));
/** Stages whose lanes are fixed: their first warm-up was started. */
export const gpLocked = signal<string[]>(Array.isArray(saved.locked) ? saved.locked.filter((x: unknown) => typeof x === 'string') : []);
/** Races that finished, as `stage:race` (race 0-based): the timeline opens what comes after them. */
export const gpDone = signal<string[]>(Array.isArray(saved.done) ? saved.done.filter((x: unknown) => typeof x === 'string') : []);
export const raceKey = (stage: string, race: number): string => `${stage}:${race}`;

/**
 * The cars on the lanes tonight, by lane ('1' … '6'; a missing lane has the
 * default car). They are the real cars on the track, so picking another
 * tournament keeps them.
 */
export const gpCars = signal<Record<string, CarSetup>>(normalizeCars(saved.cars));
export const carOf = (lane: number): CarSetup => gpCars.value[String(lane)] ?? DEFAULT_CAR;
export function setCar(lane: number, patch: Partial<CarSetup>): void {
  gpCars.value = normalizeCars({ ...gpCars.value, [lane]: { ...carOf(lane), ...patch } });
}
/** Puts a car preset's cars on the lanes (a copy: later changes tonight do not touch the preset). */
export function applyCarSet(cars: CarSet): void {
  gpCars.value = normalizeCars(cars);
}
/** The lane colours of this Grand Prix as CSS variables for the page's root (src/grandPrix/cars.ts). */
export const laneColorVars = (): string => carVars(gpCars.value);

/** Back to the defaults (a different tournament was picked). */
export function resetEvening(): void {
  gpFinalSeats.value = null; gpDraw.value = null; gpStarted.value = false;
  gpRound.value = 1; gpView.value = 'setup'; gpSection.value = 0; gpPhases.value = {}; gpSeating.value = {}; gpLocked.value = []; gpDone.value = [];
}

/** The card whose name field should take the focus next (a just-added driver). */
export const gpFocusId = signal<string | null>(null);

effect(() => {
  const data = {
    step: gpStep.value, templateId: gpTemplateId.value, drivers: gpDrivers.value.map(d => ({ name: d.name })), draw: gpDraw.value,
    finalSeats: gpFinalSeats.value, started: gpStarted.value, cars: gpCars.value, round: gpRound.value, view: gpView.value, section: gpSection.value,
    phases: gpPhases.value, seating: gpSeating.value, locked: gpLocked.value, done: gpDone.value,
  };
  try { localStorage.setItem(KEY, JSON.stringify(data)); } catch { /* not kept */ }
});

export function addDriver(after?: string): void {
  const d = { id: newDriverId(), name: '' };
  const list = gpDrivers.value;
  const i = after ? list.findIndex(x => x.id === after) : -1;
  gpDrivers.value = i < 0 ? [...list, d] : [...list.slice(0, i + 1), d, ...list.slice(i + 1)];
  gpFocusId.value = d.id;
}

export function renameDriver(id: string, name: string): void {
  gpDrivers.value = gpDrivers.value.map(d => d.id === id ? { ...d, name } : d);
}

export function removeDriver(id: string): void {
  gpDrivers.value = gpDrivers.value.filter(d => d.id !== id);
}

export function moveDriver(from: number, to: number): void {
  const list = [...gpDrivers.value];
  const [d] = list.splice(from, 1);
  list.splice(to, 0, d);
  gpDrivers.value = list;
}

/** Draws the field at random (Fisher–Yates). */
export function drawGroups(names: readonly string[]): void {
  const a = [...names];
  for (let i = a.length - 1; i > 0; i--) {
    const j = Math.floor(Math.random() * (i + 1));
    [a[i], a[j]] = [a[j], a[i]];
  }
  gpDraw.value = a;
}
