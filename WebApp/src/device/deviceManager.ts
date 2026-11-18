/*
 * The connection lifecycle above DeviceSession: which transport, reconnects,
 * and one continuous event stream across all of them.
 *
 *   - Transport preference USB > BLE (SYSTEM_ARCHITECTURE §11). Sources are
 *     tried in that order. While on BLE, the USB sources are probed every
 *     preferCheckMs; when one answers, a USB session is opened with the
 *     current resume point and the BLE session is closed after it is up:
 *     nothing is lost and nothing repeats.
 *   - A dropped link (unplug, kick, reboot) is retried every reconnectMs from
 *     any source that can reconnect without a user gesture (a remembered
 *     Web Serial port, a permitted Web Bluetooth device, the simulator).
 *     connectWith() takes a transport the user just picked.
 *   - Events reach the listener exactly once, in evSeq order per boot, even
 *     while two sessions overlap during a switch.
 *   - The node model follows NODE_LIST (fetched on every connect and shortly
 *     after lanes join) and the events.
 *   - `api` always talks to the current session.
 *
 * No DOM: transports come from TransportSource objects.
 */

import type { HelloRsp } from '../protocol/generated.ts';
import { MsgId, LaneState } from '../protocol/generated.ts';
import type { StructDesc } from '../protocol/wire.ts';
import type { Transport } from '../transport/transport.ts';
import { realScheduler, sleep } from '../util/clock.ts';
import type { Scheduler, TimerHandle } from '../util/clock.ts';
import { DeviceApi } from './deviceApi.ts';
import { DeviceSession } from './deviceSession.ts';
import type { DeviceEvent, DeviceStream, RequestOptions, Requester, ResumePoint, SessionOptions } from './deviceSession.ts';
import { applyNodeEvent, applyNodeList, emptyNodeModel } from './nodeModel.ts';
import type { NodeModel } from './nodeModel.ts';

export interface TransportSource {
  readonly kind: 'usb' | 'ble';
  /** A transport to the device that needs no user gesture now, or null. */
  find(): Promise<Transport | null>;
}

export type ManagerState = 'disconnected' | 'connecting' | 'unbound' | 'connected';

export interface ManagerListener {
  onState?(state: ManagerState, transport: 'usb' | 'ble' | null, reason?: string): void;
  onEvent?(event: DeviceEvent): void;
  onStream?(stream: DeviceStream): void;
  onBoot?(bootId: number, previous: number | null): void;
  onGap?(bootId: number, from: number, to: number): void;
  onHello?(hello: HelloRsp): void;
  onNodes?(model: NodeModel): void;
}

export interface DeviceManagerOptions extends Omit<SessionOptions, 'scheduler'> {
  readonly sources: readonly TransportSource[];
  readonly scheduler?: Scheduler;
  readonly reconnectMs?: number;
  readonly preferCheckMs?: number;
  /** Where the app already is in the event stream (from the event store). */
  readonly resume?: ResumePoint | null;
}

const rank = (k: 'usb' | 'ble' | 'sim'): number => (k === 'usb' || k === 'sim' ? 0 : 1);

export class DeviceManager {
  #opts: DeviceManagerOptions;
  #sched: Scheduler;
  #listener: ManagerListener;
  #session: DeviceSession | null = null;
  #state: ManagerState = 'disconnected';
  #running = false;
  #busy = false;
  #timer: TimerHandle | null = null;
  #resume: ResumePoint | null;
  #nodes: NodeModel = emptyNodeModel();
  #nodeRefresh: TimerHandle | null = null;
  readonly api: DeviceApi;

  constructor(opts: DeviceManagerOptions, listener: ManagerListener = {}) {
    this.#opts = opts;
    this.#sched = opts.scheduler ?? realScheduler;
    this.#listener = listener;
    this.#resume = opts.resume ? { ...opts.resume } : null;
    const self = this;
    const requester: Requester = {
      request<T>(reqDesc: StructDesc, req: object, msgId: number, rspDesc: StructDesc, o?: RequestOptions): Promise<T> {
        const s = self.#session;
        if (!s) return Promise.reject(new Error('not connected'));
        return s.request<T>(reqDesc, req, msgId, rspDesc, o);
      },
    };
    this.api = new DeviceApi(requester, { sleep: (ms) => sleep(this.#sched, ms) });
  }

  get state(): ManagerState { return this.#state; }
  get session(): DeviceSession | null { return this.#session; }
  get transportKind(): 'usb' | 'ble' | null {
    const k = this.#session?.transport.kind;
    return k === undefined ? null : k === 'ble' ? 'ble' : 'usb';
  }
  get nodes(): NodeModel { return this.#nodes; }
  get resumePoint(): ResumePoint | null { return this.#resume ? { ...this.#resume } : null; }

  /** Start connecting (and keep reconnecting) from the sources. */
  start(): void {
    if (this.#running) return;
    this.#running = true;
    this.#schedule(0);
  }

  async stop(): Promise<void> {
    this.#running = false;
    this.#sched.clearTimeout(this.#timer);
    this.#sched.clearTimeout(this.#nodeRefresh);
    this.#timer = null;
    const s = this.#session;
    this.#session = null;
    await s?.close('stopped');
    this.#setState('disconnected', 'stopped');
  }

  /** Connect with a transport the user just picked (port picker, BLE chooser). */
  async connectWith(transport: Transport): Promise<HelloRsp> {
    return this.#open(transport);
  }

  #schedule(ms: number): void {
    if (!this.#running) return;
    this.#sched.clearTimeout(this.#timer);
    this.#timer = this.#sched.setTimeout(() => { this.#timer = null; void this.#tick(); }, ms);
  }

  async #tick(): Promise<void> {
    if (!this.#running) return;
    const current = this.#session?.transport.kind;
    try {
      if (!this.#busy && (!this.#session || (current && rank(current) > 0))) {
        const wanted = this.#session ? this.#opts.sources.filter((s) => rank(s.kind) < rank(current!)) : this.#sorted();
        for (const src of wanted) {
          let t: Transport | null = null;
          try { t = await src.find(); } catch { t = null; }
          if (!t) continue;
          try {
            await this.#open(t);
            break;
          } catch {
            /* next source */
          }
        }
      }
    } finally {
      const next = !this.#session ? (this.#opts.reconnectMs ?? 1000)
        : this.transportKind === 'ble' ? (this.#opts.preferCheckMs ?? 2000) : null;
      if (next !== null) this.#schedule(next);
    }
  }

  #sorted(): TransportSource[] {
    return [...this.#opts.sources].sort((a, b) => rank(a.kind) - rank(b.kind));
  }

  async #open(transport: Transport): Promise<HelloRsp> {
    this.#busy = true;
    const previous = this.#session;
    if (!previous) this.#setState('connecting', null);
    const s: DeviceSession = new DeviceSession(transport, { ...this.#opts, scheduler: this.#sched }, {
      onState: (st, reason) => {
        if (this.#session !== s) return;
        if (st === 'unbound') this.#setState('unbound', transport.kind === 'ble' ? 'ble' : 'usb');
        else if (st === 'open') this.#setState('connected', transport.kind === 'ble' ? 'ble' : 'usb');
        else if (st === 'closed') {
          this.#session = null;
          this.#setState('disconnected', null, reason);
          this.#schedule(this.#opts.reconnectMs ?? 1000);
        }
      },
      onEvent: (e) => this.#onEvent(e),
      onStream: (st) => this.#listener.onStream?.(st),
      onBoot: (b, prev) => {
        if (this.#resume && this.#resume.bootId === b) return;     // the other session already said so
        this.#resume = { bootId: b, nextEvSeq: 0 };
        this.#listener.onBoot?.(b, prev);
      },
      onGap: (b, from, to) => {
        if (this.#resume?.bootId === b && this.#resume.nextEvSeq < to) {
          this.#resume.nextEvSeq = to;
          this.#listener.onGap?.(b, from, to);
        }
      },
      onHello: (h) => this.#listener.onHello?.(h),
    }, this.#resume);
    if (!previous) this.#session = s;
    try {
      const hello = await s.connect();
      if (previous) {
        this.#session = s;
        this.#setState('connected', transport.kind === 'ble' ? 'ble' : 'usb');
        await previous.close('switched to a preferred transport');
      }
      await this.#refreshNodes();
      return hello;
    } catch (err) {
      if (this.#session === s) {
        this.#session = previous;
        if (!previous) this.#setState('disconnected', null, String(err instanceof Error ? err.message : err));
      }
      throw err;
    } finally {
      this.#busy = false;
    }
  }

  #onEvent(e: DeviceEvent): void {
    const r = this.#resume;
    if (r && e.bootId === r.bootId) {
      if (e.evSeq < r.nextEvSeq) return;                // the overlapping session delivered it already
      r.nextEvSeq = e.evSeq + 1;
    } else {
      this.#resume = { bootId: e.bootId, nextEvSeq: e.evSeq + 1 };
    }
    this.#nodes = applyNodeEvent(this.#nodes, e);
    this.#listener.onNodes?.(this.#nodes);
    if (e.msgId === MsgId.EVT_LANE_STATE && e.value?.['state'] === LaneState.CONFIGURED) {
      this.#sched.clearTimeout(this.#nodeRefresh);
      this.#nodeRefresh = this.#sched.setTimeout(() => { void this.#refreshNodes().catch(() => undefined); }, 150);
    }
    this.#listener.onEvent?.(e);
  }

  async #refreshNodes(): Promise<void> {
    if (!this.#session || this.#session.state !== 'open') return;
    try {
      this.#nodes = applyNodeList(this.#nodes, await this.api.nodeList());
      this.#listener.onNodes?.(this.#nodes);
    } catch {
      /* next time */
    }
  }

  #setState(state: ManagerState, transport: 'usb' | 'ble' | null, reason?: string): void {
    if (this.#state === state && state !== 'connected') return;
    this.#state = state;
    this.#listener.onState?.(state, transport, reason);
  }
}
