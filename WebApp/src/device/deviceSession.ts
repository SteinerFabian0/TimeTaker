/*
 * The web app's side of one connection to the control module.
 *
 * Hides the transport (USB, BLE, simulator) behind requests, events and
 * streams (SYSTEM_ARCHITECTURE §7):
 *
 *   connect()   opens the transport, sends HELLO, then RESUME: from the next
 *               evSeq this app has not seen if it is the same boot, else from
 *               the oldest event the device still holds (so a fresh app gets
 *               the history, starting with EVT_BOOT).
 *   request()   one request, one response, matched by tag. Times out instead
 *               of hanging. A non-OK status rejects with DeviceError, which
 *               carries the status name.
 *   events      delivered in evSeq order with no repeats. A gap triggers
 *               RESUME. If the device no longer holds the missing events
 *               (ERR_LOG_GAP), onGap() reports the hole and delivery goes on.
 *   heartbeat   PING every pingIntervalMs. Each answer refines the clock
 *               estimate (sysUs ↔ performance.now(), from the lowest-RTT
 *               recent sample). Only the live clock display uses it: results
 *               never do.
 *
 * Events are keyed on (bootId, evSeq). A new bootId means the device rebooted:
 * onBoot() fires and the counting starts over. The race engine sees a flagged
 * timing gap, not silently merged clocks.
 */

import { FrameDecoder } from '../protocol/codec.ts';
import { decodePacket, encodePacket } from '../protocol/packet.ts';
import {
  ClientKind, HELLO_REQ, HELLO_RSP, HeaderFlag, MESSAGE_BY_ID, MsgId, PING_REQ, PING_RSP, PROTO, RESUME_REQ, RESUME_RSP,
  Status, responseStatus, statusName,
} from '../protocol/generated.ts';
import type { HelloRsp, MsgDesc, PingRsp, ResumeRsp } from '../protocol/generated.ts';
import { decodeStruct, encodeStruct, ProtocolError } from '../protocol/wire.ts';
import type { StructDesc } from '../protocol/wire.ts';
import type { Transport } from '../transport/transport.ts';

export class DeviceError extends Error {
  readonly status: number;
  readonly msgName: string;
  constructor(msgName: string, status: number) {
    super(`${msgName}: ${statusName(status)}`);
    this.name = 'DeviceError';
    this.status = status;
    this.msgName = msgName;
  }
}

export class TimeoutError extends Error {
  constructor(msgName: string, ms: number) {
    super(`${msgName}: no response within ${ms} ms`);
    this.name = 'TimeoutError';
  }
}

export type SessionState = 'idle' | 'connecting' | 'open' | 'closed';

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
  onFrame?(trace: FrameTrace): void;
  onClock?(rttMs: number): void;
}

export interface SessionOptions {
  readonly clientId: Uint8Array;
  readonly clientName: string;
  readonly requestTimeoutMs?: number;
  readonly pingIntervalMs?: number;
}

/** What survives a reconnect: where this app is in the device's event stream. */
export interface ResumePoint {
  bootId: number;
  nextEvSeq: number;
}

interface Pending {
  readonly msg: MsgDesc;
  readonly resolve: (payload: Uint8Array) => void;
  readonly reject: (err: Error) => void;
  readonly timer: ReturnType<typeof setTimeout>;
}

interface ClockSample { rttMs: number; offsetUs: number }

const CLOCK_SAMPLES = 8;

export class DeviceSession {
  #transport: Transport;
  #opts: Required<SessionOptions>;
  #listener: SessionListener;
  #decoder = new FrameDecoder(PROTO.headerBytes + PROTO.maxPayload);
  #pending = new Map<number, Pending>();
  #nextTag = 1;
  #state: SessionState = 'idle';
  #pingTimer: ReturnType<typeof setInterval> | null = null;
  #clock: ClockSample[] = [];
  #resume: ResumePoint | null;
  #resyncing = false;
  #hello: HelloRsp | null = null;

  constructor(transport: Transport, opts: SessionOptions, listener: SessionListener = {},
              resume: ResumePoint | null = null) {
    this.#transport = transport;
    this.#opts = { requestTimeoutMs: 1000, pingIntervalMs: 1000, ...opts };
    this.#listener = listener;
    this.#resume = resume;
  }

  get state(): SessionState { return this.#state; }
  get transport(): Transport { return this.#transport; }
  get hello(): HelloRsp | null { return this.#hello; }
  /** Where to RESUME from on the next connect (also for a new session after a transport switch). */
  get resumePoint(): ResumePoint | null { return this.#resume ? { ...this.#resume } : null; }

  /** Estimated device sysUs now, or null before the first PING answer. */
  sysUsNow(): bigint | null {
    const best = this.#bestClock();
    if (!best) return null;
    return BigInt(Math.round(performance.now() * 1000 + best.offsetUs));
  }

  /** Lowest round trip among recent PINGs, in ms. */
  rttMs(): number | null {
    return this.#bestClock()?.rttMs ?? null;
  }

  async connect(): Promise<HelloRsp> {
    this.#setState('connecting');
    await this.#transport.open({
      onBytes: (chunk) => this.#onBytes(chunk),
      onClose: (reason) => this.#onClosed(reason),
    });

    try {
      const hello = await this.request<HelloRsp>(HELLO_REQ, {
        protoMajor: PROTO.major, protoMinor: PROTO.minor, clientKind: ClientKind.WEBAPP, reserved: 0,
        clientId: this.#opts.clientId, clientName: this.#opts.clientName,
      }, MsgId.HELLO, HELLO_RSP);
      this.#hello = hello;
      this.#setState('open');

      const previous = this.#resume?.bootId ?? null;
      if (previous !== hello.bootId) {
        this.#resume = { bootId: hello.bootId, nextEvSeq: hello.logOldestSeq };
        this.#listener.onBoot?.(hello.bootId, previous);
      } else if (this.#resume && this.#resume.nextEvSeq < hello.logOldestSeq) {
        this.#listener.onGap?.(hello.bootId, this.#resume.nextEvSeq, hello.logOldestSeq);
        this.#resume.nextEvSeq = hello.logOldestSeq;
      }
      await this.#sendResume();
      await this.ping();
      this.#pingTimer = setInterval(() => { void this.ping().catch(() => undefined); }, this.#opts.pingIntervalMs);
      return hello;
    } catch (err) {
      await this.close(`connect failed: ${String(err)}`);
      throw err;
    }
  }

  async close(reason = 'closed by app'): Promise<void> {
    if (this.#state === 'closed') return;
    this.#shutdown(reason);
    await this.#transport.close();
  }

  /** One request. Resolves with the decoded response body (status included). */
  request<TRsp>(reqDesc: StructDesc, req: object, msgId: number, rspDesc: StructDesc): Promise<TRsp> {
    const msg = MESSAGE_BY_ID.get(msgId);
    if (!msg) return Promise.reject(new ProtocolError(`unknown msgId 0x${msgId.toString(16)}`));
    const payload = encodeStruct(reqDesc, req);
    return this.#send(msg, payload).then((rsp) => decodeStruct(rspDesc, rsp) as TRsp);
  }

  /** A request by descriptor (the debug console's generic sender). */
  requestMsg(msg: MsgDesc, req: object): Promise<Record<string, unknown>> {
    if (!msg.req || !msg.rsp) return Promise.reject(new ProtocolError(`${msg.name} is not a request`));
    return this.request(msg.req, req, msg.id, msg.rsp);
  }

  async ping(): Promise<PingRsp> {
    const t0 = performance.now();
    const rsp = await this.request<PingRsp>(PING_REQ, { hostMs: Math.floor(t0) >>> 0 }, MsgId.PING, PING_RSP);
    const t1 = performance.now();
    const sample = { rttMs: t1 - t0, offsetUs: Number(rsp.sysUs) - ((t0 + t1) / 2) * 1000 };
    this.#clock.push(sample);
    if (this.#clock.length > CLOCK_SAMPLES) this.#clock.shift();
    this.#listener.onClock?.(sample.rttMs);
    return rsp;
  }

  // ---- internals ------------------------------------------------------------

  #send(msg: MsgDesc, payload: Uint8Array): Promise<Uint8Array> {
    if (this.#state !== 'open' && !(this.#state === 'connecting' && msg.id === MsgId.HELLO)) {
      return Promise.reject(new Error(`${msg.name}: session not open`));
    }
    const tag = this.#nextTag;
    this.#nextTag = this.#nextTag === 0xffff ? 1 : this.#nextTag + 1;
    const frame = encodePacket({ msgId: msg.id, flags: 0, tag }, payload);

    return new Promise<Uint8Array>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.#pending.delete(tag);
        reject(new TimeoutError(msg.name, this.#opts.requestTimeoutMs));
      }, this.#opts.requestTimeoutMs);
      this.#pending.set(tag, { msg, resolve, reject, timer });
      this.#listener.onFrame?.({ dir: 'out', raw: frame });
      this.#transport.write(frame).catch((err: unknown) => {
        clearTimeout(timer);
        this.#pending.delete(tag);
        reject(err instanceof Error ? err : new Error(String(err)));
      });
    });
  }

  #onBytes(chunk: Uint8Array): void {
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
      clearTimeout(pending.timer);
      const status = responseStatus(payload);
      if (status !== Status.OK) pending.reject(new DeviceError(pending.msg.name, status));
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
      if (!this.#resyncing) void this.#resync();
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
      if (err instanceof DeviceError && err.status === Status.ERR_LOG_GAP && this.#hello) {
        // Learn the current window and skip the hole, then replay what is left.
        const hello = await this.request<HelloRsp>(HELLO_REQ, {
          protoMajor: PROTO.major, protoMinor: PROTO.minor, clientKind: ClientKind.WEBAPP, reserved: 0,
          clientId: this.#opts.clientId, clientName: this.#opts.clientName,
        }, MsgId.HELLO, HELLO_RSP);
        this.#listener.onGap?.(this.#resume.bootId, this.#resume.nextEvSeq, hello.logOldestSeq);
        this.#resume.nextEvSeq = hello.logOldestSeq;
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
    if (this.#pingTimer) clearInterval(this.#pingTimer);
    this.#pingTimer = null;
    for (const [, p] of this.#pending) {
      clearTimeout(p.timer);
      p.reject(new Error(`${p.msg.name}: link closed (${reason})`));
    }
    this.#pending.clear();
    this.#decoder.reset();
    this.#setState('closed', reason);
  }

  #setState(state: SessionState, reason?: string): void {
    this.#state = state;
    this.#listener.onState?.(state, reason);
  }
}
