/*
 * EventStorage on IndexedDB (browser only; thin on purpose). Structured
 * clone keeps bigint and Uint8Array, so events are stored as decoded.
 *
 *   db "timetaker" v1
 *     events   key [ord, evSeq]   (log order is key order)
 *     boots    key bootId
 *     actions  key [raceId, seq]
 *     races    key id
 *
 * Not covered by the Node tests (no IndexedDB there); MemoryStorage has the
 * same contract and is. Opening never happens at import time.
 */

import type { HostAction } from '../race/types.ts';
import type { BootRecord, EventStorage, RaceRecord, StoredEvent } from './eventStore.ts';

const DB_NAME = 'timetaker';
const DB_VERSION = 1;

function req<T>(r: IDBRequest<T>): Promise<T> {
  return new Promise((resolve, reject) => {
    r.onsuccess = () => resolve(r.result);
    r.onerror = () => reject(r.error);
  });
}

function done(tx: IDBTransaction): Promise<void> {
  return new Promise((resolve, reject) => {
    tx.oncomplete = () => resolve();
    tx.onerror = () => reject(tx.error);
    tx.onabort = () => reject(tx.error);
  });
}

export function isIndexedDbAvailable(): boolean {
  return typeof indexedDB !== 'undefined';
}

export class IndexedDbStorage implements EventStorage {
  #db: IDBDatabase;

  /** Use IndexedDbStorage.open(). */
  constructor(db: IDBDatabase) {
    this.#db = db;
  }

  static async open(name = DB_NAME): Promise<IndexedDbStorage> {
    const r = indexedDB.open(name, DB_VERSION);
    r.onupgradeneeded = () => {
      const db = r.result;
      if (!db.objectStoreNames.contains('events')) db.createObjectStore('events', { keyPath: ['ord', 'evSeq'] });
      if (!db.objectStoreNames.contains('boots')) db.createObjectStore('boots', { keyPath: 'bootId' });
      if (!db.objectStoreNames.contains('actions')) db.createObjectStore('actions', { keyPath: ['raceId', 'seq'] });
      if (!db.objectStoreNames.contains('races')) db.createObjectStore('races', { keyPath: 'id' });
    };
    return new IndexedDbStorage(await req(r));
  }

  close(): void { this.#db.close(); }

  async putEvents(events: readonly StoredEvent[]): Promise<void> {
    const tx = this.#db.transaction('events', 'readwrite');
    const st = tx.objectStore('events');
    for (const e of events) st.put(e);
    await done(tx);
  }

  async getEvents(): Promise<StoredEvent[]> {
    return req(this.#db.transaction('events').objectStore('events').getAll()) as Promise<StoredEvent[]>;
  }

  async putBoot(b: BootRecord): Promise<void> {
    const tx = this.#db.transaction('boots', 'readwrite');
    tx.objectStore('boots').put(b);
    await done(tx);
  }

  async getBoots(): Promise<BootRecord[]> {
    const all = (await req(this.#db.transaction('boots').objectStore('boots').getAll())) as BootRecord[];
    return all.sort((a, b) => a.ord - b.ord);
  }

  async putAction(raceId: string, a: HostAction): Promise<void> {
    const tx = this.#db.transaction('actions', 'readwrite');
    tx.objectStore('actions').put({ ...a, raceId });
    await done(tx);
  }

  async getActions(raceId: string): Promise<HostAction[]> {
    const range = IDBKeyRange.bound([raceId, -Infinity], [raceId, Infinity]);
    const all = (await req(this.#db.transaction('actions').objectStore('actions').getAll(range))) as (HostAction & { raceId?: string })[];
    return all.map(({ raceId: _r, ...a }) => a as HostAction);
  }

  async putRace(r: RaceRecord): Promise<void> {
    const tx = this.#db.transaction('races', 'readwrite');
    tx.objectStore('races').put(r);
    await done(tx);
  }

  async getRaces(): Promise<RaceRecord[]> {
    return req(this.#db.transaction('races').objectStore('races').getAll()) as Promise<RaceRecord[]>;
  }

  async clear(): Promise<void> {
    const tx = this.#db.transaction(['events', 'boots', 'actions', 'races'], 'readwrite');
    for (const s of ['events', 'boots', 'actions', 'races']) tx.objectStore(s).clear();
    await done(tx);
  }
}
