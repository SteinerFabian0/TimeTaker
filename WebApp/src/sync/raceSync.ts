/*
 * A second window (the TV) as a pure view of the race state
 * (SYSTEM_ARCHITECTURE §11): the control window folds, the TV window only
 * shows what it receives over a BroadcastChannel.
 *
 * Messages on channel "timetaker-race" (structured clone, so bigint is fine):
 *   { t: 'state', from, version, raceId, state }   publisher → all
 *   { t: 'hello', from }                           a new subscriber asks for the latest state
 *   { t: 'bye', from }                             a publisher closes
 *
 * - The publisher numbers its states; subscribers drop anything older than
 *   what they have from that publisher (a late message never rewinds a view).
 * - A subscriber that opens late sends 'hello' and gets the latest state at
 *   once, so a TV window opened mid-race is right immediately.
 * - With two publishers (two control windows), subscribers follow the one
 *   that spoke last.
 * Works with anything shaped like BroadcastChannel (FakeChannelHub in tests).
 */

import type { RaceState } from '../race/types.ts';

export const RACE_CHANNEL = 'timetaker-race';

export interface ChannelLike {
  postMessage(msg: unknown): void;
  onmessage: ((ev: { data: unknown }) => void) | null;
  close(): void;
}

export type SyncMessage =
  | { t: 'state'; from: string; version: number; raceId: string; state: RaceState }
  | { t: 'hello'; from: string }
  | { t: 'bye'; from: string };

function newId(): string {
  const b = new Uint8Array(6);
  globalThis.crypto.getRandomValues(b);
  return Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');
}

export function browserChannel(name = RACE_CHANNEL): ChannelLike {
  return new BroadcastChannel(name) as unknown as ChannelLike;
}

export class RaceStatePublisher {
  readonly id: string;
  #ch: ChannelLike;
  #version = 0;
  #last: SyncMessage | null = null;

  constructor(channel: ChannelLike, id: string = newId()) {
    this.id = id;
    this.#ch = channel;
    this.#ch.onmessage = (ev) => {
      const m = ev.data as SyncMessage;
      if (m?.t === 'hello' && this.#last) this.#ch.postMessage(this.#last);
    };
  }

  publish(state: RaceState): void {
    this.#last = { t: 'state', from: this.id, version: ++this.#version, raceId: state.raceId, state };
    this.#ch.postMessage(this.#last);
  }

  close(): void {
    this.#ch.postMessage({ t: 'bye', from: this.id });
    this.#ch.onmessage = null;
    this.#ch.close();
  }
}

export class RaceStateSubscriber {
  readonly id: string;
  #ch: ChannelLike;
  #seen = new Map<string, number>();
  #state: RaceState | null = null;
  #source: string | null = null;
  #onState: (state: RaceState, source: string) => void;

  constructor(channel: ChannelLike, onState: (state: RaceState, source: string) => void, id: string = newId()) {
    this.id = id;
    this.#ch = channel;
    this.#onState = onState;
    this.#ch.onmessage = (ev) => this.#on(ev.data as SyncMessage);
    this.#ch.postMessage({ t: 'hello', from: id });
  }

  get state(): RaceState | null { return this.#state; }
  get source(): string | null { return this.#source; }

  #on(m: SyncMessage): void {
    if (!m || typeof m !== 'object') return;
    if (m.t === 'bye') { this.#seen.delete(m.from); return; }
    if (m.t !== 'state') return;
    if ((this.#seen.get(m.from) ?? 0) >= m.version) return;
    this.#seen.set(m.from, m.version);
    this.#state = m.state;
    this.#source = m.from;
    this.#onState(m.state, m.from);
  }

  close(): void {
    this.#ch.onmessage = null;
    this.#ch.close();
  }
}

/**
 * In-memory BroadcastChannel stand-in: every channel of the hub gets every
 * message but its own, asynchronously, through structuredClone (as the
 * browser does). deliver() can reorder for tests.
 */
export class FakeChannelHub {
  #channels = new Set<ChannelLike>();
  #queue: { to: ChannelLike; data: unknown }[] = [];
  manual = false;

  channel(): ChannelLike {
    const hub = this;
    const ch: ChannelLike = {
      onmessage: null,
      postMessage(msg: unknown) { hub.#post(ch, msg); },
      close() { hub.#channels.delete(ch); },
    };
    this.#channels.add(ch);
    return ch;
  }

  get queued(): number { return this.#queue.length; }

  /** Delivers queued messages (in manual mode), optionally in another order. */
  deliver(order?: (q: { to: ChannelLike; data: unknown }[]) => { to: ChannelLike; data: unknown }[]): void {
    const q = this.#queue.splice(0);
    for (const m of order ? order(q) : q) if (this.#channels.has(m.to)) m.to.onmessage?.({ data: m.data });
  }

  #post(from: ChannelLike, msg: unknown): void {
    for (const to of this.#channels) {
      if (to === from) continue;
      const data = structuredClone(msg);
      if (this.manual) this.#queue.push({ to, data });
      else queueMicrotask(() => { if (this.#channels.has(to)) to.onmessage?.({ data }); });
    }
  }
}
