/*
 * Time sources for everything that waits: the device session (request
 * timeouts, PING), the simulator (lane joins, cars, start lights), the
 * updater and the bench suites.
 *
 * Two implementations of one small interface:
 *
 *   realScheduler   performance.now() and the global timers. The browser, the
 *                   CLI against hardware, the debug console.
 *   VirtualClock    time only moves when told to. Tests and the race-fixture
 *                   generator run a 3-minute race in well under a second, and
 *                   the same seed gives the same bytes every time.
 *
 * Nothing here is used for results: race results come from device timestamps
 * only (SYSTEM_ARCHITECTURE §3). This is about *when code runs*.
 */

export type TimerHandle = { readonly __timer: true } | number | object;

export interface Scheduler {
  /** Milliseconds on a monotonic clock (fractions allowed). */
  now(): number;
  setTimeout(fn: () => void, ms: number): TimerHandle;
  clearTimeout(handle: TimerHandle | null | undefined): void;
  setInterval(fn: () => void, ms: number): TimerHandle;
  clearInterval(handle: TimerHandle | null | undefined): void;
}

export const realScheduler: Scheduler = {
  now: () => performance.now(),
  setTimeout: (fn, ms) => globalThis.setTimeout(fn, ms) as unknown as TimerHandle,
  clearTimeout: (h) => { if (h !== null && h !== undefined) globalThis.clearTimeout(h as ReturnType<typeof setTimeout>); },
  setInterval: (fn, ms) => globalThis.setInterval(fn, ms) as unknown as TimerHandle,
  clearInterval: (h) => { if (h !== null && h !== undefined) globalThis.clearInterval(h as ReturnType<typeof setInterval>); },
};

/** Sleep on a scheduler. */
export function sleep(s: Scheduler, ms: number): Promise<void> {
  return new Promise((resolve) => { s.setTimeout(resolve, ms); });
}

/**
 * Wait until cond() is true, checking every stepMs. Rejects after timeoutMs
 * with `what` in the message.
 */
export async function waitFor(s: Scheduler, cond: () => boolean, timeoutMs: number, what = 'condition',
                              stepMs = 5): Promise<void> {
  const end = s.now() + timeoutMs;
  while (!cond()) {
    if (s.now() >= end) throw new Error(`timed out after ${timeoutMs} ms waiting for ${what}`);
    await sleep(s, stepMs);
  }
}

/** Yields one macrotask so every pending promise continuation runs first. */
function macrotask(): Promise<void> {
  const g = globalThis as { setImmediate?: (fn: () => void) => unknown };
  if (typeof g.setImmediate === 'function') return new Promise((r) => { g.setImmediate!(r); });
  return new Promise((r) => { globalThis.setTimeout(r, 0); });
}

interface VTimer {
  readonly id: number;
  at: number;
  readonly fn: () => void;
  readonly every: number | null;
}

/**
 * A clock that moves only when advanced. Timers fire in time order (ties in
 * the order they were set). Between two timers every pending promise
 * continuation runs, so async code behaves as it would in real time.
 */
export class VirtualClock implements Scheduler {
  #now: number;
  #nextId = 1;
  #timers = new Map<number, VTimer>();
  #auto = false;
  #autoDone: Promise<void> | null = null;

  constructor(startMs = 1000) {
    this.#now = startMs;
  }

  now(): number { return this.#now; }

  setTimeout(fn: () => void, ms: number): TimerHandle {
    return this.#add(fn, ms, null);
  }

  setInterval(fn: () => void, ms: number): TimerHandle {
    return this.#add(fn, ms, Math.max(ms, 0.001));
  }

  clearTimeout(handle: TimerHandle | null | undefined): void {
    if (typeof handle === 'number') this.#timers.delete(handle);
  }

  clearInterval(handle: TimerHandle | null | undefined): void {
    this.clearTimeout(handle);
  }

  /** Number of timers waiting. */
  get pending(): number { return this.#timers.size; }

  /** Lets pending promise continuations run without moving time. */
  async settle(rounds = 3): Promise<void> {
    for (let i = 0; i < rounds; i++) await macrotask();
  }

  /** Moves time forward by ms, firing every timer due on the way. */
  async advance(ms: number): Promise<void> {
    await this.advanceTo(this.#now + ms);
  }

  async advanceTo(target: number): Promise<void> {
    await this.settle();
    for (;;) {
      const next = this.#earliest();
      if (!next || next.at > target) break;
      this.#now = Math.max(this.#now, next.at);
      if (next.every !== null) next.at += next.every;
      else this.#timers.delete(next.id);
      next.fn();
      await this.settle(2);
    }
    this.#now = Math.max(this.#now, target);
    await this.settle();
  }

  /**
   * Runs time as fast as the code allows: whenever nothing else is pending,
   * jumps to the next timer. Stop with stopAuto(). For suites and generators
   * that `await sleep()` on this clock from the outside.
   */
  startAuto(maxMs = Number.POSITIVE_INFINITY): void {
    if (this.#auto) return;
    this.#auto = true;
    const limit = this.#now + maxMs;
    this.#autoDone = (async () => {
      while (this.#auto) {
        await this.settle(2);
        const next = this.#earliest();
        if (!next) { await macrotask(); continue; }
        if (next.at > limit) { this.#auto = false; break; }
        await this.advanceTo(next.at);
      }
    })();
  }

  async stopAuto(): Promise<void> {
    this.#auto = false;
    await this.#autoDone;
    this.#autoDone = null;
  }

  /** Runs fn with the clock advancing automatically, then stops it. */
  async run<T>(fn: () => Promise<T>, maxMs?: number): Promise<T> {
    this.startAuto(maxMs);
    try {
      return await fn();
    } finally {
      await this.stopAuto();
    }
  }

  #add(fn: () => void, ms: number, every: number | null): number {
    const id = this.#nextId++;
    this.#timers.set(id, { id, at: this.#now + Math.max(0, ms), fn, every });
    return id;
  }

  #earliest(): VTimer | undefined {
    let best: VTimer | undefined;
    for (const t of this.#timers.values()) if (!best || t.at < best.at || (t.at === best.at && t.id < best.id)) best = t;
    return best;
  }
}
