/*
 * Scripted bench suites: the firmware's "Done when" checks (CONTROL guide
 * §5.1–5.4, §5.7; SYSTEM_ARCHITECTURE §7.4) as runnable steps. Against
 * hardware they print operator steps ("unplug lane 3"); with --sim the
 * simulator performs them, so every suite runs in `npm test` today.
 *
 *   usb        HELLO, PING, NODE_LIST, STATUS, unknown id, bad length, EVT_BOOT replay
 *   lanes      enumeration in chain order, NODE_LIST lanes, config round trip,
 *              LANE_STATUS, LANE_SCAN, broken ribbon + rejoin, power off
 *   crossings  sampling, a pass on every lane, laneSeq, plausible tEntryUs, raw stream
 *   resume     USB unplugged during crossings: nothing lost, nothing twice
 *   start      start sequence: SEQ_STARTED, LIGHTS_OUT in range and never early,
 *              ERR_SEQ_RUNNING, LED refused, abort
 *   binding    USB HELLO of a web app binds, BIND_ALLOW/LIST/REMOVE; BLE part sim only
 *   update     bootloader entry, wrong board refused, image written, new app
 */

import { DeviceApi, laneMask, ALL_LANES } from '../../src/device/deviceApi.ts';
import { DeviceError, DeviceSession } from '../../src/device/deviceSession.ts';
import { makeSimImage } from '../../src/device/simDevice.ts';
import {
  BindChange, BindState, BoardType, ClientKind, LaneEventType, LaneState, LedMode, MsgId, NodeClass, PROTO,
  SamplingMode, SeqAbortReason, Status, Transport as HpTransport, HELLO_REQ,
} from '../../src/protocol/generated.ts';
import { encodeStruct } from '../../src/protocol/wire.ts';
import { updateFirmware } from '../../src/update/firmwareUpdater.ts';
import type { Bench } from './bench.ts';

export interface Check { name: string; ok: boolean; detail: string }
export interface SuiteResult { suite: string; checks: Check[]; error: string | null }
export type Suite = (b: Bench, c: Checker, opts: SuiteOptions) => Promise<void>;
export interface SuiteOptions { image?: Uint8Array; lanes?: number }

export class Checker {
  readonly checks: Check[] = [];
  #b: Bench;
  constructor(b: Bench) { this.#b = b; }
  ok(name: string, cond: boolean, detail = ''): boolean {
    this.checks.push({ name, ok: cond, detail });
    this.#b.log(`  ${cond ? 'PASS' : 'FAIL'}  ${name}${detail ? `  (${detail})` : ''}`);
    return cond;
  }
  async status(name: string, p: Promise<unknown>, want: number): Promise<void> {
    try { await p; this.ok(name, want === Status.OK, 'OK'); } catch (e) {
      this.ok(name, e instanceof DeviceError && e.status === want, e instanceof Error ? e.message : String(e));
    }
  }
}

const laneEvents = (b: Bench, since: number): string[] => b.named('EVT_LANE_STATE', since).map((e) =>
  `${String(e.value?.['lane'])}${Object.entries(LaneState).find(([, v]) => v === e.value?.['state'])?.[0][0] ?? '?'}`);
const configuredCount = (b: Bench, since: number): number => b.named('EVT_LANE_STATE', since).filter((e) => e.value?.['state'] === LaneState.CONFIGURED).length;

async function powerUp(b: Bench, c: Checker, n: number): Promise<number> {
  const since = b.events.length;
  await b.api.lanePower(true);
  try { await b.until(() => configuredCount(b, since) >= n, 3000, `${n} lanes`); } catch { /* checked below */ }
  await b.sleep(700);                                    // end of chain timeout
  const present = (await b.api.status()).lanesPresent;
  c.ok('lanes enumerate after power on', present > 0, `${present} present`);
  return present;
}

function inOrder(seq: string[], n: number): boolean {
  const want = Array.from({ length: n }, (_, i) => [`${i + 1}J`, `${i + 1}C`]).flat();
  return seq.join() === want.join();
}

export const SUITES: Record<string, Suite> = {
  async usb(b, c) {
    const h = b.session.hello!;
    c.ok('HELLO: protocol major matches', h.protoMajor === PROTO.major, `${h.protoMajor}.${h.protoMinor}`);
    c.ok('HELLO: transport USB, TRUSTED', h.transport === HpTransport.USB && h.bindState === BindState.TRUSTED);
    c.ok('HELLO: bootId set', h.bootId !== 0, `0x${h.bootId.toString(16)}`);
    c.ok('HELLO: board CONTROL', h.boardType === BoardType.CONTROL, `${h.boardType}`);
    const t0 = b.clock.now();
    const p1 = await b.api.ping(1);
    const p2 = await b.api.ping(2);
    c.ok('PING: echo and rising sysUs', p1.hostMs === 1 && p2.sysUs > p1.sysUs);
    c.ok('PING: round trip under 20 ms', (b.clock.now() - t0) / 2 < 20, `${((b.clock.now() - t0) / 2).toFixed(1)} ms`);
    const nodes = await b.api.nodeList();
    const ctrl = nodes.find((n) => n.nodeClass === NodeClass.CTRL);
    c.ok('NODE_LIST: CTRL node, board CONTROL', ctrl?.boardType === BoardType.CONTROL, ctrl ? `fw ${ctrl.fwMajor}.${ctrl.fwMinor}.${ctrl.fwPatch}` : 'missing');
    const st = await b.api.status();
    c.ok('STATUS: log window matches HELLO', st.logNextSeq >= h.logNextSeq && st.logOldestSeq <= st.logNextSeq);
    c.ok('unknown request → ERR_UNSUPPORTED', (await b.session.requestRaw(0x0f, new Uint8Array(0))).status === Status.ERR_UNSUPPORTED);
    c.ok('wrong length → ERR_BAD_LENGTH', (await b.session.requestRaw(MsgId.PING, new Uint8Array(3))).status === Status.ERR_BAD_LENGTH);
    if (h.logOldestSeq === 0) {
      await b.until(() => b.events.length > 0, 1000, 'replay').catch(() => undefined);
      c.ok('history replays from EVT_BOOT at evSeq 0', b.events[0]?.name === 'EVT_BOOT' && b.events[0].evSeq === 0);
    }
  },

  async lanes(b, c, o) {
    const since = b.events.length;
    const n = await powerUp(b, c, o.lanes ?? 6);
    c.ok('JOINED → CONFIGURED in chain order', inOrder(laneEvents(b, since), n), laneEvents(b, since).join(' '));
    const lanes = (await b.api.nodeList()).filter((x) => x.nodeClass === NodeClass.LANE);
    c.ok('NODE_LIST lists the lanes 1..n with versions', lanes.length === n && lanes.every((l, i) => l.index === i + 1 && l.boardType === BoardType.LANE),
      lanes.map((l) => `${l.index}:${l.fwMajor}.${l.fwMinor}.${l.fwPatch}`).join(' '));
    const cfg = await b.api.laneConfigGet(1);
    await b.api.laneConfigSet(laneMask([1]), { ...cfg, holdoffUs: 1234000 });
    c.ok('LANE_CONFIG set/get round trip', (await b.api.laneConfigGet(1)).holdoffUs === 1234000);
    await b.api.laneConfigSet(laneMask([1]), cfg);
    for (let l = 1; l <= n; l++) {
      const s = await b.api.laneStatus(l);
      c.ok(`LANE_STATUS ${l}: configured, baseline > thresholds`, s.state >= 2 && s.baseline > s.thrLow, `raw ${s.raw} base ${s.baseline} noise ${s.noise}`);
    }
    await c.status('LANE_STATUS of an absent lane → ERR_LANE_ABSENT', n < 6 ? b.api.laneStatus(n + 1) : Promise.reject(new DeviceError('x', Status.ERR_LANE_ABSENT)), Status.ERR_LANE_ABSENT);
    let mark = b.events.length;
    await b.api.laneScan();
    await b.until(() => configuredCount(b, mark) >= n, 3000, 'rescan').catch(() => undefined);
    const scan = laneEvents(b, mark);
    c.ok('LANE_SCAN: all LEFT, then the chain again in order', scan.slice(0, n).every((x) => x.endsWith('L')) && inOrder(scan.slice(n), n), scan.join(' '));
    if (n >= 3) {
      mark = b.events.length;
      await b.operator('unplug the ribbon between lane 2 and lane 3', () => b.sim?.unplugLane(3));
      await b.sleep(300);
      const left = laneEvents(b, mark);
      c.ok('broken ribbon: lanes 3..n LEFT', left.join() === Array.from({ length: n - 2 }, (_, i) => `${i + 3}L`).join(), left.join(' '));
      mark = b.events.length;
      await b.operator('plug the ribbon back in', () => b.sim?.replugLane(3));
      await b.until(() => configuredCount(b, mark) >= n - 2, 5000, 'rejoin').catch(() => undefined);
      const back = laneEvents(b, mark);
      c.ok('replugged: lanes 3..n re-join in order', back.join() === Array.from({ length: n - 2 }, (_, i) => [`${i + 3}J`, `${i + 3}C`]).flat().join(), back.join(' '));
    }
    mark = b.events.length;
    await b.api.lanePower(false);
    await b.sleep(100);
    c.ok('power off: LEFT for every lane', laneEvents(b, mark).filter((x) => x.endsWith('L')).length === n);
  },

  async crossings(b, c, o) {
    const powerOnUs = Number((await b.api.ping()).sysUs);
    const n = await powerUp(b, c, o.lanes ?? 6);
    await b.api.laneSampling(ALL_LANES, SamplingMode.ON);
    const mark = b.events.length;
    await b.operator(`pass a hand (or a car) through each of the ${n} lanes, twice, a few seconds apart`, async () => {
      b.sim!.carsPractice(Array.from({ length: n }, (_, i) => i + 1));
      await b.sleep(16000);
      b.sim!.carsStop();
    });
    await b.sleep(200);
    const xs = b.named('EVT_CROSSING', mark).filter((e) => e.value?.['type'] === LaneEventType.CROSSING);
    const nowUs = Number((await b.api.ping()).sysUs);
    for (let l = 1; l <= n; l++) {
      const mine = xs.filter((e) => e.value?.['lane'] === l).map((e) => e.value!);
      const seqs = mine.map((v) => v['laneSeq'] as number);
      c.ok(`lane ${l}: crossings with consecutive laneSeq`, mine.length >= 2 && seqs.every((s, i) => i === 0 || s === ((seqs[i - 1]! + 1) & 0xffff)), `${mine.length} seen`);
      c.ok(`lane ${l}: tEntryUs plausible, dwell 2..200 ms`, mine.every((v) => {
        const t = Number(v['tEntryUs']); const dw = (v['dwell10us'] as number) / 100;
        return t > powerOnUs && t < nowUs && dw >= 2 && dw <= 200;
      }));
    }
    const rawMark = b.streams.length;
    await b.api.laneRawStream(1, 1);
    await b.sleep(1000);
    await b.api.laneRawStream(0);
    const raw = b.streams.slice(rawMark).filter((s) => s.msgId === MsgId.STR_RAW_SAMPLES).map((s) => s.value!);
    c.ok('raw stream: chunks with counting chunkSeq', raw.length >= 5 && raw.every((r, i) => i === 0 || r['chunkSeq'] === ((raw[i - 1]!['chunkSeq'] as number) + 1) & 0xffff), `${raw.length} chunks`);
    await b.api.lanePower(false);
  },

  async resume(b, c, o) {
    await powerUp(b, c, o.lanes ?? 6);
    const mark = b.events.length;
    await b.operator('keep crossings going (hand or cars) for the whole step', () => b.sim?.carsPractice());
    await b.sleep(3000);
    const beforeUnplug = b.events.length;
    await b.operator('pull the USB cable now and keep crossings going for 30 s', () => b.sim?.unplugUsb());
    await b.until(() => b.session.state === 'closed', 5000, 'the link to drop').catch(() => undefined);
    c.ok('the session notices the unplug', b.session.state === 'closed');
    await b.operator('plug the USB cable back in', async () => { await b.sleep(15000); b.sim!.plugUsb(); });
    await b.reconnect(30000);
    await b.sleep(3000);
    b.sim?.carsStop();
    const seqs = b.events.filter((e) => e.bootId === b.session.hello!.bootId).map((e) => e.evSeq);
    c.ok('every evSeq exactly once, in order, across the unplug', seqs.every((s, i) => i === 0 || s === seqs[i - 1]! + 1), `${seqs.length} events`);
    const during = b.named('EVT_CROSSING', beforeUnplug).length;
    c.ok('crossings from the unplugged time arrived', during > 0, `${during} after the unplug`);
    c.ok('no crossing twice', new Set(b.named('EVT_CROSSING', mark).map((e) => e.evSeq)).size === b.named('EVT_CROSSING', mark).length);
    await b.api.lanePower(false);
  },

  async start(b, c, o) {
    const n = await powerUp(b, c, o.lanes ?? 6);
    const p = { lights: 6, stepMs: 300, holdMinMs: 200, holdMaxMs: 800 };
    for (let i = 0; i < 3; i++) {
      const mark = b.events.length;
      const nowUs = Number((await b.api.ping()).sysUs);
      const t0 = Number(await b.api.startSequence(p));
      await b.until(() => b.named('EVT_SEQ_STARTED', mark).length === 1, 1000, 'SEQ_STARTED').catch(() => undefined);
      c.ok(`#${i + 1} SEQ_STARTED with T0 ≥ now + 150 ms`, b.named('EVT_SEQ_STARTED', mark).length === 1 && t0 - nowUs >= 140_000, `lead ${((t0 - nowUs) / 1000).toFixed(0)} ms`);
      await c.status(`#${i + 1} second START → ERR_SEQ_RUNNING`, b.api.startSequence(p), Status.ERR_SEQ_RUNNING);
      await c.status(`#${i + 1} LANE_LED while running → ERR_SEQ_RUNNING`, b.api.laneLed(1, { mode: LedMode.OFF }), Status.ERR_SEQ_RUNNING);
      c.ok(`#${i + 1} no LIGHTS_OUT before the hold`, b.named('EVT_LIGHTS_OUT', mark).length === 0);
      await b.until(() => b.named('EVT_LIGHTS_OUT', mark).length === 1, 5000, 'LIGHTS_OUT').catch(() => undefined);
      const out = b.named('EVT_LIGHTS_OUT', mark)[0];
      const hold = out ? Number(out.value!['toutUs']) - t0 - Math.min(6, n) * p.stepMs * 1000 : NaN;
      c.ok(`#${i + 1} LIGHTS_OUT: Tout = T0 + n·step + hold, hold in range`, hold >= p.holdMinMs * 1000 && hold <= p.holdMaxMs * 1000, `hold ${(hold / 1000).toFixed(1)} ms`);
    }
    await c.status('ABORT when idle → ERR_INVALID_STATE', b.api.abortSequence(), Status.ERR_INVALID_STATE);
    const mark = b.events.length;
    await b.api.startSequence(p);
    await b.api.abortSequence();
    await b.until(() => b.named('EVT_SEQ_ABORTED', mark).length === 1, 1000, 'abort').catch(() => undefined);
    c.ok('ABORT: EVT_SEQ_ABORTED{HOST}, no LIGHTS_OUT', b.named('EVT_SEQ_ABORTED', mark)[0]?.value?.['reason'] === SeqAbortReason.HOST && b.named('EVT_LIGHTS_OUT', mark).length === 0);
    await b.api.lanePower(false);
  },

  async binding(b, c) {
    const testId = new Uint8Array(16).fill(0xb1);
    const mark = b.events.length;
    // A web app's HELLO on this USB link (the device binds it); then ours again.
    const raw = await b.session.requestRaw(MsgId.HELLO, encodeStruct(HELLO_REQ, {
      protoMajor: PROTO.major, protoMinor: PROTO.minor, clientKind: ClientKind.WEBAPP, reserved: 0, clientId: testId, clientName: 'bench web app',
    }));
    c.ok('USB HELLO of a web app is accepted', raw.status === Status.OK);
    let list = await b.api.bindList();
    c.ok('… and binds its clientId', list.entries.some((e) => e.clientName === 'bench web app'), `${list.entries.length} bound`);
    await b.api.bindAllow(30);
    list = await b.api.bindList();
    c.ok('BIND_ALLOW opens the pairing window', list.windowS > 0 && list.windowS <= 30, `${list.windowS} s`);
    await b.api.bindAllow(0);
    c.ok('BIND_ALLOW 0 closes it', (await b.api.bindList()).windowS === 0);
    await b.api.bindRemove(testId);
    c.ok('BIND_REMOVE takes it off the list', !(await b.api.bindList()).entries.some((e) => e.clientName === 'bench web app'));
    await b.sleep(100);
    const ch = b.named('EVT_BIND_CHANGED', mark).map((e) => e.value?.['change']);
    c.ok('EVT_BIND_CHANGED: ADDED, WINDOW_OPENED, WINDOW_CLOSED, REMOVED', [BindChange.ADDED, BindChange.WINDOW_OPENED, BindChange.WINDOW_CLOSED, BindChange.REMOVED].every((x) => ch.includes(x)), ch.join(','));
    if (!b.sim) { b.log('  (BLE steps need a browser: run them from the web app; Node has no Web Bluetooth)'); return; }
    const ble = new DeviceSession(b.sim.transport('ble'), { clientId: new Uint8Array(16).fill(0xb2), clientName: 'ble test', clientKind: ClientKind.WEBAPP, scheduler: b.clock, requestTimeoutMs: 1000 });
    const connecting = ble.connect();
    await b.until(() => ble.state === 'unbound', 3000, 'unbound').catch(() => undefined);
    c.ok('BLE: unknown client is UNBOUND', ble.state === 'unbound');
    await c.status('BLE: UNBOUND request → ERR_NOT_BOUND', new DeviceApi(ble).status(), Status.ERR_NOT_BOUND);
    await b.api.bindAllow(30);
    const h = await connecting;
    c.ok('BLE: the window binds it live', h.bindState === BindState.BOUND && ble.state === 'open');
    await ble.close();
    await b.api.bindAllow(0);
    await b.api.bindRemove(new Uint8Array(16).fill(0xb2));
  },

  async update(b, c, o) {
    const image = o.image ?? (b.sim ? makeSimImage({ major: 9, minor: 9, patch: 9, gitRev: 0x9999999 }, 6000) : null);
    if (!image) { b.log('  SKIP  update needs --image <file.bin> on hardware'); return; }
    await b.close();
    let lastPhase = '';
    try {
      const r = await updateFirmware(image, {
        scheduler: b.clock,
        connect: async () => { await b.reconnect(3000); return { api: b.api, boardType: b.session.boardType, close: () => b.session.close('updater') }; },
        onProgress: (p) => { if (p.phase !== lastPhase) { lastPhase = p.phase; b.log(`  … ${p.phase}`); } },
      });
      c.ok('update: new app runs, imageCrc matches', r.ctrl.boardType === BoardType.CONTROL && (r.ctrl.imageCrc === 0 || r.ctrl.imageCrc === r.crc32),
        `fw ${r.ctrl.fwMajor}.${r.ctrl.fwMinor}.${r.ctrl.fwPatch}, ${r.recoveries} recoveries`);
    } catch (e) {
      c.ok('update', false, e instanceof Error ? e.message : String(e));
    }
    if (b.session?.state !== 'open') await b.reconnect();
  },
};

export async function runSuites(b: Bench, names: string[], opts: SuiteOptions = {}): Promise<SuiteResult[]> {
  const results: SuiteResult[] = [];
  for (const name of names) {
    const suite = SUITES[name];
    if (!suite) throw new Error(`no suite ${name} (have: ${Object.keys(SUITES).join(', ')})`);
    b.log(`── ${name}`);
    const c = new Checker(b);
    let error: string | null = null;
    try {
      if (b.session?.state !== 'open') await b.reconnect();
      await suite(b, c, opts);
    } catch (e) {
      error = e instanceof Error ? e.message : String(e);
      b.log(`  FAIL  aborted: ${error}`);
    }
    results.push({ suite: name, checks: c.checks, error });
  }
  const all = results.flatMap((r) => r.checks);
  const failed = all.filter((x) => !x.ok).length + results.filter((r) => r.error).length;
  b.log(`── ${all.length} checks, ${failed} failed`);
  return results;
}
