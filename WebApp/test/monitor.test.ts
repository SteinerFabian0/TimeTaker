/*
 * HealthMonitor (src/health/monitor.ts): scheduling and gating on a fake
 * api, the single LANE_STATUS poller, the ring buffer, and calibration
 * (flow, timeout, refusal) against a fake and against the simulator.
 */

import assert from 'node:assert/strict';
import { describe, test } from 'node:test';

import { DeviceApi } from '../src/device/deviceApi.ts';
import { DeviceManager } from '../src/device/deviceManager.ts';
import type { DeviceEvent } from '../src/device/deviceSession.ts';
import { LaneStatusFlag } from '../src/device/laneFlags.ts';
import type { SimDevice } from '../src/device/simDevice.ts';
import { findCheck } from '../src/health/health.ts';
import { HealthMonitor, MonitorBusyError, Ring } from '../src/health/monitor.ts';
import type { MonitorApi, MonitorOptions, MonitorUpdate } from '../src/health/monitor.ts';
import {
  BleBridgeState, BleLinkState, BoardType, CalibrateAction, CalibrateResult, ClientKind, LaneBusState, LaneState, MsgId, NodeClass,
  NodeState, ResetCause, SamplingMode,
} from '../src/protocol/generated.ts';
import type { LaneStatusRsp, NodeInfo, StatusRsp } from '../src/protocol/generated.ts';
import { sleep, VirtualClock } from '../src/util/clock.ts';
import { named, recorder, rig } from './helpers.ts';
import type { Rig } from './helpers.ts';

// ---- a fake device api ---------------------------------------------------------------------

class FakeApi implements MonitorApi {
  lanes = [1, 2];
  seqRunning = 0;
  delayMs = 5;
  calls = { status: 0, nodeList: 0, lane: [0, 0, 0, 0, 0, 0, 0], calibrate: [] as [number, number][] };
  inflight = [0, 0, 0, 0, 0, 0, 0];
  maxInflight = 0;
  readonly clock: VirtualClock;
  constructor(clock: VirtualClock) { this.clock = clock; }

  async status(): Promise<StatusRsp> {
    this.calls.status++;
    await sleep(this.clock, this.delayMs);
    return {
      status: 0, lastError: 0, loopMaxMs: 2, errorCount: 0, uptimeMs: 1, lanesPresent: this.lanes.length, lanesPowered: 1,
      bleLink: BleLinkState.DOWN, seqRunning: this.seqRunning, usbFaults: 0, settingsWrites: 0, logOldestSeq: 0, logNextSeq: 0,
      bleBridge: BleBridgeState.OK, bleRssi: 127, bleCrcErrors: 0, bleUartErrors: 0, bleDataDropped: 0, nrfLinkCrcErrors: 0,
      settingsDefaults: 0, reserved: 0,
    };
  }

  async nodeList(): Promise<NodeInfo[]> {
    this.calls.nodeList++;
    await sleep(this.clock, this.delayMs);
    return this.lanes.map((index) => ({
      nodeClass: NodeClass.LANE, index, boardType: BoardType.LANE, state: NodeState.OK, fwMajor: 0, fwMinor: 1, fwPatch: 0,
      protoMinor: 3, fwGitRev: 0, imageCrc: 0,
    }));
  }

  async laneStatus(lane: number): Promise<LaneStatusRsp> {
    this.calls.lane[lane]!++;
    this.inflight[lane]!++;
    this.maxInflight = Math.max(this.maxInflight, this.inflight[lane]!);
    try {
      await sleep(this.clock, this.delayMs);
    } finally {
      this.inflight[lane]!--;
    }
    const n = this.calls.lane[lane]!;
    return {
      status: 0, lane, state: LaneBusState.SAMPLING, samplingMode: SamplingMode.ON, fifoCount: 0,
      flags: LaneStatusFlag.SYNC_LOCKED | LaneStatusFlag.SYNC_ALIGNED | LaneStatusFlag.SENSOR_OK | LaneStatusFlag.CONFIG_VALID | LaneStatusFlag.CALIBRATED,
      droppedEvents: 0, syncShifts: 0, badWrites: 0, lateActions: 0, lastError: 0, resetCause: ResetCause.POWER_ON, faultId: 0,
      uptimeMs: 1, raw: 3000 + n, baseline: 3000, thrLow: 2100, thrHigh: 2400, noise: 8, reference: 3000, rawMin: 2990, rawMax: 3010,
    };
  }

  async laneCalibrate(mask: number, action: CalibrateAction = CalibrateAction.LEARN): Promise<void> {
    this.calls.calibrate.push([mask, action]);
    await sleep(this.clock, this.delayMs);
  }
}

async function fakeMonitor(o: Partial<MonitorOptions> = {}): Promise<{ clock: VirtualClock; api: FakeApi; mon: HealthMonitor }> {
  const clock = new VirtualClock();
  const api = new FakeApi(clock);
  const mon = new HealthMonitor({ api, scheduler: clock, ...o });
  mon.setConnected(true);
  mon.start();
  await clock.advance(50);
  return { clock, api, mon };
}

const calEvent = (lane: number, result: number, reference = 3333): DeviceEvent => ({
  bootId: 1, evSeq: 10 + lane, msgId: MsgId.EVT_LANE_CALIBRATED, name: 'EVT_LANE_CALIBRATED',
  value: { lane, result, reference, noise: 9, reserved: 0, sysUs: 1000n },
} as unknown as DeviceEvent);

describe('monitor: scheduling and gating (fake api)', () => {
  test('idle: STATUS every 2 s, NODE_LIST once, LANE_STATUS at 4 Hz per present lane', async () => {
    const { clock, api, mon } = await fakeMonitor();
    await clock.advance(10_000);
    assert.ok(api.calls.status >= 5 && api.calls.status <= 7, `status ${api.calls.status}`);
    assert.equal(api.calls.nodeList, 1);
    for (const l of [1, 2]) assert.ok(api.calls.lane[l]! >= 38 && api.calls.lane[l]! <= 42, `lane ${l}: ${api.calls.lane[l]}`);
    assert.equal(api.calls.lane[3], 0);
    assert.equal(mon.laneRateHz(), 4);
    mon.stop();
  });

  test('NODE_LIST again when lanesPresent changes; then the new lane is polled too', async () => {
    const { clock, api, mon } = await fakeMonitor();
    await clock.advance(1000);
    api.lanes = [1, 2, 3];
    await clock.advance(5000);
    assert.equal(api.calls.nodeList, 2);
    assert.ok(api.calls.lane[3]! >= 10, `lane 3: ${api.calls.lane[3]}`);
    mon.stop();
  });

  test('race running (busy): 0.2 Hz per lane; busyLaneRateHz 0: off; the device seqRunning gates too', async () => {
    let racing = true;
    const { clock, api, mon } = await fakeMonitor({ busy: () => racing });
    const before = api.calls.lane[1]!;
    await clock.advance(20_000);
    const n = api.calls.lane[1]! - before;
    assert.ok(n >= 3 && n <= 5, `busy polls ${n}`);
    assert.equal(mon.laneRateHz(), 0.2);
    racing = false;
    await clock.advance(1000);
    assert.equal(mon.laneRateHz(), 4);
    mon.stop();

    const off = await fakeMonitor({ busyLaneRateHz: 0 });
    off.api.seqRunning = 1;
    await off.clock.advance(2100);                       // next STATUS sees seqRunning
    const b = off.api.calls.lane[1]!;
    await off.clock.advance(10_000);
    assert.equal(off.api.calls.lane[1], b);
    assert.equal(off.mon.busy(), true);
    off.mon.stop();
  });

  test('single poller: slow answers never overlap per lane, runSelfCheck goes through the same queue', async () => {
    const { clock, api, mon } = await fakeMonitor();
    api.delayMs = 400;                                   // longer than the 250 ms period
    const r = mon.runSelfCheck();
    await clock.advance(6000);
    await r;
    assert.equal(api.maxInflight, 1);
    mon.stop();
  });

  test('not connected: no requests; the report says offline', async () => {
    const { clock, api, mon } = await fakeMonitor();
    const updates: MonitorUpdate[] = [];
    mon.subscribe((u) => updates.push(u));
    mon.setConnected(false);
    const s = api.calls.status;
    const l = api.calls.lane[1]!;
    await clock.advance(5000);
    assert.equal(api.calls.status, s);
    assert.equal(api.calls.lane[1], l);
    assert.equal(mon.report().control.online, false);
    assert.ok(updates.some((u) => u.type === 'connection' && !u.connected));
    mon.stop();
  });

  test('ring buffer: samples cover historyMs, oldest first; markers too; Ring overwrites when full', async () => {
    const { clock, mon } = await fakeMonitor({ historyMs: 2000 });
    const updates: MonitorUpdate[] = [];
    mon.subscribe((u) => updates.push(u));
    await clock.advance(10_000);
    const s = mon.samples(1);
    assert.ok(s.length >= 7 && s.length <= 9, `${s.length} samples`);
    assert.ok(s.every((x) => x.tMs >= clock.now() - 2000));
    assert.ok(s.every((x, i) => i === 0 || x.tMs > s[i - 1]!.tMs));
    assert.deepEqual(Object.keys(s[0]!).sort(), ['baseline', 'flags', 'noise', 'raw', 'rawMax', 'rawMin', 'reference', 'tMs', 'thrHigh', 'thrLow']);
    assert.ok(updates.filter((u) => u.type === 'sample' && u.lane === 1).length >= 38);
    mon.onEvent(calEvent(1, CalibrateResult.SAVED));
    assert.equal(mon.markers(1).length, 1);
    await clock.advance(2500);
    assert.equal(mon.markers(1).length, 0);
    mon.stop();

    const r = new Ring<{ tMs: number }>(3);
    for (let i = 0; i < 5; i++) r.push({ tMs: i });
    assert.deepEqual(r.toArray().map((x) => x.tMs), [2, 3, 4]);
    r.pruneBefore(4);
    assert.deepEqual(r.toArray().map((x) => x.tMs), [4]);
  });
});

describe('monitor: calibration (fake api)', () => {
  test('LEARN of present lanes; resolves per lane from the events; absent lane fails without asking', async () => {
    const { clock, api, mon } = await fakeMonitor();
    const p = mon.calibrate([1, 2, 5]);
    await clock.advance(20);
    assert.deepEqual(api.calls.calibrate, [[0b11, CalibrateAction.LEARN]]);
    mon.onEvent(calEvent(2, CalibrateResult.NOT_SAVED, 3200));
    mon.onEvent(calEvent(1, CalibrateResult.SAVED, 3100));
    const out = await p;
    assert.deepEqual(out.map((o) => [o.lane, o.result, o.reference]), [[1, 'saved', 3100], [2, 'notSaved', 3200], [5, 'failed', 0]]);
    assert.equal(mon.markers(1)[0]!.label, 'saved 3100');
    assert.equal(findCheck(mon.report(), 'lane.savedReference', 2)!.level, 'warn');
    mon.stop();
  });

  test('timeout: no event within calibrateTimeoutMs → failed, timedOut, shown in the report and as a marker', async () => {
    const { clock, mon } = await fakeMonitor({ calibrateTimeoutMs: 15_000 });
    const p = mon.calibrate('all');
    let settled = false;
    void p.then(() => { settled = true; });
    await clock.advance(10_000);
    mon.onEvent(calEvent(1, CalibrateResult.SAVED));                // lane 1 answers late (deferred flash write)
    await clock.advance(4000);
    assert.equal(settled, false, 'still waiting for lane 2 before the timeout');
    await clock.advance(2000);
    assert.equal(settled, true, 'resolved by the 15 s timeout');
    const out = await p;
    assert.deepEqual(out.map((o) => [o.lane, o.result, o.timedOut ?? false]), [[1, 'saved', false], [2, 'failed', true]]);
    assert.equal(findCheck(mon.report(), 'lane.savedReference', 2)!.level, 'fail');
    assert.match(mon.markers(2)[0]!.label, /timed out/);
    mon.stop();
  });

  test('FORGET sends every requested index, present or not', async () => {
    const { clock, api, mon } = await fakeMonitor();
    const p = mon.forget([2, 6]);
    await clock.advance(20);
    assert.deepEqual(api.calls.calibrate, [[0b100010, CalibrateAction.FORGET]]);
    mon.onEvent(calEvent(2, CalibrateResult.FORGOTTEN, 0));
    mon.onEvent(calEvent(6, CalibrateResult.FORGOTTEN, 0));
    assert.deepEqual((await p).map((o) => o.result), ['forgotten', 'forgotten']);
    mon.stop();
  });

  test('refused while a race or start sequence runs, while another calibration runs, and when not connected', async () => {
    let racing = true;
    const { clock, api, mon } = await fakeMonitor({ busy: () => racing });
    await assert.rejects(mon.calibrate('all'), MonitorBusyError);
    await assert.rejects(mon.forget([1]), MonitorBusyError);
    racing = false;
    api.seqRunning = 1;
    await clock.advance(2100);
    await assert.rejects(mon.calibrate('all'), MonitorBusyError);
    api.seqRunning = 0;
    await clock.advance(2100);
    const p = mon.calibrate([1]);
    await assert.rejects(mon.calibrate([2]), /calibration is running/);
    mon.onEvent(calEvent(1, CalibrateResult.SAVED));
    await clock.advance(20);
    await p;
    mon.setConnected(false);
    await assert.rejects(mon.calibrate([1]), /not connected/);
    assert.equal(api.calls.calibrate.length, 1);
    mon.stop();
  });
});

// ---- against the simulator ----------------------------------------------------------------------

async function simMonitor(t: Rig, lanes = 2, o: Partial<MonitorOptions> = {}): Promise<{ dev: SimDevice; api: DeviceApi; mon: HealthMonitor }> {
  const dev = t.sim({ lanes });
  let mon: HealthMonitor | null = null;
  const rec = recorder();
  const s = t.session(dev, { ...rec, onEvent: (e) => { rec.events.push(e); mon?.onEvent(e); } });
  await s.connect();
  const api = new DeviceApi(s);
  mon = new HealthMonitor({ api, scheduler: t.clock, ...o });
  mon.setConnected(true);
  mon.start();
  await api.lanePower(true);
  await t.until(() => named(rec, 'EVT_LANE_STATE').filter((e) => e.value?.['state'] === LaneState.CONFIGURED).length >= lanes);
  await t.sleep(2500);
  return { dev, api, mon };
}

describe('monitor: calibration against the simulator', () => {
  test('LEARN: the graph shows the new reference at once, SAVED follows; ≈ the bare level; drift after a dirty strip', async () => {
    const t = rig();
    await t.run(async () => {
      const { dev, mon } = await simMonitor(t);
      const r0 = await mon.runSelfCheck();
      // A fresh device has nothing saved: only lane.savedReference warns.
      const notOk = r0.lanes.flatMap((l) => l.checks.filter((c) => c.level !== 'ok' && c.level !== 'unknown'));
      assert.deepEqual([...new Set(notOk.map((c) => `${c.id}:${c.level}`))], ['lane.savedReference:warn'], JSON.stringify(notOk));
      dev.setLaneDirt(1, 0.6);
      await t.sleep(12_000);                                        // the baseline tracks the dirty level
      const r1 = await mon.runSelfCheck();
      assert.equal(findCheck(r1, 'lane.drift', 1)!.level, 'fail');
      assert.equal(findCheck(r1, 'lane.drift', 2)!.level, 'ok');
      const tCal = t.clock.now();
      const out = await mon.calibrate('all');
      assert.deepEqual(out.map((o) => o.result), ['saved', 'saved']);
      for (const o of out) {
        const bare = dev.laneBareLevel(o.lane);
        assert.ok(Math.abs(o.reference - bare) / bare < 0.02, `lane ${o.lane}: ${o.reference} vs ${bare}`);
        assert.equal(dev.savedReference(o.lane), o.reference);
      }
      const marks = mon.markers(1).filter((m) => m.kind === 'calibration');
      assert.equal(marks.length, 1);
      assert.ok(marks[0]!.tMs >= tCal);
      await t.sleep(600);
      const after = mon.samples(1).filter((s) => s.tMs > tCal + 200);
      assert.ok(after.length > 0 && after.every((s) => s.reference === out[0]!.reference));
      assert.ok(mon.samples(1).some((s) => s.tMs < tCal && s.reference !== out[0]!.reference), 'the reference line steps');
      const r2 = await mon.runSelfCheck();
      assert.equal(findCheck(r2, 'lane.drift', 1)!.level, 'ok');
      assert.equal(findCheck(r2, 'lane.savedReference', 1)!.level, 'ok');
      mon.stop();
    });
  });

  test('SAVED waits for the deferred flash write (10 s after the previous one); reference visible before', async () => {
    const t = rig();
    await t.run(async () => {
      const { api, mon } = await simMonitor(t, 1);
      await mon.calibrate([1]);
      const t0 = t.clock.now();
      const p = mon.calibrate([1]);
      await t.sleep(500);
      const ref = (await api.laneStatus(1)).reference;
      const [o] = await p;
      assert.equal(o!.result, 'saved');
      assert.equal(o!.reference, ref);
      assert.ok(t.clock.now() - t0 > 5000, `SAVED after ${t.clock.now() - t0} ms`);
      mon.stop();
    });
  });

  test('refused during a start sequence (monitor and device), FAILED when the lane leaves while learning', async () => {
    const t = rig();
    await t.run(async () => {
      const { dev, api, mon } = await simMonitor(t, 3);
      await api.startSequence({ lights: 3, stepMs: 1000, holdMinMs: 200, holdMaxMs: 300, attentionPulses: 0 });
      await t.sleep(2100);
      await assert.rejects(mon.calibrate('all'), MonitorBusyError);
      await assert.rejects(api.laneCalibrate(1), (e: unknown) => e instanceof Error && /ERR_SEQ_RUNNING/.test(e.message));
      await t.sleep(4000);
      const p = mon.calibrate([3]);
      await t.sleep(20);
      dev.unplugLane(3);
      const [o] = await p;
      assert.equal(o!.result, 'failed');
      assert.equal(o!.timedOut ?? false, false);
      mon.stop();
    });
  });

  test('FORGET: FORGOTTEN per masked index (absent too); saved references survive a lane power cycle and a reboot, per index', async () => {
    const t = rig();
    await t.run(async () => {
      const { dev, api, mon } = await simMonitor(t, 2);
      const [a] = await mon.calibrate([1]);
      dev.setLaneDirt(1, 0.8);                                        // a re-learn would now find 80 %
      await api.lanePower(false);
      await t.sleep(100);
      await api.lanePower(true);
      await t.sleep(500);
      assert.equal((await api.laneStatus(1)).reference, a!.reference);       // seeded from flash, no learn
      dev.setLaneDirt(1, 1);
      assert.notEqual((await api.laneStatus(2)).reference, a!.reference);
      const out = await mon.forget([1, 5]);
      assert.deepEqual(out.map((o) => [o.lane, o.result, o.reference]), [[1, 'forgotten', 0], [5, 'forgotten', 0]]);
      assert.equal(dev.savedReference(1), 0);
      await t.sleep(200);
      assert.ok((await api.laneStatus(1)).reference > 0, 'the lane re-learned as at boot');
      mon.stop();
    });
  });

  test('wired to a DeviceManager: attach() follows its events and connection; the race gate is busy()', async () => {
    const t = rig();
    await t.run(async () => {
      const dev = t.sim({ lanes: 2 });
      const manager = new DeviceManager({
        clientId: new Uint8Array(16).fill(9), clientName: 'mon', clientKind: ClientKind.WEBAPP, scheduler: t.clock, requestTimeoutMs: 500,
        sources: [{ kind: 'usb', find: async () => dev.transport('usb') }],
      });
      let racing = false;
      const mon = HealthMonitor.attach(manager, { scheduler: t.clock, busy: () => racing });
      manager.start();
      await t.until(() => manager.state === 'connected');
      await manager.api.lanePower(true);
      await t.sleep(3000);
      const r = mon.report();
      assert.equal(r.control.online, true);
      assert.deepEqual(r.lanes.map((l) => l.online), [true, true, false, false, false, false]);
      assert.ok(mon.samples(2).length >= 8);
      racing = true;
      await assert.rejects(mon.calibrate('all'), MonitorBusyError);
      await manager.stop();
      await t.sleep(10);
      assert.equal(mon.report().control.online, false);
      mon.stop();
    });
  });
});
