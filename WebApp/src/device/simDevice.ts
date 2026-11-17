/*
 * A simulated control module that speaks the real host protocol.
 *
 * It exists so the web app can be built and tested without hardware
 * (SYSTEM_ARCHITECTURE §11). It follows the firmware's host_session and
 * host_manager rules, so anything that works here should work against the
 * board:
 *   - frames: same codec and packets as the device;
 *   - HELLO gate, PING, RESUME with ERR_BOOT_MISMATCH / ERR_LOG_GAP,
 *     status-only error responses;
 *   - a ring event log of `logCapacity`, evSeq 0 = EVT_BOOT;
 *   - one cursor per attached link, pumped as the link allows;
 *   - LANE_POWER with a settle time, NODE_LIST, REBOOT (APP only).
 *
 * It also simulates what the firmware does not do yet, so the app can grow
 * ahead of it:
 *   - lanes that join when powered (EVT_LANE_STATE JOINED/CONFIGURED, LEFT on
 *     power-off);
 *   - optionally, cars crossing every few seconds (EVT_CROSSING).
 *
 * Test hooks: chunked delivery with latency (exercises reassembly), and
 * dropNextEvents() (exercises the app's gap detection and RESUME).
 */

import { FrameDecoder, frameEncode } from '../protocol/codec.ts';
import { decodePacket, encodePacket } from '../protocol/packet.ts';
import {
  BoardType, EVT_BOOT, EVT_BUTTON, EVT_CROSSING, EVT_LANE_STATE, HELLO_REQ, HELLO_RSP, HeaderFlag,
  LANE_POWER_REQ, LANE_POWER_RSP, LaneEventType, LaneState, MsgId, NODE_LIST_RSP, NodeClass, NodeState, PING_REQ,
  PING_RSP, PROTO, REBOOT_REQ, REBOOT_RSP, RESUME_REQ, RESUME_RSP, RebootTarget, ResetCause, Status, Transport as
  HpTransport, BindState,
} from '../protocol/generated.ts';
import type { NodeInfo } from '../protocol/generated.ts';
import { decodeStruct, encodeStruct } from '../protocol/wire.ts';
import type { StructDesc } from '../protocol/wire.ts';
import type { Transport, TransportHandlers } from '../transport/transport.ts';

export interface SimOptions {
  /** One-way delivery delay, ms. */
  readonly latencyMs?: number;
  /** Device → host bytes are delivered in chunks of at most this size. */
  readonly chunkBytes?: number;
  /** Event log size (the firmware has 1024). */
  readonly logCapacity?: number;
  /** Cars cross on powered lanes. */
  readonly simulateCars?: boolean;
  /** Lanes that join when powered. */
  readonly lanes?: number;
  /** Fixed seed for repeatable tests; random otherwise. */
  readonly seed?: number;
}

interface LogEntry { readonly msgId: number; readonly body: Uint8Array }

interface Link {
  readonly handlers: TransportHandlers;
  readonly decoder: FrameDecoder;
  open: boolean;
  cursor: number;
  outQueue: number;   // pending delivery timers, for flush-on-close
}

const LANE_SETTLE_MS = 30;
const PUMP_MS = 5;

export class SimDevice {
  readonly #opts: Required<SimOptions>;
  #rng: () => number;
  #bootId = 0;
  #bootMs = 0;
  #log: (LogEntry | undefined)[] = [];
  #next = 0;
  #links = new Set<Link>();
  #pump: ReturnType<typeof setInterval> | null = null;
  #lanePower: 'off' | 'settling' | 'on' = 'off';
  #laneTimer: ReturnType<typeof setTimeout> | null = null;
  #carTimers: ReturnType<typeof setTimeout>[] = [];
  #laneSeq: number[] = [];
  #dropEvents = 0;
  #cars: boolean;

  constructor(opts: SimOptions = {}) {
    this.#opts = {
      latencyMs: 2, chunkBytes: 20, logCapacity: 1024, simulateCars: false, lanes: 6,
      seed: Math.floor(Math.random() * 0xffffffff), ...opts,
    };
    let s = this.#opts.seed >>> 0 || 1;
    this.#rng = () => {
      s ^= s << 13; s >>>= 0; s ^= s >>> 17; s ^= s << 5; s >>>= 0;
      return s / 0x100000000;
    };
    this.#cars = this.#opts.simulateCars;
    this.#boot(ResetCause.POWER_ON);
  }

  get bootId(): number { return this.#bootId; }
  get logNextSeq(): number { return this.#next; }
  get lanesPowered(): boolean { return this.#lanePower === 'on'; }

  sysUs(): bigint {
    return BigInt(Math.round((performance.now() - this.#bootMs) * 1000));
  }

  /** A transport to plug into a DeviceSession: one "cable" to this device. */
  transport(kind: 'sim' = 'sim'): Transport {
    return new SimTransport(this, kind);
  }

  pressButton(button: number, press: number): void {
    this.#publish(EVT_BUTTON, { evSeq: 0, button, press, reserved: 0, sysUs: this.sysUs() });
  }

  setCars(on: boolean): void {
    this.#cars = on;
    this.#restartCars();
  }

  get cars(): boolean { return this.#cars; }

  /** The next n events are logged but not sent: the app must notice the gap and RESUME. */
  dropNextEvents(n: number): void {
    this.#dropEvents = n;
  }

  /** Power loss: every link closes, new bootId, empty log. */
  powerCycle(): void {
    this.#boot(ResetCause.POWER_ON);
  }

  dispose(): void {
    this.#stopLanes();
    if (this.#pump) clearInterval(this.#pump);
    this.#pump = null;
    for (const l of this.#links) l.handlers.onClose('simulator disposed');
    this.#links.clear();
  }

  // ---- links (called by SimTransport) -----------------------------------------

  attach(handlers: TransportHandlers): Link {
    const link: Link = {
      handlers, decoder: new FrameDecoder(PROTO.headerBytes + PROTO.maxPayload), open: false, cursor: 0, outQueue: 0,
    };
    this.#links.add(link);
    if (!this.#pump) this.#pump = setInterval(() => this.#pumpAll(), PUMP_MS);
    return link;
  }

  detach(link: Link): void {
    this.#links.delete(link);
  }

  receive(link: Link, bytes: Uint8Array): void {
    setTimeout(() => {
      link.decoder.feed(bytes, (raw) => this.#onFrame(link, raw));
    }, this.#opts.latencyMs);
  }

  // ---- device behaviour -------------------------------------------------------

  #boot(cause: number): void {
    this.#stopLanes();
    for (const l of this.#links) l.handlers.onClose('device rebooted');
    this.#links.clear();
    this.#bootId = (Math.floor(this.#rng() * 0xfffffffe) + 1) >>> 0;
    this.#bootMs = performance.now();
    this.#log = new Array<LogEntry | undefined>(this.#opts.logCapacity);
    this.#next = 0;
    this.#publish(EVT_BOOT, { evSeq: 0, resetCause: cause, reserved: new Uint8Array(3), bootId: this.#bootId, sysUs: 0n });
  }

  #oldest(): number {
    return Math.max(0, this.#next - this.#opts.logCapacity);
  }

  #publish(desc: StructDesc, value: object): void {
    const payload = encodeStruct(desc, value);
    this.#log[this.#next % this.#opts.logCapacity] = { msgId: this.#msgIdOf(desc), body: payload.subarray(4) };
    this.#next++;
  }

  #msgIdOf(desc: StructDesc): number {
    switch (desc) {
      case EVT_BOOT: return MsgId.EVT_BOOT;
      case EVT_BUTTON: return MsgId.EVT_BUTTON;
      case EVT_CROSSING: return MsgId.EVT_CROSSING;
      case EVT_LANE_STATE: return MsgId.EVT_LANE_STATE;
      default: throw new Error(`simulator cannot publish ${desc.name}`);
    }
  }

  #send(link: Link, bytes: Uint8Array): void {
    const n = this.#opts.chunkBytes;
    for (let i = 0; i < bytes.length; i += n) {
      const chunk = bytes.slice(i, i + n);
      link.outQueue++;
      setTimeout(() => {
        link.outQueue--;
        if (this.#links.has(link)) link.handlers.onBytes(chunk);
      }, this.#opts.latencyMs);
    }
  }

  #respond(link: Link, msgId: number, tag: number, status: number, desc?: StructDesc, body?: object): void {
    let payload: Uint8Array;
    if (status !== Status.OK || !desc) {
      payload = new Uint8Array(2);
      new DataView(payload.buffer).setInt16(0, status, true);
    } else {
      payload = encodeStruct(desc, { ...body, status: Status.OK });
    }
    this.#send(link, encodePacket({ msgId, flags: HeaderFlag.RESPONSE, tag }, payload));
  }

  #onFrame(link: Link, raw: Uint8Array): void {
    let pkt: ReturnType<typeof decodePacket>;
    try { pkt = decodePacket(raw); } catch { return; }
    const { header: h, payload } = pkt;
    if (h.flags !== 0) return;

    const reply = (status: number, desc?: StructDesc, body?: object): void =>
      this.#respond(link, h.msgId, h.tag, status, desc, body);
    const parse = (desc: StructDesc): Record<string, unknown> | null => {
      if (payload.length !== desc.size) { reply(Status.ERR_BAD_LENGTH); return null; }
      return decodeStruct(desc, payload);
    };

    if (h.msgId === MsgId.HELLO) {
      const req = parse(HELLO_REQ);
      if (!req) return;
      if (req['protoMajor'] !== PROTO.major) { link.open = false; reply(Status.ERR_PROTO_MISMATCH); return; }
      link.open = true;
      link.cursor = this.#next;
      reply(Status.OK, HELLO_RSP, {
        protoMajor: PROTO.major, protoMinor: PROTO.minor, transport: HpTransport.USB, bindState: BindState.TRUSTED,
        bootId: this.#bootId, sysUs: this.sysUs(), logOldestSeq: this.#oldest(), logNextSeq: this.#next,
        otherSessions: this.#links.size - 1, reserved: new Uint8Array(3),
      });
      return;
    }
    if (!link.open) { reply(Status.ERR_NO_SESSION); return; }

    switch (h.msgId) {
      case MsgId.PING: {
        const req = parse(PING_REQ);
        if (req) reply(Status.OK, PING_RSP, { hostMs: req['hostMs'], sysUs: this.sysUs() });
        return;
      }
      case MsgId.RESUME: {
        const req = parse(RESUME_REQ);
        if (!req) return;
        const from = req['fromSeq'] as number;
        if (req['bootId'] !== this.#bootId) reply(Status.ERR_BOOT_MISMATCH);
        else if (from < this.#oldest()) reply(Status.ERR_LOG_GAP);
        else if (from > this.#next) reply(Status.ERR_INVALID_ARG);
        else {
          link.cursor = from;
          reply(Status.OK, RESUME_RSP, { logOldestSeq: this.#oldest(), logNextSeq: this.#next });
        }
        return;
      }
      case MsgId.NODE_LIST: {
        if (payload.length !== 0) { reply(Status.ERR_BAD_LENGTH); return; }
        reply(Status.OK, NODE_LIST_RSP, { nodes: this.#nodes() });
        return;
      }
      case MsgId.LANE_POWER: {
        const req = parse(LANE_POWER_REQ);
        if (!req) return;
        if ((req['on'] as number) > 1) { reply(Status.ERR_INVALID_ARG); return; }
        this.#setLanePower(req['on'] === 1);
        reply(Status.OK, LANE_POWER_RSP, { powered: this.#lanePower === 'on' ? 1 : 0 });
        return;
      }
      case MsgId.REBOOT: {
        const req = parse(REBOOT_REQ);
        if (!req) return;
        if (req['target'] === RebootTarget.BOOTLOADER) { reply(Status.ERR_UNSUPPORTED); return; }
        if (req['target'] !== RebootTarget.APP) { reply(Status.ERR_INVALID_ARG); return; }
        reply(Status.OK, REBOOT_RSP, {});
        setTimeout(() => this.#boot(ResetCause.SOFTWARE), 100);
        return;
      }
      default:
        reply(Status.ERR_UNSUPPORTED);
    }
  }

  #nodes(): NodeInfo[] {
    const nodes: NodeInfo[] = [{
      nodeClass: NodeClass.CTRL, index: 0, boardType: BoardType.CONTROL, state: NodeState.OK,
      fwMajor: 0, fwMinor: 1, fwPatch: 0, protoMinor: PROTO.minor, fwGitRev: 0x51d0051, imageCrc: 0,
    }];
    if (this.#lanePower === 'on') {
      for (let lane = 1; lane <= this.#opts.lanes; lane++) {
        nodes.push({
          nodeClass: NodeClass.LANE, index: lane, boardType: BoardType.LANE, state: NodeState.OK,
          fwMajor: 0, fwMinor: 1, fwPatch: 0, protoMinor: 0, fwGitRev: 0x1a9e000, imageCrc: 0,
        });
      }
    }
    return nodes;
  }

  #setLanePower(on: boolean): void {
    if (!on) {
      const wasOn = this.#lanePower === 'on';
      this.#stopLanes();
      if (wasOn) {
        for (let lane = 1; lane <= this.#opts.lanes; lane++) this.#laneState(lane, LaneState.LEFT);
      }
      return;
    }
    if (this.#lanePower !== 'off') return;
    this.#lanePower = 'settling';
    this.#laneTimer = setTimeout(() => {
      this.#lanePower = 'on';
      this.#laneSeq = new Array<number>(this.#opts.lanes + 1).fill(0);
      for (let lane = 1; lane <= this.#opts.lanes; lane++) {
        this.#laneState(lane, LaneState.JOINED);
        this.#laneState(lane, LaneState.CONFIGURED);
      }
      this.#restartCars();
    }, LANE_SETTLE_MS);
  }

  #laneState(lane: number, state: number): void {
    this.#publish(EVT_LANE_STATE, { evSeq: 0, lane, state, status: 0, sysUs: this.sysUs() });
  }

  #stopLanes(): void {
    if (this.#laneTimer) clearTimeout(this.#laneTimer);
    this.#laneTimer = null;
    for (const t of this.#carTimers) clearTimeout(t);
    this.#carTimers = [];
    this.#lanePower = 'off';
  }

  #restartCars(): void {
    for (const t of this.#carTimers) clearTimeout(t);
    this.#carTimers = [];
    if (!this.#cars || this.#lanePower !== 'on') return;
    for (let lane = 1; lane <= this.#opts.lanes; lane++) {
      const lapMs = 4000 + this.#rng() * 3000;        // each car has its own pace
      const go = (): void => {
        const dwell10us = Math.round(1500 + this.#rng() * 1500);   // 15..30 ms under the sensor
        const seq = this.#laneSeq[lane] ?? 0;
        this.#laneSeq[lane] = (seq + 1) & 0xffff;
        this.#publish(EVT_CROSSING, {
          evSeq: 0, lane, type: LaneEventType.CROSSING, flags: 0, reserved: 0,
          tEntryUs: this.sysUs() - BigInt(dwell10us * 10), laneSeq: seq, dwell10us,
          depth: Math.round(600 + this.#rng() * 300), reserved2: 0,
        });
        this.#carTimers.push(setTimeout(go, lapMs * (0.95 + this.#rng() * 0.1)));
      };
      this.#carTimers.push(setTimeout(go, 300 + this.#rng() * lapMs));
    }
  }

  #pumpAll(): void {
    for (const link of this.#links) {
      while (link.open && link.cursor < this.#next) {
        if (link.cursor < this.#oldest()) link.cursor = this.#oldest();
        const seq = link.cursor;
        const entry = this.#log[seq % this.#opts.logCapacity];
        link.cursor++;
        if (!entry) continue;
        if (this.#dropEvents > 0) { this.#dropEvents--; continue; }
        const head = new Uint8Array(PROTO.headerBytes + 4);
        head.set(encodePacketHeader(entry.msgId));
        new DataView(head.buffer).setUint32(PROTO.headerBytes, seq, true);
        this.#send(link, frameEncode(head, entry.body));
      }
    }
  }
}

function encodePacketHeader(msgId: number): Uint8Array {
  return new Uint8Array([msgId, HeaderFlag.EVENT, 0, 0]);
}

/** One simulated cable: a Transport whose far end is a SimDevice. */
class SimTransport implements Transport {
  readonly kind: 'sim';
  readonly label = 'Simulator';
  #device: SimDevice;
  #link: ReturnType<SimDevice['attach']> | null = null;

  constructor(device: SimDevice, kind: 'sim') {
    this.#device = device;
    this.kind = kind;
  }

  async open(handlers: TransportHandlers): Promise<void> {
    this.#link = this.#device.attach({
      onBytes: (c) => handlers.onBytes(c),
      onClose: (r) => { this.#link = null; handlers.onClose(r); },
    });
  }

  async write(bytes: Uint8Array): Promise<void> {
    if (!this.#link) throw new Error('simulator link closed');
    this.#device.receive(this.#link, bytes);
  }

  async close(): Promise<void> {
    if (this.#link) this.#device.detach(this.#link);
    this.#link = null;
  }
}

