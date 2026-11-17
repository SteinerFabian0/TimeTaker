/*
 * DeviceSession against the simulated control module: the app-side contract
 * of SYSTEM_ARCHITECTURE §7.4, end to end through real frames.
 *
 *   - connect: HELLO, then the history replays from EVT_BOOT;
 *   - requests resolve, error statuses reject with their names;
 *   - events arrive in evSeq order, exactly once, even when frames are lost;
 *   - a reconnect (or a transport switch) continues where it stopped;
 *   - a device reboot shows as a new bootId.
 */

import assert from 'node:assert/strict';
import { afterEach, describe, test } from 'node:test';

import { DeviceError, DeviceSession } from '../src/device/deviceSession.ts';
import type { DeviceEvent, ResumePoint, SessionListener } from '../src/device/deviceSession.ts';
import { SimDevice } from '../src/device/simDevice.ts';
import {
  LANE_POWER_REQ, LANE_POWER_RSP, MESSAGE_BY_ID, MsgId, NODE_LIST_REQ, NODE_LIST_RSP, PressKind, REBOOT_REQ,
  REBOOT_RSP, RebootTarget, Status,
} from '../src/protocol/generated.ts';
import type { LanePowerRsp, NodeListRsp } from '../src/protocol/generated.ts';

const sleep = (ms: number): Promise<void> => new Promise((r) => setTimeout(r, ms));

async function until(cond: () => boolean, ms = 2000): Promise<void> {
  const end = Date.now() + ms;
  while (!cond()) {
    if (Date.now() > end) throw new Error('condition not met in time');
    await sleep(5);
  }
}

const opts = { clientId: new Uint8Array(16).fill(7), clientName: 'test', requestTimeoutMs: 500, pingIntervalMs: 50 };

interface Recorder extends SessionListener {
  events: DeviceEvent[];
  boots: number[];
  gaps: [number, number][];
}

function recorder(): Recorder {
  const r: Recorder = {
    events: [], boots: [], gaps: [],
    onEvent: (e) => { r.events.push(e); },
    onBoot: (b) => { r.boots.push(b); },
    onGap: (_b, from, to) => { r.gaps.push([from, to]); },
  };
  return r;
}

const devices: SimDevice[] = [];
const sessions: DeviceSession[] = [];
afterEach(async () => {
  for (const s of sessions.splice(0)) await s.close();
  for (const d of devices.splice(0)) d.dispose();
});

function sim(o: ConstructorParameters<typeof SimDevice>[0] = {}): SimDevice {
  const d = new SimDevice({ seed: 42, ...o });
  devices.push(d);
  return d;
}

function session(dev: SimDevice, rec: SessionListener, resume: ResumePoint | null = null): DeviceSession {
  const s = new DeviceSession(dev.transport(), opts, rec, resume);
  sessions.push(s);
  return s;
}

const seqs = (r: Recorder): number[] => r.events.map((e) => e.evSeq);
const isExactly = (r: Recorder, n: number): boolean => seqs(r).join() === Array.from({ length: n }, (_, i) => i).join();

describe('connect', () => {
  test('HELLO answers, the history replays from EVT_BOOT', async () => {
    const dev = sim();
    const rec = recorder();
    const s = session(dev, rec);
    const hello = await s.connect();
    assert.equal(hello.bootId, dev.bootId);
    assert.equal(s.state, 'open');
    assert.deepEqual(rec.boots, [dev.bootId]);
    await until(() => rec.events.length >= 1);
    assert.equal(rec.events[0]?.name, 'EVT_BOOT');
    assert.equal(rec.events[0]?.evSeq, 0);
  });

  test('the clock estimate follows the device', async () => {
    const dev = sim();
    const s = session(dev, recorder());
    await s.connect();
    await sleep(120);
    const est = s.sysUsNow();
    assert.ok(est !== null);
    const diffUs = Number(est - dev.sysUs());
    assert.ok(Math.abs(diffUs) < 20000, `estimate off by ${diffUs} µs`);
    assert.ok((s.rttMs() ?? 99) < 50);
  });
});

describe('requests', () => {
  test('NODE_LIST and LANE_POWER resolve with decoded bodies', async () => {
    const dev = sim();
    const s = session(dev, recorder());
    await s.connect();
    const before = await s.request<NodeListRsp>(NODE_LIST_REQ, {}, MsgId.NODE_LIST, NODE_LIST_RSP);
    assert.equal(before.nodes.length, 1);
    await s.request<LanePowerRsp>(LANE_POWER_REQ, { on: 1 }, MsgId.LANE_POWER, LANE_POWER_RSP);
    await until(() => dev.lanesPowered);
    const after = await s.request<NodeListRsp>(NODE_LIST_REQ, {}, MsgId.NODE_LIST, NODE_LIST_RSP);
    assert.equal(after.nodes.length, 7);
  });

  test('an error status rejects with DeviceError and its name', async () => {
    const dev = sim();
    const s = session(dev, recorder());
    await s.connect();
    await assert.rejects(
      s.request(REBOOT_REQ, { target: RebootTarget.BOOTLOADER }, MsgId.REBOOT, REBOOT_RSP),
      (err: unknown) => err instanceof DeviceError && err.status === Status.ERR_UNSUPPORTED
        && /ERR_UNSUPPORTED/.test(err.message),
    );
  });

  test('requestMsg drives any request by descriptor', async () => {
    const dev = sim();
    const s = session(dev, recorder());
    await s.connect();
    const msg = MESSAGE_BY_ID.get(MsgId.LANE_POWER);
    assert.ok(msg);
    const rsp = await s.requestMsg(msg, { on: 0 });
    assert.equal(rsp['status'], Status.OK);
  });
});

describe('events', () => {
  test('arrive in order, exactly once', async () => {
    const dev = sim();
    const rec = recorder();
    const s = session(dev, rec);
    await s.connect();
    for (let i = 0; i < 20; i++) dev.pressButton(1 + (i % 3), PressKind.SHORT);
    await until(() => rec.events.length === 21);
    assert.ok(isExactly(rec, 21));
  });

  test('lost frames are noticed and RESUMEd: still exactly once, in order', async () => {
    const dev = sim();
    const rec = recorder();
    const s = session(dev, rec);
    await s.connect();
    await until(() => rec.events.length === 1);
    dev.dropNextEvents(3);
    for (let i = 0; i < 10; i++) dev.pressButton(2, PressKind.LONG);
    await until(() => rec.events.length === 11);
    assert.ok(isExactly(rec, 11), seqs(rec).join());
    assert.deepEqual(rec.gaps, []);
  });

  test('events overwritten on the device are reported as a gap, the rest delivered', async () => {
    const dev = sim({ logCapacity: 8 });
    const rec = recorder();
    const s = session(dev, rec);
    await s.connect();
    await until(() => rec.events.length === 1);
    await s.close();

    for (let i = 0; i < 20; i++) dev.pressButton(1, PressKind.SHORT);   // 21 events in a log of 8
    const rec2 = recorder();
    const s2 = session(dev, rec2, s.resumePoint);
    await s2.connect();
    await until(() => rec2.events.length === 8);
    assert.deepEqual(rec2.gaps, [[1, 13]]);
    assert.deepEqual(seqs(rec2), [13, 14, 15, 16, 17, 18, 19, 20]);
  });
});

describe('reconnect', () => {
  test('a new session with the resume point continues without repeats (transport switch)', async () => {
    const dev = sim();
    const rec = recorder();
    const s1 = session(dev, rec);
    await s1.connect();
    dev.pressButton(1, PressKind.SHORT);
    await until(() => rec.events.length === 2);
    await s1.close('USB unplugged');

    dev.pressButton(2, PressKind.SHORT);           // while nobody listens
    dev.pressButton(3, PressKind.SHORT);
    const s2 = session(dev, rec, s1.resumePoint);   // "BLE" now
    await s2.connect();
    dev.pressButton(1, PressKind.LONG);
    await until(() => rec.events.length === 5);
    assert.ok(isExactly(rec, 5), seqs(rec).join());
    assert.deepEqual(rec.boots, [dev.bootId]);       // same boot: no second onBoot
  });

  test('a device reboot closes the link and shows a new bootId on reconnect', async () => {
    const dev = sim();
    const rec = recorder();
    const s1 = session(dev, rec);
    await s1.connect();
    const firstBoot = dev.bootId;
    await s1.request(REBOOT_REQ, { target: RebootTarget.APP }, MsgId.REBOOT, REBOOT_RSP);
    await until(() => s1.state === 'closed');

    const s2 = session(dev, rec, s1.resumePoint);
    await s2.connect();
    assert.notEqual(dev.bootId, firstBoot);
    assert.deepEqual(rec.boots, [firstBoot, dev.bootId]);
    await until(() => rec.events.some((e) => e.bootId === dev.bootId && e.name === 'EVT_BOOT'));
  });
});

describe('simulated cars', () => {
  test('powered lanes join and cars cross', async () => {
    const dev = sim({ simulateCars: true, lanes: 2 });
    const rec = recorder();
    const s = session(dev, rec);
    await s.connect();
    await s.request(LANE_POWER_REQ, { on: 1 }, MsgId.LANE_POWER, LANE_POWER_RSP);
    await until(() => rec.events.filter((e) => e.name === 'EVT_CROSSING').length >= 2, 9000);
    const crossing = rec.events.find((e) => e.name === 'EVT_CROSSING');
    assert.ok(crossing?.value);
    assert.equal(typeof crossing.value['tEntryUs'], 'bigint');
    assert.equal(rec.events.filter((e) => e.name === 'EVT_LANE_STATE').length, 4);
  });
});
