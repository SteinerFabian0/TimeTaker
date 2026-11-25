/*
 * The library: the user's presets (and edited shipped ones), tournament
 * templates (likewise) and the track's settings. Later also drivers.
 *
 *   db "timetaker-library" v3
 *     presets   key id
 *     templates key id
 *     settings  out-of-line key (e.g. "trackTiming"), any structured value
 *
 * A database of its own, apart from the event log ("timetaker"), so the
 * library (presets, later templates and drivers) can grow without touching
 * race data. MemoryPresetStorage has the same contract and is what the app
 * falls back to when IndexedDB cannot be opened (private window, blocked
 * storage): the app still works, it just forgets on reload.
 */

import { normalizePreset, type RacePreset } from '../presets/preset.ts';
import { normalizeTemplate, type TournamentTemplate } from '../tournaments/template.ts';

export interface LibraryStorage {
  /** Every stored preset, normalized; unreadable records are skipped. */
  list(): Promise<RacePreset[]>;
  put(p: RacePreset): Promise<void>;
  delete(id: string): Promise<void>;
  /** Every stored tournament template, normalized; unreadable records are skipped. */
  listTemplates(): Promise<TournamentTemplate[]>;
  putTemplate(t: TournamentTemplate): Promise<void>;
  deleteTemplate(id: string): Promise<void>;
  /** A stored setting as it was stored (callers normalize), or undefined. */
  getSetting(key: string): Promise<unknown>;
  putSetting(key: string, value: unknown): Promise<void>;
  deleteSetting(key: string): Promise<void>;
  /** False for the in-memory fallback. */
  readonly persistent: boolean;
}

export class MemoryLibraryStorage implements LibraryStorage {
  readonly persistent = false;
  #items = new Map<string, unknown>();
  #settings = new Map<string, unknown>();
  #templates = new Map<string, unknown>();

  async getSetting(key: string): Promise<unknown> { return structuredClone(this.#settings.get(key)); }
  async putSetting(key: string, value: unknown): Promise<void> { this.#settings.set(key, structuredClone(value)); }
  async deleteSetting(key: string): Promise<void> { this.#settings.delete(key); }

  async list(): Promise<RacePreset[]> {
    return [...this.#items.values()].map(normalizePreset).filter((p): p is RacePreset => p !== null);
  }
  async put(p: RacePreset): Promise<void> { this.#items.set(p.id, structuredClone(p)); }
  async delete(id: string): Promise<void> { this.#items.delete(id); }

  async listTemplates(): Promise<TournamentTemplate[]> {
    return [...this.#templates.values()].map(normalizeTemplate).filter((t): t is TournamentTemplate => t !== null);
  }
  async putTemplate(t: TournamentTemplate): Promise<void> { this.#templates.set(t.id, structuredClone(t)); }
  async deleteTemplate(id: string): Promise<void> { this.#templates.delete(id); }
}

const DB_NAME = 'timetaker-library';
const DB_VERSION = 3;
const STORE = 'presets';
const SETTINGS = 'settings';
const TEMPLATES = 'templates';

function req<T>(r: IDBRequest<T>): Promise<T> {
  return new Promise((resolve, reject) => {
    r.onsuccess = () => resolve(r.result);
    r.onerror = () => reject(r.error);
  });
}

export class IndexedDbLibraryStorage implements LibraryStorage {
  readonly persistent = true;
  #db: IDBDatabase;

  constructor(db: IDBDatabase) { this.#db = db; }

  static async open(): Promise<IndexedDbLibraryStorage> {
    const r = indexedDB.open(DB_NAME, DB_VERSION);
    r.onupgradeneeded = () => {
      const db = r.result;
      if (!db.objectStoreNames.contains(STORE)) db.createObjectStore(STORE, { keyPath: 'id' });
      if (!db.objectStoreNames.contains(SETTINGS)) db.createObjectStore(SETTINGS);
      if (!db.objectStoreNames.contains(TEMPLATES)) db.createObjectStore(TEMPLATES, { keyPath: 'id' });
    };
    return new IndexedDbLibraryStorage(await req(r));
  }

  async getSetting(key: string): Promise<unknown> {
    return req(this.#db.transaction(SETTINGS).objectStore(SETTINGS).get(key));
  }
  async putSetting(key: string, value: unknown): Promise<void> {
    await req(this.#db.transaction(SETTINGS, 'readwrite').objectStore(SETTINGS).put(value, key));
  }
  async deleteSetting(key: string): Promise<void> {
    await req(this.#db.transaction(SETTINGS, 'readwrite').objectStore(SETTINGS).delete(key));
  }

  async list(): Promise<RacePreset[]> {
    const all = await req(this.#db.transaction(STORE).objectStore(STORE).getAll());
    return all.map(normalizePreset).filter((p): p is RacePreset => p !== null);
  }
  async put(p: RacePreset): Promise<void> {
    await req(this.#db.transaction(STORE, 'readwrite').objectStore(STORE).put(p));
  }
  async delete(id: string): Promise<void> {
    await req(this.#db.transaction(STORE, 'readwrite').objectStore(STORE).delete(id));
  }

  async listTemplates(): Promise<TournamentTemplate[]> {
    const all = await req(this.#db.transaction(TEMPLATES).objectStore(TEMPLATES).getAll());
    return all.map(normalizeTemplate).filter((t): t is TournamentTemplate => t !== null);
  }
  async putTemplate(t: TournamentTemplate): Promise<void> {
    await req(this.#db.transaction(TEMPLATES, 'readwrite').objectStore(TEMPLATES).put(t));
  }
  async deleteTemplate(id: string): Promise<void> {
    await req(this.#db.transaction(TEMPLATES, 'readwrite').objectStore(TEMPLATES).delete(id));
  }
}

let opened: Promise<LibraryStorage> | null = null;

/** IndexedDB if it opens, else memory. One shared instance per page. */
export function openLibraryStorage(): Promise<LibraryStorage> {
  opened ??= (async () => {
    try {
      if (typeof indexedDB !== 'undefined') return await IndexedDbLibraryStorage.open();
    } catch { /* fall back */ }
    return new MemoryLibraryStorage();
  })();
  return opened;
}
