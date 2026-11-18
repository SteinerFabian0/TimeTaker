/*
 * The app's long-term memory (SYSTEM_ARCHITECTURE §7.4: "long-term history
 * is the app's job, not the device's").
 *
 * - Device events: append-only, keyed by (bootId, evSeq), deduplicated, in
 *   log order: boots in the order this app first saw them (bootIds are
 *   random, so the store gives each boot an ordinal), evSeq within a boot.
 * - Host actions: append-only per race (corrections are new actions, never
 *   edits; undo is an action too).
 * - Races: id, config, and where in the device log the race was armed.
 *
 * Behind a small async storage interface: MemoryStorage here (tests, the
 * CLI), IndexedDbStorage in the browser. EventStore keeps an in-memory index
 * so appends are synchronous for the caller; writes are queued in order.
 */

import type { DeviceEvent, ResumePoint } from '../device/deviceSession.ts';
import type { HostAction, LogPos, RaceConfig, RaceInputEvent } from '../race/types.ts';

export interface StoredEvent extends RaceInputEvent {
  /** Boot ordinal: order of first appearance. */
  readonly ord: number;
  readonly name: string;
}

export interface BootRecord {
  readonly bootId: number;
  readonly ord: number;
}

export interface RaceRecord {
  readonly id: string;
  readonly config: RaceConfig;
  /** First device event that belongs to the race (inclusive), null = from the beginning of the log. */
  readonly from: LogPos | null;
  /** Informational: wall clock when the race was created. */
  readonly createdAt: string;
}

export interface EventStorage {
  putEvents(events: readonly StoredEvent[]): Promise<void>;
  getEvents(): Promise<StoredEvent[]>;
  putBoot(b: BootRecord): Promise<void>;
  getBoots(): Promise<BootRecord[]>;
  putAction(raceId: string, a: HostAction): Promise<void>;
  getActions(raceId: string): Promise<HostAction[]>;
  putRace(r: RaceRecord): Promise<void>;
  getRaces(): Promise<RaceRecord[]>;
  clear(): Promise<void>;
}

const key = (bootId: number, evSeq: number): string => `${bootId >>> 0}:${evSeq}`;

export class MemoryStorage implements EventStorage {
  events = new Map<string, StoredEvent>();
  boots = new Map<number, BootRecord>();
  actions = new Map<string, HostAction[]>();
  races = new Map<string, RaceRecord>();

  async putEvents(events: readonly StoredEvent[]): Promise<void> {
    for (const e of events) if (!this.events.has(key(e.bootId, e.evSeq))) this.events.set(key(e.bootId, e.evSeq), e);
  }
  async getEvents(): Promise<StoredEvent[]> {
    return [...this.events.values()].sort((a, b) => a.ord - b.ord || a.evSeq - b.evSeq);
  }
  async putBoot(b: BootRecord): Promise<void> { if (!this.boots.has(b.bootId)) this.boots.set(b.bootId, b); }
  async getBoots(): Promise<BootRecord[]> { return [...this.boots.values()].sort((a, b) => a.ord - b.ord); }
  async putAction(raceId: string, a: HostAction): Promise<void> {
    const list = this.actions.get(raceId) ?? [];
    if (!list.some((x) => x.id === a.id)) list.push(a);
    this.actions.set(raceId, list);
  }
  async getActions(raceId: string): Promise<HostAction[]> { return [...(this.actions.get(raceId) ?? [])].sort((a, b) => a.seq - b.seq); }
  async putRace(r: RaceRecord): Promise<void> { this.races.set(r.id, r); }
  async getRaces(): Promise<RaceRecord[]> { return [...this.races.values()]; }
  async clear(): Promise<void> { this.events.clear(); this.boots.clear(); this.actions.clear(); this.races.clear(); }
}

export interface RaceInput {
  readonly config: RaceConfig;
  readonly events: StoredEvent[];
  readonly actions: HostAction[];
}

export type ActionDraft = HostAction extends infer A ? A extends HostAction ? Omit<A, 'id' | 'seq'> & { id?: string } : never : never;

export class EventStore {
  #storage: EventStorage;
  #events: StoredEvent[] = [];
  #keys = new Set<string>();
  #boots = new Map<number, number>();
  #actions = new Map<string, HostAction[]>();
  #races = new Map<string, RaceRecord>();
  #queue: Promise<void> = Promise.resolve();
  #writeError: unknown = null;
  #listeners = new Set<() => void>();

  constructor(storage: EventStorage = new MemoryStorage()) {
    this.#storage = storage;
  }

  /** Loads what the storage holds. Call once before use. */
  async open(): Promise<this> {
    for (const b of await this.#storage.getBoots()) this.#boots.set(b.bootId, b.ord);
    this.#events = await this.#storage.getEvents();
    for (const e of this.#events) this.#keys.add(key(e.bootId, e.evSeq));
    for (const r of await this.#storage.getRaces()) {
      this.#races.set(r.id, r);
      this.#actions.set(r.id, await this.#storage.getActions(r.id));
    }
    return this;
  }

  /** Resolves when every queued write is stored; rejects with the first write error. */
  async flush(): Promise<void> {
    await this.#queue;
    if (this.#writeError) throw this.#writeError;
  }

  /** Called after every change (views re-fold). Returns an unsubscribe function. */
  subscribe(fn: () => void): () => void {
    this.#listeners.add(fn);
    return () => this.#listeners.delete(fn);
  }

  // ---- device events ---------------------------------------------------------

  /** Appends a device event; returns false for a duplicate. */
  appendEvent(e: DeviceEvent | RaceInputEvent): boolean {
    const k = key(e.bootId, e.evSeq);
    if (this.#keys.has(k)) return false;
    let ord = this.#boots.get(e.bootId);
    if (ord === undefined) {
      ord = this.#boots.size;
      this.#boots.set(e.bootId, ord);
      const rec = { bootId: e.bootId, ord };
      this.#write(() => this.#storage.putBoot(rec));
    }
    const stored: StoredEvent = { bootId: e.bootId, evSeq: e.evSeq, msgId: e.msgId, name: e.name ?? '', value: e.value, ord };
    this.#keys.add(k);
    // Usually appended in order; an out-of-order append (import) is inserted in place.
    const last = this.#events[this.#events.length - 1];
    if (!last || last.ord < ord || (last.ord === ord && last.evSeq < e.evSeq)) this.#events.push(stored);
    else {
      const i = this.#events.findIndex((x) => x.ord > ord || (x.ord === ord && x.evSeq > e.evSeq));
      this.#events.splice(i < 0 ? this.#events.length : i, 0, stored);
    }
    this.#write(() => this.#storage.putEvents([stored]));
    this.#notify();
    return true;
  }

  get events(): readonly StoredEvent[] { return this.#events; }

  /** The last stored device event. */
  position(): LogPos | null {
    const e = this.#events[this.#events.length - 1];
    return e ? { bootId: e.bootId, evSeq: e.evSeq } : null;
  }

  /** Where a DeviceSession should RESUME: after the newest boot's last stored event. */
  resumePoint(): ResumePoint | null {
    const p = this.position();
    return p ? { bootId: p.bootId, nextEvSeq: p.evSeq + 1 } : null;
  }

  /** Device events from a position on (inclusive), in log order. */
  eventsFrom(from: LogPos | null): StoredEvent[] {
    if (!from) return this.#events.slice();
    const ord = this.#boots.get(from.bootId);
    if (ord === undefined) return [];
    return this.#events.filter((e) => e.ord > ord || (e.ord === ord && e.evSeq >= from.evSeq));
  }

  // ---- races and host actions ---------------------------------------------------

  createRace(config: RaceConfig, from: LogPos | null = this.#nextPos(), createdAt = new Date().toISOString()): RaceRecord {
    if (this.#races.has(config.id)) throw new Error(`race ${config.id} exists`);
    const r: RaceRecord = { id: config.id, config, from, createdAt };
    this.#races.set(r.id, r);
    this.#actions.set(r.id, []);
    this.#write(() => this.#storage.putRace(r));
    this.#notify();
    return r;
  }

  /** Replaces a race's config (before it starts, typically). The device log is untouched. */
  updateRace(id: string, config: RaceConfig): void {
    const r = this.race(id);
    const next = { ...r, config };
    this.#races.set(id, next);
    this.#write(() => this.#storage.putRace(next));
    this.#notify();
  }

  race(id: string): RaceRecord {
    const r = this.#races.get(id);
    if (!r) throw new Error(`no race ${id}`);
    return r;
  }

  races(): RaceRecord[] { return [...this.#races.values()]; }

  /** Appends a host action; id and seq are assigned unless given. */
  appendAction(raceId: string, draft: ActionDraft, newId: () => string = randomId): HostAction {
    this.race(raceId);
    const list = this.#actions.get(raceId)!;
    const action = { ...draft, id: draft.id ?? newId(), seq: (list.at(-1)?.seq ?? 0) + 1 } as HostAction;
    if (list.some((a) => a.id === action.id)) throw new Error(`action ${action.id} exists`);
    list.push(action);
    this.#write(() => this.#storage.putAction(raceId, action));
    this.#notify();
    return action;
  }

  actions(raceId: string): HostAction[] { return (this.#actions.get(raceId) ?? []).slice(); }

  /** Everything raceEngine.fold needs for one race. */
  raceInput(raceId: string): RaceInput {
    const r = this.race(raceId);
    return { config: r.config, events: this.eventsFrom(r.from), actions: this.actions(raceId) };
  }

  /** The position the next device event will most likely get (where a new race begins). */
  #nextPos(): LogPos | null {
    const p = this.position();
    return p ? { bootId: p.bootId, evSeq: p.evSeq + 1 } : null;
  }

  #write(op: () => Promise<void>): void {
    this.#queue = this.#queue.then(op).catch((err: unknown) => { this.#writeError ??= err; });
  }

  #notify(): void {
    for (const fn of this.#listeners) fn();
  }
}

export function randomId(): string {
  const b = new Uint8Array(8);
  globalThis.crypto.getRandomValues(b);
  return Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');
}
