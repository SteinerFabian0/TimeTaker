// The tournament template library as app state, like app/lib/presets.ts:
// the shipped templates (with the host's edits, if any) plus the user's own,
// loaded from IndexedDB, saved on every change. The list order is the
// host's, stored as the setting "templateOrder". The data and its rules are
// in src/tournaments (DOM-free); this file only holds and persists it.
import { signal, computed } from '@preact/signals';
import {
  BUILTIN_TEMPLATES, builtInTemplate, duplicateTemplate, newTemplate, presetsUsed, type TournamentTemplate,
} from '../../src/tournaments/template.ts';
import { uniqueName } from '../../src/presets/preset.ts';
import { applyOrder, insertAfter, moveId } from '../../src/presets/order.ts';
import { openLibraryStorage, type LibraryStorage } from '../../src/store/libraryStorage.ts';
import { t, type Key } from '../i18n/i18n.ts';
import { setUsedByProvider } from './presets.ts';

const userTemplates = signal<TournamentTemplate[]>([]);
const order = signal<string[]>([]);
const ORDER_KEY = 'templateOrder';
export const templatesLoaded = signal(false);
export const templatesPersistent = signal(true);

let storage: LibraryStorage | null = null;
const ready = openLibraryStorage().then(async s => {
  storage = s;
  templatesPersistent.value = s.persistent;
  userTemplates.value = await s.listTemplates();
  const stored = await s.getSetting(ORDER_KEY);
  if (Array.isArray(stored)) order.value = stored.filter((x): x is string => typeof x === 'string');
}).catch(() => { templatesPersistent.value = false; })
  .finally(() => { templatesLoaded.value = true; });

/** Display name: a shipped template is translated while it keeps its shipped name. */
export function templateName(x: TournamentTemplate): string {
  return x.builtIn && x.name === builtInTemplate(x.id)?.name ? t(`tpl.builtin.${x.id}` as Key) : x.name;
}

const byName = (a: TournamentTemplate, b: TournamentTemplate) =>
  a.name.localeCompare(b.name, undefined, { sensitivity: 'base', numeric: true });

export const templates = computed<TournamentTemplate[]>(() => {
  const stored = new Map(userTemplates.value.map(x => [x.id, x]));
  return applyOrder([
    ...BUILTIN_TEMPLATES.map(b => stored.get(b.id) ?? b),
    ...userTemplates.value.filter(x => !x.builtIn).sort(byName),
  ], order.value);
});

// A race preset cannot be deleted while a template uses it (GUI_SPEC §4.1).
setUsedByProvider(presetId => templates.value.filter(x => presetsUsed(x).includes(presetId)).map(templateName));

function setOrder(ids: string[]): void {
  order.value = ids;
  void ready.then(() => storage?.putSetting(ORDER_KEY, ids)).catch(() => {});
}

export function moveTemplate(from: number, to: number): void {
  if (from === to) return;
  setOrder(moveId(templates.value.map(x => x.id), from, to));
}

export const selectedTemplateId = signal<string>(BUILTIN_TEMPLATES[0].id);

function newId(): string {
  return 't-' + (crypto.randomUUID?.() ?? Date.now().toString(36) + Math.random().toString(36).slice(2));
}

function takenNames(): string[] {
  return templates.value.map(x => [x.name, templateName(x)]).flat();
}

async function save(x: TournamentTemplate): Promise<void> {
  await ready;
  try { await storage?.putTemplate(x); } catch { templatesPersistent.value = false; }
}

export function createTemplate(): TournamentTemplate {
  const x = newTemplate(newId(), uniqueName(t('tpl.newName'), takenNames()));
  setOrder(insertAfter(templates.value.map(y => y.id), x.id, null));
  userTemplates.value = [...userTemplates.value, x];
  selectedTemplateId.value = x.id;
  void save(x);
  return x;
}

export function duplicateTpl(src: TournamentTemplate): TournamentTemplate {
  const d = duplicateTemplate(src, newId(), uniqueName(templateName(src), takenNames()));
  setOrder(insertAfter(templates.value.map(y => y.id), d.id, src.id));
  userTemplates.value = [...userTemplates.value, d];
  selectedTemplateId.value = d.id;
  void save(d);
  return d;
}

/** Applies a change; a shipped template is stored from its first change on and keeps its format. */
export function updateTemplate(id: string, patch: Partial<TournamentTemplate>): void {
  const old = templates.value.find(x => x.id === id);
  if (!old) return;
  const changed: TournamentTemplate = { ...old, ...patch, id, builtIn: old.builtIn, format: old.builtIn ? old.format : patch.format ?? old.format };
  userTemplates.value = userTemplates.value.some(x => x.id === id)
    ? userTemplates.value.map(x => x.id === id ? changed : x)
    : [...userTemplates.value, changed];
  void save(changed);
}

export function resetTemplate(id: string): void {
  if (!builtInTemplate(id)) return;
  userTemplates.value = userTemplates.value.filter(x => x.id !== id);
  void ready.then(() => storage?.deleteTemplate(id)).catch(() => {});
}

export function deleteTemplate(id: string): void {
  const list = templates.value;
  const i = list.findIndex(x => x.id === id);
  if (i < 0 || list[i].builtIn) return;
  userTemplates.value = userTemplates.value.filter(x => x.id !== id);
  if (order.value.includes(id)) setOrder(order.value.filter(x => x !== id));
  if (selectedTemplateId.value === id) selectedTemplateId.value = (list[i + 1] ?? list[i - 1]).id;
  void ready.then(() => storage?.deleteTemplate(id)).catch(() => {});
}

/** Adds imported templates (ids and names already unique, app/lib/library.ts). */
export function addTemplates(list: readonly TournamentTemplate[]): void {
  if (!list.length) return;
  userTemplates.value = [...userTemplates.value, ...list];
  for (const x of list) void save(x);
}

export function templateNames(): string[] { return takenNames(); }
