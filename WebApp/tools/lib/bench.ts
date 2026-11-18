/*
 * The bench context the CLI commands and suites run in: one device (a tty
 * port or a SimDevice), the current session with its event/stream log, a
 * reconnect that keeps the resume point, operator prompts (hardware: print
 * and wait for Enter; simulator: perform the action), and event formatting.
 */

import { createInterface } from 'node:readline/promises';

import { DeviceApi } from '../../src/device/deviceApi.ts';
import { DeviceSession } from '../../src/device/deviceSession.ts';
import type { DeviceEvent, DeviceStream, ResumePoint } from '../../src/device/deviceSession.ts';
import type { SimDevice } from '../../src/device/simDevice.ts';
import {
  BindChange, BleLinkState, ButtonId, CalibrateResult, ClientKind, FaultSource, LaneEventFlag, LaneEventType, LaneState, LogLevel, MsgId,
  PressKind, ResetCause, SeqAbortReason, statusName,
} from '../../src/protocol/generated.ts';
import type { Transport } from '../../src/transport/transport.ts';
import { sleep, waitFor } from '../../src/util/clock.ts';
import type { Scheduler } from '../../src/util/clock.ts';
import { toHex } from '../../src/util/bytes.ts';
import type { Check, HealthReport } from '../../src/health/health.ts';
import type { CalibrationOutcome, LaneSample } from '../../src/health/monitor.ts';
import { laneStatusFlagNames } from '../../src/device/laneFlags.ts';
import { findTimeTaker, TtyTransport } from './ttyTransport.ts';

export const BENCH_CLIENT_ID = new Uint8Array([0x7e, 0x57, 0xbe, 0x0c, 0x4c, 0x11, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1]);

export interface BenchOptions {
  readonly clock: Scheduler;
  readonly sim?: SimDevice | null;
  /** Hardware: the tty path, or undefined to find the TimeTaker port. */
  readonly port?: string;
  readonly out?: (line: string) => void;
  /** Hardware prompts: wait for Enter (false in tests). */
  readonly interactive?: boolean;
  readonly requestTimeoutMs?: number;
}

export class Bench {
  readonly clock: Scheduler;
  readonly sim: SimDevice | null;
  readonly events: DeviceEvent[] = [];
  readonly streams: DeviceStream[] = [];
  session!: DeviceSession;
  api!: DeviceApi;
  watch = false;
  /** Extra event consumer (the HealthMonitor of `status` / `sensors` / `calibrate`). */
  onEvent: ((e: DeviceEvent) => void) | null = null;
  #o: BenchOptions;
  #resume: ResumePoint | null = null;

  constructor(o: BenchOptions) {
    this.#o = o;
    this.clock = o.clock;
    this.sim = o.sim ?? null;
  }

  get isSim(): boolean { return this.sim !== null; }

  log(line: string): void { (this.#o.out ?? console.log)(line); }

  transport(): Transport {
    if (this.sim) return this.sim.transport('usb');
    const path = this.#o.port ?? findTimeTaker()[0]?.path;
    if (!path) throw new Error('no TimeTaker on USB (see `tt ports`)');
    return new TtyTransport(path);
  }

  /** Opens a session (keeps the resume point across reconnects). */
  async connect(): Promise<DeviceSession> {
    const s = new DeviceSession(this.transport(), {
      clientId: BENCH_CLIENT_ID, clientName: 'tt bench', clientKind: ClientKind.TOOL, scheduler: this.clock,
      requestTimeoutMs: this.#o.requestTimeoutMs ?? 1000,
    }, {
      onEvent: (e) => { this.events.push(e); this.onEvent?.(e); if (this.watch) this.log(formatEvent(e)); },
      onStream: (st) => { this.streams.push(st); if (this.watch && st.msgId === MsgId.DBG_LOG) this.log(formatStream(st)); },
      onGap: (b, from, to) => this.log(`! events ${from}..${to - 1} of boot ${hex32(b)} were lost (overwritten on the device)`),
      onBoot: (b, prev) => { if (prev !== null) this.log(`! device rebooted: boot ${hex32(prev)} → ${hex32(b)}`); },
    }, this.#resume);
    s.closed().then(() => { this.#resume = s.resumePoint; }, () => undefined);
    await s.connect();
    this.session = s;
    this.api = new DeviceApi(s);
    return s;
  }

  /** Reconnects (after an unplug or reboot), retrying until timeoutMs. */
  async reconnect(timeoutMs = 15000): Promise<DeviceSession> {
    if (this.session && this.session.state !== 'closed') await this.session.close('reconnect');
    this.#resume = this.session?.resumePoint ?? this.#resume;
    const end = this.clock.now() + timeoutMs;
    for (;;) {
      try { return await this.connect(); } catch (e) {
        if (this.clock.now() >= end) throw e;
        await sleep(this.clock, 300);
      }
    }
  }

  async close(): Promise<void> { await this.session?.close('bench done'); }

  sleep(ms: number): Promise<void> { return sleep(this.clock, ms); }
  until(cond: () => boolean, ms: number, what: string): Promise<void> { return waitFor(this.clock, cond, ms, what, 5); }

  named(name: string, since = 0): DeviceEvent[] { return this.events.slice(since).filter((e) => e.name === name); }

  /** A step a person does at the bench; the simulator does it itself. */
  async operator(instruction: string, simAction?: () => void | Promise<void>): Promise<void> {
    if (this.sim) { this.log(`  (sim) ${instruction}`); await simAction?.(); return; }
    this.log(`  >>> ${instruction}`);
    if (this.#o.interactive === false) return;
    const rl = createInterface({ input: process.stdin, output: process.stdout });
    await rl.question('      press Enter when done ');
    rl.close();
  }
}

// ---- formatting --------------------------------------------------------------------

export const hex32 = (n: number): string => `0x${(n >>> 0).toString(16).padStart(8, '0')}`;
export const nameOf = (table: Readonly<Record<string, number>>, v: unknown): string =>
  Object.entries(table).find(([, x]) => x === v)?.[0] ?? String(v);
const flagNames = (bits: number): string =>
  Object.entries(LaneEventFlag).filter(([, v]) => bits & v).map(([k]) => k).join('|') || '-';

export function fmtUs(us: unknown): string {
  if (typeof us !== 'bigint') return '-';
  const s = Number(us) / 1e6;
  return `${s.toFixed(6)} s`;
}

export function formatEvent(e: DeviceEvent): string {
  const v = e.value;
  const head = `#${String(e.evSeq).padStart(5)} ${e.name.padEnd(16)}`;
  if (!v) return `${head} (unknown to this tool)`;
  const t = v['tEntryUs'] ?? v['toutUs'] ?? v['atUs'] ?? v['sysUs'] ?? v['t0Us'];
  let d: string;
  switch (e.msgId) {
    case MsgId.EVT_BOOT: d = `${nameOf(ResetCause, v['resetCause'])} boot ${hex32(v['bootId'] as number)}`; break;
    case MsgId.EVT_CROSSING:
      d = `lane ${String(v['lane'])} ${nameOf(LaneEventType, v['type'])} dwell ${((v['dwell10us'] as number) / 100).toFixed(2)} ms`
        + ` depth ${String(v['depth'])} seq ${String(v['laneSeq'])} flags ${flagNames(v['flags'] as number)}`;
      break;
    case MsgId.EVT_LANE_STATE: d = `lane ${String(v['lane'])} ${nameOf(LaneState, v['state'])}${v['status'] ? ` ${statusName(v['status'] as number)}` : ''}`; break;
    case MsgId.EVT_BUTTON: d = `${nameOf(ButtonId, v['button'])} ${nameOf(PressKind, v['press'])}`; break;
    case MsgId.EVT_SEQ_STARTED: d = `lights ${String(v['lights'])} step ${String(v['stepMs'])} ms T0 ${fmtUs(v['t0Us'])}`; break;
    case MsgId.EVT_LIGHTS_OUT: d = `lights ${String(v['lights'])}`; break;
    case MsgId.EVT_SEQ_ABORTED: d = nameOf(SeqAbortReason, v['reason']); break;
    case MsgId.EVT_BIND_CHANGED: d = `${nameOf(BindChange, v['change'])} count ${String(v['count'])} window ${String(v['windowS'])} s id ${toHex(v['clientIdPrefix'] as Uint8Array)}…`; break;
    case MsgId.EVT_BLE_LINK: d = `${nameOf(BleLinkState, v['state'])} phy ${String(v['phy'])} mtu ${String(v['attMtu'])} interval ${String((v['intervalUnits'] as number) * 1.25)} ms reason 0x${(v['reason'] as number).toString(16)}`; break;
    case MsgId.EVT_FAULT: d = `${nameOf(FaultSource, v['source'])} ${statusName(v['status'] as number)}`; break;
    case MsgId.EVT_LANE_CALIBRATED: d = `lane ${String(v['lane'])} ${nameOf(CalibrateResult, v['result'])} reference ${String(v['reference'])} noise ${String(v['noise'])}`; break;
    default: d = JSON.stringify(v, (_k, x: unknown) => (typeof x === 'bigint' ? x.toString() : x instanceof Uint8Array ? toHex(x) : x));
  }
  return `${head} ${fmtUs(t).padStart(16)}  ${d}`;
}

export function formatStream(s: DeviceStream): string {
  const v = s.value;
  if (s.msgId === MsgId.DBG_LOG && v) return `  log ${nameOf(LogLevel, v['level'])} ${fmtUs(v['sysUs'])} ${String(v['text'])}`;
  if (s.msgId === MsgId.STR_RAW_SAMPLES && v) {
    const smp = v['samples'] as number[];
    return `  raw lane ${String(v['lane'])} chunk ${String(v['chunkSeq'])} @${fmtUs(v['firstUs'])} /${String(v['periodUs'])} µs: ${smp.join(' ')}`;
  }
  return `  ${s.name}`;
}

// ---- health ------------------------------------------------------------------------

const MARK: Record<string, string> = { ok: 'ok  ', warn: 'WARN', fail: 'FAIL', unknown: ' -- ' };

/** The health report as text: one block per part, one line per check (unknown checks of offline parts folded). */
export function formatReport(r: HealthReport): string[] {
  const out: string[] = [`overall: ${r.overall.toUpperCase()}`];
  const checks = (cs: readonly Check[]): void => {
    if (cs.slice(1).every((c) => c.level === 'unknown') && cs.length > 1) {
      out.push(`  ${MARK[cs[0]!.level]} ${cs[0]!.id.padEnd(20)} ${cs[0]!.message}`);
      return;
    }
    for (const c of cs) out.push(`  ${MARK[c.level]} ${c.id.padEnd(20)} ${c.message}`);
  };
  out.push(`control module: ${r.control.online ? 'online' : 'OFFLINE'}`);
  checks(r.control.checks);
  out.push(`BLE chip: ${r.ble.online ? 'online' : 'OFFLINE'}, ${r.ble.connected ? 'phone connected' : 'no phone connected'}`);
  checks(r.ble.checks);
  for (const l of r.lanes) {
    out.push(`lane ${l.index}: ${l.online ? 'online' : 'offline'}`);
    checks(l.checks);
  }
  return out;
}

export function formatSample(lane: number, s: LaneSample): string {
  const flags = laneStatusFlagNames(s.flags).filter((f) => f === 'BLOCKED' || f === 'SYNC_LOST' || f === 'FIFO_OVERFLOW').join(' ');
  return `lane ${lane}  raw ${String(s.raw).padStart(5)} [${String(s.rawMin).padStart(5)}..${String(s.rawMax).padStart(5)}]`
    + `  base ${String(s.baseline).padStart(5)}  thr ${s.thrLow}/${s.thrHigh}  ref ${String(s.reference).padStart(5)}`
    + `  noise ${String(s.noise).padStart(3)}${s.reference ? `  ${Math.round((s.baseline / s.reference) * 100)} %` : ''}${flags ? `  ${flags}` : ''}`;
}

export function formatOutcome(o: CalibrationOutcome): string {
  const what = o.timedOut ? 'FAILED (no answer in time)' : o.result === 'saved' ? `saved reference ${o.reference}`
    : o.result === 'notSaved' ? `learned ${o.reference}, NOT saved (flash write failed): used until power-off`
    : o.result === 'forgotten' ? 'saved reference forgotten (learns at every power-up)' : 'FAILED (lane absent, busy or left)';
  return `lane ${o.lane}: ${what}${o.result === 'saved' || o.result === 'notSaved' ? `, noise ${o.noise}` : ''}`;
}
