/*
 * HealthMonitor: the status & diagnostics poller behind the status page.
 *
 *   STATUS      every statusIntervalMs (2 s); NODE_LIST only when something
 *               changed (connect, boot, a lane event, lanesPresent differs
 *               from the model).
 *   LANE_STATUS THE ONE poller of LANE_STATUS in the app: rawMin/rawMax are
 *               "since the previous LANE_STATUS of that lane" (reset by every
 *               read), so a second reader would steal the window. Every read
 *               (timer, runSelfCheck) goes through one per-lane queue: at
 *               most one LANE_STATUS per lane in flight, each answer becomes
 *               a sample. Rate per present lane: laneRateHz (4 Hz) while
 *               idle; busyLaneRateHz (0.2 Hz, 0 = off) while a race or a
 *               start sequence runs (busy(): the director, or seqRunning).
 *   history     a ring per lane of samples {tMs, raw, rawMin, rawMax,
 *               baseline, thrLow, thrHigh, reference, noise, flags} covering
 *               historyMs (60 s), plus markers (calibration outcomes, lane
 *               joined/configured/left/fault) on the same time axis (tMs =
 *               the scheduler's ms). A GUI draws a small graph per lane.
 *   calibrate   calibrate(lanes) / forget(lanes): refused while busy();
 *               sends LANE_CALIBRATE once and resolves with each lane's
 *               EVT_LANE_CALIBRATED (no event within calibrateTimeoutMs,
 *               15 s → 'failed', timedOut). SAVED follows the device's
 *               deferred flash write (up to ~10 s); the lane uses the new
 *               reference at once, so the samples show it before the event.
 *               LEARN: lanes not present → 'failed' without asking (the
 *               device skips them silently). FORGET: every requested index,
 *               present or not (the device clears the flash entry). The
 *               first LANE_STATUS after a calibration covers a shorter
 *               rawMin/rawMax window (the device read SIGNAL itself).
 *   report      evaluate() (health.ts) over the latest data; runSelfCheck()
 *               polls everything fresh first.
 *   subscribe   updates for the GUI: sample, marker, report, calibration,
 *               connection.
 *
 * Wiring: HealthMonitor.attach(manager, { scheduler, busy: () => director.active() })
 * subscribes to the DeviceManager's events and connection state. Without a
 * manager (CLI, tests) feed onEvent() / setConnected() yourself.
 *
 * TODO highRate(lane): a short LANE_RAW_STREAM (STR_RAW_SAMPLES) burst for a
 * full-rate trace of one lane. It needs the stream to be owned here too
 * (one lane at a time device-wide) and on BLE it is capped at 2000 B/s; left
 * out until the GUI asks for it.
 */

import type { DeviceApi } from '../device/deviceApi.ts';
import { laneMask } from '../device/deviceApi.ts';
import type { DeviceManager } from '../device/deviceManager.ts';
import type { DeviceEvent } from '../device/deviceSession.ts';
import { applyNodeEvent, applyNodeList, applyStatus, calibrationResultOf, emptyNodeModel, presentLanes } from '../device/nodeModel.ts';
import type { LaneCalibration, NodeModel } from '../device/nodeModel.ts';
import { CalibrateAction, LaneState, MsgId } from '../protocol/generated.ts';
import type { LaneStatusRsp, StatusRsp } from '../protocol/generated.ts';
import { realScheduler } from '../util/clock.ts';
import type { Scheduler, TimerHandle } from '../util/clock.ts';
import { DEFAULT_THRESHOLDS, evaluate } from './health.ts';
import type { HealthReport, HealthThresholds } from './health.ts';

/** The DeviceApi methods the monitor uses (a fake can stand in). */
export type MonitorApi = Pick<DeviceApi, 'status' | 'nodeList' | 'laneStatus' | 'laneCalibrate'>;

export interface MonitorOptions {
  readonly api: MonitorApi;
  readonly scheduler?: Scheduler;
  /** A race or start sequence is under way (e.g. () => director.active()). seqRunning from the device counts too. */
  readonly busy?: () => boolean;
  /** The app's next evSeq of the current boot (event store position), for the event log check. */
  readonly position?: () => number | null;
  /** Lanes that must be there (race entries); lanes seen this session count anyway. */
  readonly expectedLanes?: () => readonly number[];
  readonly statusIntervalMs?: number;
  /** LANE_STATUS rate per present lane while idle. */
  readonly laneRateHz?: number;
  /** LANE_STATUS rate per present lane during a race / start sequence; 0 = off. */
  readonly busyLaneRateHz?: number;
  /** History kept per lane. */
  readonly historyMs?: number;
  /** Wait this long for each EVT_LANE_CALIBRATED. */
  readonly calibrateTimeoutMs?: number;
  readonly thresholds?: HealthThresholds;
}

export const MONITOR_DEFAULTS = {
  statusIntervalMs: 2000,
  laneRateHz: 4,
  busyLaneRateHz: 0.2,
  historyMs: 60_000,
  /** SAVED follows the deferred flash write: up to ~10 s after the ack (control firmware). */
  calibrateTimeoutMs: 15_000,
} as const;

export interface LaneSample {
  /** Host time (scheduler ms) the answer arrived. */
  readonly tMs: number;
  readonly raw: number;
  /** Min / max of every sample since the previous read. */
  readonly rawMin: number;
  readonly rawMax: number;
  readonly baseline: number;
  readonly thrLow: number;
  readonly thrHigh: number;
  /** 0 = none. */
  readonly reference: number;
  readonly noise: number;
  /** LANE_STATUS flags (LaneStatusFlag). */
  readonly flags: number;
}

export type MarkerKind = 'calibration' | 'joined' | 'configured' | 'left' | 'fault';

export interface LaneMarker {
  readonly tMs: number;
  readonly lane: number;
  readonly kind: MarkerKind;
  /** Short label for the graph ("saved 3412", "forgotten", "failed", "left", ...). */
  readonly label: string;
  /** kind 'calibration' only. */
  readonly calibration?: LaneCalibration;
}

export interface CalibrationOutcome extends LaneCalibration {
  readonly lane: number;
}

export type MonitorUpdate =
  | { readonly type: 'sample'; readonly lane: number; readonly sample: LaneSample }
  | { readonly type: 'marker'; readonly lane: number; readonly marker: LaneMarker }
  | { readonly type: 'report'; readonly report: HealthReport }
  | { readonly type: 'calibration'; readonly lane: number; readonly outcome: CalibrationOutcome }
  | { readonly type: 'connection'; readonly connected: boolean };

/** calibrate() / forget() while a race or start sequence runs. */
export class MonitorBusyError extends Error {
  constructor(what: string) {
    super(`${what} refused: a race or start sequence is running`);
    this.name = 'MonitorBusyError';
  }
}

/** Fixed-capacity ring of time-stamped items, pruned by age. */
export class Ring<T extends { readonly tMs: number }> {
  readonly capacity: number;
  #buf: (T | undefined)[];
  #head = 0;
  #size = 0;

  constructor(capacity: number) {
    this.capacity = Math.max(1, Math.floor(capacity));
    this.#buf = new Array<T | undefined>(this.capacity);
  }

  get size(): number { return this.#size; }

  push(item: T): void {
    const at = (this.#head + this.#size) % this.capacity;
    this.#buf[at] = item;
    if (this.#size < this.capacity) this.#size++;
    else this.#head = (this.#head + 1) % this.capacity;
  }

  /** Drops items older than tMs from the front. */
  pruneBefore(tMs: number): void {
    while (this.#size > 0 && this.#buf[this.#head]!.tMs < tMs) {
      this.#buf[this.#head] = undefined;
      this.#head = (this.#head + 1) % this.capacity;
      this.#size--;
    }
  }

  toArray(): T[] {
    const out: T[] = [];
    for (let i = 0; i < this.#size; i++) out.push(this.#buf[(this.#head + i) % this.capacity]!);
    return out;
  }
}

interface Waiter {
  resolve(c: LaneCalibration): void;
  timer: TimerHandle;
}

export class HealthMonitor {
  readonly #o: MonitorOptions;
  readonly #sched: Scheduler;
  readonly #t: HealthThresholds;
  readonly #rate: number;
  readonly #busyRate: number;
  readonly #historyMs: number;
  #connected = false;
  #running = false;
  #statusTimer: TimerHandle | null = null;
  #laneTimer: TimerHandle | null = null;
  #laneTickBusy = false;
  #status: StatusRsp | null = null;
  #statusAtMs: number | null = null;
  #statusFailed = false;
  #nodes: NodeModel = emptyNodeModel();
  #needNodeList = true;
  #seen = new Set<number>();
  #last: ({ rsp: LaneStatusRsp; atMs: number } | null)[] = new Array(7).fill(null);
  #lastPollMs: number[] = new Array<number>(7).fill(Number.NEGATIVE_INFINITY);
  #inflight: (Promise<LaneStatusRsp | null> | null)[] = new Array(7).fill(null);
  #samples: Ring<LaneSample>[];
  #markers: Ring<LaneMarker>[];
  #waiters = new Map<number, Waiter>();
  #calibrating = false;
  #subs = new Set<(u: MonitorUpdate) => void>();

  constructor(o: MonitorOptions) {
    this.#o = o;
    this.#sched = o.scheduler ?? realScheduler;
    this.#t = o.thresholds ?? DEFAULT_THRESHOLDS;
    this.#rate = o.laneRateHz ?? MONITOR_DEFAULTS.laneRateHz;
    this.#busyRate = o.busyLaneRateHz ?? MONITOR_DEFAULTS.busyLaneRateHz;
    this.#historyMs = o.historyMs ?? MONITOR_DEFAULTS.historyMs;
    const cap = Math.ceil((this.#historyMs / 1000) * Math.max(this.#rate, this.#busyRate, 0.1)) + 16;
    this.#samples = Array.from({ length: 7 }, () => new Ring<LaneSample>(cap));
    this.#markers = Array.from({ length: 7 }, () => new Ring<LaneMarker>(256));
  }

  /**
   * A monitor on a DeviceManager: its api, its events and connection state, its resume point as the app position.
   * Returns the monitor (started); monitor.stop() also unsubscribes.
   */
  static attach(manager: DeviceManager, o: Omit<MonitorOptions, 'api' | 'position'> & { position?: () => number | null }): HealthMonitor {
    const m = new HealthMonitor({ ...o, api: manager.api, position: o.position ?? (() => manager.resumePoint?.nextEvSeq ?? null) });
    m.#nodes = manager.nodes;
    m.#connected = manager.state === 'connected';
    const unsub = manager.subscribe({
      onEvent: (e) => m.onEvent(e),
      onState: (st) => m.setConnected(st === 'connected'),
    });
    m.#detach = unsub;
    m.start();
    return m;
  }

  #detach: (() => void) | null = null;

  // ---- lifecycle ------------------------------------------------------------------------

  get running(): boolean { return this.#running; }
  get connected(): boolean { return this.#connected; }
  get nodes(): NodeModel { return this.#nodes; }
  get status(): StatusRsp | null { return this.#status; }

  start(): void {
    if (this.#running) return;
    this.#running = true;
    const st = this.#o.statusIntervalMs ?? MONITOR_DEFAULTS.statusIntervalMs;
    this.#statusTimer = this.#sched.setInterval(() => { void this.#pollStatus(); }, st);
    const fast = Math.max(this.#rate, this.#busyRate);
    if (fast > 0) this.#laneTimer = this.#sched.setInterval(() => { void this.#laneTick(); }, 1000 / fast);
    void this.#pollStatus();
  }

  stop(): void {
    this.#running = false;
    this.#sched.clearInterval(this.#statusTimer);
    this.#sched.clearInterval(this.#laneTimer);
    this.#statusTimer = null;
    this.#laneTimer = null;
    this.#detach?.();
    this.#detach = null;
    for (const [lane, w] of this.#waiters) { this.#sched.clearTimeout(w.timer); w.resolve(this.#timedOut()); this.#waiters.delete(lane); }
  }

  subscribe(fn: (u: MonitorUpdate) => void): () => void {
    this.#subs.add(fn);
    return () => { this.#subs.delete(fn); };
  }

  #emit(u: MonitorUpdate): void {
    for (const fn of this.#subs) {
      try { fn(u); } catch { /* a broken subscriber must not stop polling */ }
    }
  }

  /** The session opened (true) or closed (false). */
  setConnected(connected: boolean): void {
    if (connected === this.#connected) return;
    this.#connected = connected;
    if (connected) { this.#needNodeList = true; this.#statusFailed = false; }
    this.#emit({ type: 'connection', connected });
    if (connected && this.#running) void this.#pollStatus();
    else this.#emit({ type: 'report', report: this.report() });
  }

  /** Every device event (in order). */
  onEvent(e: DeviceEvent): void {
    this.#nodes = applyNodeEvent(this.#nodes, e);
    const v = e.value;
    if (e.msgId === MsgId.EVT_BOOT) {
      this.#needNodeList = true;
      this.#last = new Array(7).fill(null);
      return;
    }
    if (!v) return;
    if (e.msgId === MsgId.EVT_LANE_STATE) {
      const lane = v['lane'] as number;
      if (lane < 1 || lane > 6) return;
      this.#needNodeList = true;
      const kind: MarkerKind | null = v['state'] === LaneState.JOINED ? 'joined' : v['state'] === LaneState.CONFIGURED ? 'configured'
        : v['state'] === LaneState.LEFT ? 'left' : v['state'] === LaneState.FAULT ? 'fault' : null;
      if (kind === 'joined' || kind === 'configured') this.#seen.add(lane);
      if (kind === 'left') this.#last[lane] = null;
      if (kind) this.#mark({ tMs: this.#sched.now(), lane, kind, label: kind });
      this.#emit({ type: 'report', report: this.report() });
      return;
    }
    if (e.msgId === MsgId.EVT_LANE_CALIBRATED) {
      const lane = v['lane'] as number;
      const cal = this.#nodes.lanes[lane]?.calibration;
      if (!cal) return;
      this.#onCalibration(lane, cal);
      const w = this.#waiters.get(lane);
      if (w) { this.#sched.clearTimeout(w.timer); this.#waiters.delete(lane); w.resolve(cal); }
    }
  }

  // ---- data --------------------------------------------------------------------------------

  /** Samples of the last historyMs, oldest first. */
  samples(lane: number): LaneSample[] {
    const r = this.#samples[lane];
    if (!r) return [];
    r.pruneBefore(this.#sched.now() - this.#historyMs);
    return r.toArray();
  }

  markers(lane: number): LaneMarker[] {
    const r = this.#markers[lane];
    if (!r) return [];
    r.pruneBefore(this.#sched.now() - this.#historyMs);
    return r.toArray();
  }

  lastLaneStatus(lane: number): LaneStatusRsp | null { return this.#last[lane]?.rsp ?? null; }

  /** busy(): race / start sequence running (director or device). */
  busy(): boolean {
    return (this.#o.busy?.() ?? false) || this.#nodes.seqRunning || this.#status?.seqRunning === 1;
  }

  /** The LANE_STATUS rate per lane right now (Hz). */
  laneRateHz(): number { return this.busy() ? this.#busyRate : this.#rate; }

  /** The report from the latest data (no device traffic). */
  report(): HealthReport {
    const now = this.#sched.now();
    const expected = new Set([...this.#seen, ...(this.#o.expectedLanes?.() ?? [])]);
    return evaluate({
      connected: this.#connected,
      status: this.#statusFailed ? null : this.#status,
      statusAgeMs: this.#statusAtMs === null || this.#statusFailed ? null : now - this.#statusAtMs,
      nodes: this.#nodes,
      laneStatus: this.#last.map((x) => x?.rsp ?? null),
      laneAgeMs: this.#last.map((x) => (x ? now - x.atMs : null)),
      expectedLanes: [...expected],
      appNextEvSeq: this.#o.position?.() ?? null,
    }, this.#t);
  }

  /** Polls everything fresh (STATUS, NODE_LIST, every present lane), then the report. */
  async runSelfCheck(): Promise<HealthReport> {
    this.#needNodeList = true;
    await this.#pollStatus();
    const lanes = presentLanes(this.#nodes);
    for (const lane of lanes) {
      await this.#inflight[lane];
      await this.#readLane(lane);
    }
    const r = this.report();
    this.#emit({ type: 'report', report: r });
    return r;
  }

  // ---- calibration -----------------------------------------------------------------------------

  /** LEARN and save the reference of these lanes ('all' = every present lane). Lanes must be free. */
  calibrate(lanes: readonly number[] | 'all' = 'all'): Promise<CalibrationOutcome[]> {
    return this.#calibrate(lanes, CalibrateAction.LEARN);
  }

  /** Drop the saved references (the lanes learn at every power-up again). */
  forget(lanes: readonly number[] | 'all' = 'all'): Promise<CalibrationOutcome[]> {
    return this.#calibrate(lanes, CalibrateAction.FORGET);
  }

  async #calibrate(lanes: readonly number[] | 'all', action: number): Promise<CalibrationOutcome[]> {
    const what = action === CalibrateAction.LEARN ? 'calibrate' : 'forget';
    if (this.busy()) throw new MonitorBusyError(what);
    if (this.#calibrating) throw new Error(`${what} refused: a calibration is running`);
    if (!this.#connected) throw new Error(`${what} refused: not connected`);
    const present = presentLanes(this.#nodes);
    const wanted = lanes === 'all' ? present : [...new Set(lanes)].sort((a, b) => a - b);
    laneMask(wanted);                                                       // range check
    const send = action === CalibrateAction.FORGET ? wanted : wanted.filter((l) => present.includes(l));
    const out = new Map<number, CalibrationOutcome>();
    for (const l of wanted) {
      if (!send.includes(l)) out.set(l, { lane: l, result: 'failed', reference: 0, noise: 0, sysUs: null, bootId: null });
    }
    if (send.length === 0) return wanted.map((l) => out.get(l)!);
    this.#calibrating = true;
    try {
      const timeoutMs = this.#o.calibrateTimeoutMs ?? MONITOR_DEFAULTS.calibrateTimeoutMs;
      const waits = send.map((lane) => new Promise<LaneCalibration>((resolve) => {
        const timer = this.#sched.setTimeout(() => {
          this.#waiters.delete(lane);
          const cal = this.#timedOut();
          const lanesM = this.#nodes.lanes.slice();
          lanesM[lane] = { ...lanesM[lane]!, calibration: cal };
          this.#nodes = { ...this.#nodes, lanes: lanesM };
          this.#onCalibration(lane, cal);
          resolve(cal);
        }, timeoutMs);
        this.#waiters.set(lane, { resolve, timer });
      }));
      try {
        await this.#o.api.laneCalibrate(laneMask(send), action as CalibrateAction);
      } catch (err) {
        for (const l of send) { const w = this.#waiters.get(l); if (w) this.#sched.clearTimeout(w.timer); this.#waiters.delete(l); }
        throw err;
      }
      const got = await Promise.all(waits);
      send.forEach((lane, i) => out.set(lane, { lane, ...got[i]! }));
    } finally {
      this.#calibrating = false;
    }
    this.#emit({ type: 'report', report: this.report() });
    return wanted.map((l) => out.get(l)!);
  }

  #timedOut(): LaneCalibration {
    return { result: 'failed', reference: 0, noise: 0, sysUs: null, bootId: null, timedOut: true };
  }

  #onCalibration(lane: number, cal: LaneCalibration): void {
    const label = cal.timedOut ? 'calibration timed out'
      : cal.result === 'saved' ? `saved ${cal.reference}` : cal.result === 'notSaved' ? `not saved ${cal.reference}` : cal.result;
    this.#mark({ tMs: this.#sched.now(), lane, kind: 'calibration', label, calibration: cal });
    this.#emit({ type: 'calibration', lane, outcome: { lane, ...cal } });
  }

  #mark(m: LaneMarker): void {
    this.#markers[m.lane]!.push(m);
    this.#markers[m.lane]!.pruneBefore(m.tMs - this.#historyMs);
    this.#emit({ type: 'marker', lane: m.lane, marker: m });
  }

  // ---- polling -----------------------------------------------------------------------------------

  async #pollStatus(): Promise<void> {
    if (!this.#connected) return;
    try {
      const st = await this.#o.api.status();
      this.#status = st;
      this.#statusAtMs = this.#sched.now();
      this.#statusFailed = false;
      this.#nodes = applyStatus(this.#nodes, st);
      if (this.#needNodeList || st.lanesPresent !== presentLanes(this.#nodes).length) {
        this.#needNodeList = false;
        try {
          this.#nodes = applyNodeList(this.#nodes, await this.#o.api.nodeList());
          for (const l of presentLanes(this.#nodes)) this.#seen.add(l);
        } catch {
          this.#needNodeList = true;
        }
      }
    } catch {
      this.#statusFailed = true;
    }
    this.#emit({ type: 'report', report: this.report() });
  }

  async #laneTick(): Promise<void> {
    if (!this.#connected || this.#laneTickBusy) return;
    const rate = this.laneRateHz();
    if (rate <= 0) return;
    const period = 1000 / rate;
    this.#laneTickBusy = true;
    try {
      for (const lane of presentLanes(this.#nodes)) {
        if (this.#sched.now() - this.#lastPollMs[lane]! < period - 1) continue;
        await this.#readLane(lane);
      }
    } finally {
      this.#laneTickBusy = false;
    }
  }

  /** The single LANE_STATUS path: one in flight per lane; each answer is a sample. */
  #readLane(lane: number): Promise<LaneStatusRsp | null> {
    const cur = this.#inflight[lane];
    if (cur) return cur;
    this.#lastPollMs[lane] = this.#sched.now();
    const p = (async (): Promise<LaneStatusRsp | null> => {
      try {
        const rsp = await this.#o.api.laneStatus(lane);
        const tMs = this.#sched.now();
        this.#last[lane] = { rsp, atMs: tMs };
        const sample: LaneSample = {
          tMs, raw: rsp.raw, rawMin: rsp.rawMin, rawMax: rsp.rawMax, baseline: rsp.baseline, thrLow: rsp.thrLow,
          thrHigh: rsp.thrHigh, reference: rsp.reference, noise: rsp.noise, flags: rsp.flags,
        };
        const ring = this.#samples[lane]!;
        ring.push(sample);
        ring.pruneBefore(tMs - this.#historyMs);
        this.#emit({ type: 'sample', lane, sample });
        return rsp;
      } catch {
        return null;
      } finally {
        this.#inflight[lane] = null;
      }
    })();
    this.#inflight[lane] = p;
    return p;
  }
}

export { calibrationResultOf };
