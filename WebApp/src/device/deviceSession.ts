/*
 * The web app's side of one connection to the control module.
 *
 * Hides the transport (USB, BLE, simulator) behind requests, events and
 * streams (SYSTEM_ARCHITECTURE §7):
 *
 *   connect()   opens the transport, sends HELLO, waits for the binding if
 *               the device says UNBOUND, then RESUME: from the next evSeq
 *               this app has not seen if it is the same boot, else from the
 *               oldest event the device still holds (so a fresh app gets the
 *               history, starting with EVT_BOOT).
 *   binding     a BLE session the device does not know yet is UNBOUND: only
 *               HELLO and PING work (SYSTEM_ARCHITECTURE §9.5). The session
 *               goes to state 'unbound' and repeats HELLO every bindPollMs
 *               until the device answers BOUND (someone held the BLE button
 *               or opened the window over USB) or kicks the link after 60 s.
 *               A request answered ERR_NOT_BOUND later (the bind list was
 *               cleared) puts it back into that wait.
 *   request()   one request, one response, matched by tag. Times out instead
 *               of hanging. A non-OK status rejects with DeviceError, which
 *               carries the status name. ERR_NO_SESSION (the control module
 *               rebooted under a BLE link that stayed up) triggers a new
 *               HELLO and RESUME by itself.
 *   events      delivered in evSeq order with no repeats. A gap triggers
 *               RESUME. If the device no longer holds the missing events
 *               (ERR_LOG_GAP), onGap() reports the hole and delivery goes on.
 *   heartbeat   PING every pingIntervalMs. Each answer refines the clock
 *               estimate (sysUs ↔ scheduler time, from the lowest-RTT recent
 *               sample). Only the live clock display uses it: results never
 *               do. Nothing heard for staleAfterMs → onStale(true).
 *
 * Events are keyed on (bootId, evSeq). A new bootId means the device rebooted:
 * onBoot() fires and the counting starts over. The race engine sees a flagged
 * timing gap, not silently merged clocks.
 *
 * No DOM here: the scheduler is injected (VirtualClock in tests).
 */

import { FrameDecoder } from '../protocol/codec.ts';
import { decodePacket, encodePacket } from '../protocol/packet.ts';
import {
  BindState, ClientKind, HELLO_REQ, HELLO_RSP, HeaderFlag, MESSAGE_BY_ID, MsgId, PING_REQ, PING_RSP, PROTO,
  RESUME_REQ, RESUME_RSP, Status, responseStatus, statusName,
} from '../protocol/generated.ts';
import type { HelloRsp, MsgDesc, PingRsp, ResumeRsp } from '../protocol/generated.ts';
import { decodeStruct, encodeStruct, ProtocolError } from '../protocol/wire.ts';
import type { StructDesc } from '../protocol/wire.ts';
import type { Transport } from '../transport/transport.ts';
import { realScheduler } from '../util/clock.ts';
import type { Scheduler, TimerHandle } from '../util/clock.ts';

export class DeviceError extends Error {
  readonly status: number;
  readonly msgName: string;
  readonly statusName: string;
  constructor(msgName: string, status: number) {
    super(`${msgName}: ${statusName(status)}`);
    this.name = 'DeviceError';
    this.status = status;
    this.msgName = msgName;
    this.statusName = statusName(status);
  }
}

export class TimeoutError extends Error {
  constructor(msgName: string, ms: number) {
    super(`${msgName}: no response within ${ms} ms`);
    this.name = 'TimeoutError';
  }
}

/** The link closed while a request was waiting. */
export class LinkClosedError extends Error {
  constructor(msgName: string, reason: string) {
    super(`${msgName}: link closed (${reason})`);
    this.name = 'LinkClosedError';
  }
}

export type SessionState = 'idle' | 'connecting' | 'unbound' | 'open' | 'closed';

export interface DeviceEvent {
  readonly bootId: number;
  readonly evSeq: number;
  readonly msgId: number;
  /** Message name, or `EVT_0x..` for an event this app does not know. */
  readonly name: string;
  /** Decoded payload (evSeq included), or null for an unknown event. */
  readonly value: Record<string, unknown> | null;
}

export interface DeviceStream {
  readonly msgId: number;
  readonly name: string;
  readonly value: Record<string, unknown> | null;
}

export interface FrameTrace {
  readonly dir: 'in' | 'out';
  readonly raw: Uint8Array;
}

export interface SessionListener {
  onState?(state: SessionState, reason?: string): void;
  onEvent?(event: DeviceEvent): void;
  onStream?(stream: DeviceStream): void;
  /** Events [from, to) of this boot are gone for good (overwritten on the device). */
  onGap?(bootId: number, from: number, to: number): void;
  /** A new bootId: the device rebooted (or this is the first connect). */
  onBoot?(bootId: number, previous: number | null): void;
  /** Every HELLO answer (bind state, transport, other sessions). */
  onHello?(hello: HelloRsp): void;
  onFrame?(trace: FrameTrace): void;
  onClock?(rttMs: number): void;
  /** Nothing heard from the device for staleAfterMs (true), or heard again (false). */
  onStale?(stale: boolean): void;
}

export interface SessionOptions {
  readonly clientId: Uint8Array;
  readonly clientName: string;
  /** WEBAPP (default) binds over USB; TOOL never does. */
  readonly clientKind?: number;
  readonly requestTimeoutMs?: number;
  readonly pingIntervalMs?: number;
  /** HELLO repeat while UNBOUND. */
  readonly bindPollMs?: number;
  readonly staleAfterMs?: number;
  readonly scheduler?: Scheduler;
}

export interface RequestOptions {
  /** Overrides requestTimeoutMs (UPDATE_BEGIN erases flash for a while). */
  readonly timeoutMs?: number;
}

/** What survives a reconnect: where this app is in the device's event stream. */
export interface ResumePoint {
  bootId: number;
  nextEvSeq: number;
}

/** The request surface other modules depend on (DeviceSession, fakes in tests). */
export interface Requester {
  request<TRsp>(reqDesc: StructDesc, req: object, msgId: number, rspDesc: StructDesc, opts?: RequestOptions): Promise<TRsp>;
}

interface Pending {
  readonly msg: MsgDesc;
  readonly resolve: (payload: Uint8Array) => void;
  readonly reject: (err: Error) => void;
  readonly timer: TimerHandle;
  /** requestRaw: resolve with the payload whatever the status. */
  readonly raw: boolean;
}

interface ClockSample { rttMs: number; offsetUs: number }

const CLOCK_SAMPLES = 8;
const ALWAYS_ALLOWED = new Set<number>([MsgId.HELLO, MsgId.PING]);

export class DeviceSession implements Requester {
  #transport: Transport;
  #opts: Required<Omit<SessionOptions, 'scheduler'>>;
  #sched: Scheduler;
  #listener: SessionListener;
  #decoder = new FrameDecoder(PROTO.headerBytes + PROTO.maxPayload);
  #pending = new Map<number, Pending>();
  #nextTag = 1;
  #state: SessionState = 'idle';
  #pingTimer: TimerHandle | null = null;
  #clock: ClockSample[] = [];
  #resume: ResumePoint | null;
  #resyncing = false;
  #recovering: Promise<void> | null = null;
  #hello: HelloRsp | null = null;
  #lastHeardMs = 0;
  #stale = false;
  #closeWaiters: ((reason: string) => void)[] = [];

  constructor(transport: Transport, opts: SessionOptions, listener: SessionListener = {},
              resume: ResumePoint | null = null) {
    this.#transport = transport;
    this.#sched = opts.scheduler ?? realScheduler;
    this.#opts = {
      clientKind: ClientKind.WEBAPP, requestTimeoutMs: 1000, pingIntervalMs: 1000, bindPollMs: 1000, staleAfterMs: 3000,
      ...Object.fromEntries(Object.entries(opts).filter(([k, v]) => v !== undefined && k !== 'scheduler')),
    } as Required<Omit<SessionOptions, 'scheduler'>>;
    this.#listener = listener;
    this.#resume = resume ? { ...resume } : null;
  }

  get state(): SessionState { return this.#state; }
  get transport(): Transport { return this.#transport; }
  get hello(): HelloRsp | null { return this.#hello; }
  get bindState(): number | null { return this.#hello?.bindState ?? null; }
  /** hpBoardType from HELLO: CONTROL (app) or CONTROL_BOOT (bootloader); 0 from 0.2 firmware. */
  get boardType(): number | null { return this.#hello?.boardType ?? null; }
  get scheduler(): Scheduler { return this.#sched; }
  get stale(): boolean { return this.#stale; }
  /** Where to RESUME from on the next connect (also for a new session after a transport switch). */
  get resumePoint(): ResumePoint | null { return this.#resume ? { ...this.#resume } : null; }

  /** Estimated device sysUs now, or null before the first PING answer. */
  sysUsNow(): bigint | null {
    const best = this.#bestClock();
    if (!best) return null;
    return BigInt(Math.round(this.#sched.now() * 1000 + best.offsetUs));
  }

  /** Lowest round trip among recent PINGs, in ms. */
  rttMs(): number | null {
    return this.#bestClock()?.rttMs ?? null;
  }

  /** Resolves once the link is closed (now or later), with the reason. */
  closed(): Promise<string> {
    if (this.#state === 'closed') return Promise.resolve('closed');
    return new Promise((r) => this.#closeWaiters.push(r));
  }

  async connect(): Promise<HelloRsp> {
    this.#setState('connecting');
    await this.#transport.open({
      onBytes: (chunk) => this.#onBytes(chunk),
      onClose: (reason) => this.#onClosed(reason),
    });
    this.#lastHeardMs = this.#sched.now();
    try {
      let hello = await this.#sendHello();
      if (hello.bindState === BindState.UNBOUND) hello = await this.#waitBound();
      this.#setState('open');
      await this.#sendResume();
      this.#pingTimer = this.#sched.setInterval(() => this.#heartbeat(), this.#opts.pingIntervalMs);
      await this.ping();
      return hello;
    } catch (err) {
      await this.close(`connect failed: ${String(err instanceof Error ? err.message : err)}`);
      throw err;
    }
  }

  async close(reason = 'closed by app'): Promise<void> {
    if (this.#state === 'closed') return;
    this.#shutdown(reason);
    await this.#transport.close();
  }

  /** One request. Resolves with the decoded response body (status included). */
  request<TRsp>(reqDesc: StructDesc, req: object, msgId: number, rspDesc: StructDesc, opts: RequestOptions = {}): Promise<TRsp> {
    const msg = MESSAGE_BY_ID.get(msgId);
    if (!msg) return Promise.reject(new ProtocolError(`unknown msgId 0x${msgId.toString(16)}`));
    let payload: Uint8Array;
    try { payload = encodeStruct(reqDesc, req); } catch (err) { return Promise.reject(err); }
    return this.#send(msg, payload, opts.timeoutMs).then((rsp) => decodeStruct(rspDesc, rsp) as TRsp);
  }

  /** A request by descriptor (the debug console's generic sender). */
  requestMsg(msg: MsgDesc, req: object, opts?: RequestOptions): Promise<Record<string, unknown>> {
    if (!msg.req || !msg.rsp) return Promise.reject(new ProtocolError(`${msg.name} is not a request`));
    return this.request(msg.req, req, msg.id, msg.rsp, opts);
  }

  /**
   * Raw bytes as a request with any msgId (bench suites: unknown ids, wrong
   * lengths). Resolves with the status and the raw response payload.
   */
  requestRaw(msgId: number, payload: Uint8Array, opts: RequestOptions = {}): Promise<{ status: number; payload: Uint8Array }> {
    const msg: MsgDesc = MESSAGE_BY_ID.get(msgId) ?? { id: msgId, name: `REQ_0x${msgId.toString(16)}`, kind: 'request' };
    return this.#send(msg, payload, opts.timeoutMs, true).then((p) => ({ status: responseStatus(p), payload: p }));
  }

  async ping(): Promise<PingRsp> {
    const t0 = this.#sched.now();
    const rsp = await this.request<PingRsp>(PING_REQ, { hostMs: Math.floor(t0) >>> 0 }, MsgId.PING, PING_RSP);
    const t1 = this.#sched.now();
    const sample = { rttMs: t1 - t0, offsetUs: Number(rsp.sysUs) - ((t0 + t1) / 2) * 1000 };
    this.#clock.push(sample);
    if (this.#clock.length > CLOCK_SAMPLES) this.#clock.shift();
    this.#listener.onClock?.(sample.rttMs);
    return rsp;
  }

  // ---- internals ------------------------------------------------------------

  #heartbeat(): void {
    if (this.#state === 'closed') return;
    const silent = this.#sched.now() - this.#lastHeardMs;
    if (silent >= this.#opts.staleAfterMs && !this.#stale) {
      this.#stale = true;
      this.#listener.onStale?.(true);
    }
    this.ping().catch(() => undefined);
  }

  #helloReq(): object {
    return {
      protoMajor: PROTO.major, protoMinor: PROTO.minor, clientKind: this.#opts.clientKind, reserved: 0,
      clientId: this.#opts.clientId, clientName: this.#opts.clientName,
    };
  }

  /** HELLO, then bootId bookkeeping. Note: every HELLO moves the device cursor to its head. */
  async #sendHello(): Promise<HelloRsp> {
    const hello = await this.request<HelloRsp>(HELLO_REQ, this.#helloReq(), MsgId.HELLO, HELLO_RSP);
    this.#hello = hello;
    this.#listener.onHello?.(hello);
    const previous = this.#resume?.bootId ?? null;
    if (previous !== hello.bootId) {
      this.#resume = { bootId: hello.bootId, nextEvSeq: hello.logOldestSeq };
      this.#listener.onBoot?.(hello.bootId, previous);
    } else if (this.#resume && this.#resume.nextEvSeq < hello.logOldestSeq) {
      this.#listener.onGap?.(hello.bootId, this.#resume.nextEvSeq, hello.logOldestSeq);
      this.#resume.nextEvSeq = hello.logOldestSeq;
    }
    return hello;
  }

  /** UNBOUND: repeat HELLO until the device binds us. Rejects if the link closes (kick). */
  async #waitBound(): Promise<HelloRsp> {
    this.#setState('unbound');
    for (;;) {
      const closed = await new Promise<boolean>((resolve) => {
        const t = this.#sched.setTimeout(() => { done(false); }, this.#opts.bindPollMs);
        const done = (c: boolean): void => {
          this.#sched.clearTimeout(t);
          const i = this.#closeWaiters.indexOf(onClose);
          if (i >= 0) this.#closeWaiters.splice(i, 1);
          resolve(c);
        };
        const onClose = (): void => done(true);
        this.#closeWaiters.push(onClose);
      });
      if (closed || (this.#state as SessionState) === 'closed') throw new Error('link closed while waiting for the binding');
      let hello: HelloRsp;
      try {
        hello = await this.#sendHello();
      } catch (err) {
        if ((this.#state as SessionState) === 'closed') throw err;
        continue;                                  // a lost HELLO: try again next round
      }
      if (hello.bindState !== BindState.UNBOUND) return hello;
    }
  }

  /** Session lost on the device side (reboot under BLE, or binding revoked): HELLO again, wait, RESUME. */
  #recover(): void {
    if (this.#recovering || this.#state !== 'open') return;   // connect() or a wait already runs
    this.#recovering = (async () => {
      try {
        let hello = await this.#sendHello();
        if (hello.bindState === BindState.UNBOUND) hello = await this.#waitBound();
        if (this.#state === 'closed') return;
        this.#setState('open');
        await this.#sendResume();
      } catch {
        /* the heartbeat tries again, or the link is gone */
      } finally {
        this.#recovering = null;
      }
    })();
  }

  #send(msg: MsgDesc, payload: Uint8Array, timeoutMs?: number, raw = false): Promise<Uint8Array> {
    // While unbound everything goes out: the device answers ERR_NOT_BOUND itself.
    const allowed = this.#state === 'open' || this.#state === 'unbound'
      || (this.#state === 'connecting' && ALWAYS_ALLOWED.has(msg.id));
    if (!allowed) return Promise.reject(new Error(`${msg.name}: session not open (${this.#state})`));
    const tag = this.#nextTag;
    this.#nextTag = this.#nextTag === 0xffff ? 1 : this.#nextTag + 1;
    let frame: Uint8Array;
    try { frame = encodePacket({ msgId: msg.id, flags: 0, tag }, payload); } catch (err) { return Promise.reject(err); }
    const ms = timeoutMs ?? this.#opts.requestTimeoutMs;

    return new Promise<Uint8Array>((resolve, reject) => {
      const timer = this.#sched.setTimeout(() => {
        this.#pending.delete(tag);
        reject(new TimeoutError(msg.name, ms));
      }, ms);
      const fail = (err: Error): void => {
        if (!raw && err instanceof DeviceError) {
          if (err.status === Status.ERR_NO_SESSION || (err.status === Status.ERR_NOT_BOUND && !ALWAYS_ALLOWED.has(msg.id))) this.#recover();
        }
        reject(err);
      };
      this.#pending.set(tag, { msg, resolve, reject: fail, timer, raw });
      this.#listener.onFrame?.({ dir: 'out', raw: frame });
      this.#transport.write(frame).catch((err: unknown) => {
        this.#sched.clearTimeout(timer);
        this.#pending.delete(tag);
        reject(err instanceof Error ? err : new Error(String(err)));
      });
    });
  }

  #onBytes(chunk: Uint8Array): void {
    this.#lastHeardMs = this.#sched.now();
    if (this.#stale) {
      this.#stale = false;
      this.#listener.onStale?.(false);
    }
    this.#decoder.feed(chunk, (raw) => {
      this.#listener.onFrame?.({ dir: 'in', raw });
      try {
        this.#onPacket(decodePacket(raw));
      } catch {
        /* malformed packet: counted by the decoder stats, nothing to act on */
      }
    });
  }

  #onPacket(p: ReturnType<typeof decodePacket>): void {
    const { header, payload } = p;
    if (header.flags & HeaderFlag.RESPONSE) {
      const pending = this.#pending.get(header.tag);
      if (!pending || pending.msg.id !== header.msgId) return;   // late or foreign: ignore
      this.#pending.delete(header.tag);
      this.#sched.clearTimeout(pending.timer);
      const status = payload.length >= 2 ? responseStatus(payload) : Status.ERR_BAD_LENGTH;
      if (pending.raw) pending.resolve(payload);
      else if (status !== Status.OK) pending.reject(new DeviceError(pending.msg.name, status));
      else pending.resolve(payload);
    } else if (header.flags & HeaderFlag.EVENT) {
      this.#onEventPacket(header.msgId, payload);
    } else if (header.flags & HeaderFlag.STREAM) {
      const msg = MESSAGE_BY_ID.get(header.msgId);
      let value: Record<string, unknown> | null = null;
      if (msg?.body) {
        try { value = decodeStruct(msg.body, payload); } catch { value = null; }
      }
      this.#listener.onStream?.({ msgId: header.msgId, name: msg?.name ?? `STR_0x${header.msgId.toString(16)}`, value });
    }
  }

  #onEventPacket(msgId: number, payload: Uint8Array): void {
    if (payload.length < 4 || !this.#resume) return;
    const evSeq = new DataView(payload.buffer, payload.byteOffset, 4).getUint32(0, true);
    const expected = this.#resume.nextEvSeq;

    if (evSeq < expected) return;                     // replayed duplicate
    if (evSeq > expected) {                           // we missed some: ask again
      if (!this.#resyncing && this.#state === 'open') void this.#resync();
      return;
    }

    const msg = MESSAGE_BY_ID.get(msgId);
    let value: Record<string, unknown> | null = null;
    if (msg?.body) {
      try { value = decodeStruct(msg.body, payload); } catch { value = null; }
    }
    this.#resume.nextEvSeq = evSeq + 1;
    this.#listener.onEvent?.({
      bootId: this.#resume.bootId, evSeq, msgId,
      name: msg?.name ?? `EVT_0x${msgId.toString(16)}`, value,
    });
  }

  async #resync(): Promise<void> {
    this.#resyncing = true;
    try {
      await this.#sendResume();
    } catch {
      /* the next out-of-order event retries */
    } finally {
      this.#resyncing = false;
    }
  }

  async #sendResume(): Promise<void> {
    if (!this.#resume) return;
    try {
      await this.request<ResumeRsp>(RESUME_REQ, { bootId: this.#resume.bootId, fromSeq: this.#resume.nextEvSeq }, MsgId.RESUME, RESUME_RSP);
    } catch (err) {
      if (err instanceof DeviceError && (err.status === Status.ERR_LOG_GAP || err.status === Status.ERR_BOOT_MISMATCH)) {
        // Learn the current window (and maybe a new boot), skip the hole, replay what is left.
        await this.#sendHello();
        await this.request<ResumeRsp>(RESUME_REQ, { bootId: this.#resume.bootId, fromSeq: this.#resume.nextEvSeq }, MsgId.RESUME, RESUME_RSP);
      } else {
        throw err;
      }
    }
  }

  #bestClock(): ClockSample | undefined {
    let best: ClockSample | undefined;
    for (const s of this.#clock) if (!best || s.rttMs < best.rttMs) best = s;
    return best;
  }

  #onClosed(reason: string): void {
    this.#shutdown(reason);
  }

  #shutdown(reason: string): void {
    if (this.#state === 'closed') return;
    this.#sched.clearInterval(this.#pingTimer);
    this.#pingTimer = null;
    for (const [, p] of this.#pending) {
      this.#sched.clearTimeout(p.timer);
      p.reject(new LinkClosedError(p.msg.name, reason));
    }
    this.#pending.clear();
    this.#decoder.reset();
    this.#setState('closed', reason);
    for (const w of this.#closeWaiters.splice(0)) w(reason);
  }

  #setState(state: SessionState, reason?: string): void {
    if (this.#state === state && state !== 'closed') return;
    this.#state = state;
    this.#listener.onState?.(state, reason);
  }
}
