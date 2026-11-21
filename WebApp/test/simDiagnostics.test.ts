/*
 * Simulator 0.6 diagnostics: the lane signal and LANE_STATUS window
 * semantics, the new STATUS fields, and every fault knob → the check it
 * must trip (through HealthMonitor.runSelfCheck, i.e. the whole path).
 */

import assert from 'node:assert/strict';
import { describe, test } from 'node:test';

import { DeviceApi } from '../src/device/deviceApi.ts';
import { DeviceSession } from '../src/device/deviceSession.ts';
import { LaneStatusFlag } from '../src/device/laneFlags.ts';
import type { SimDevice } from '../src/device/simDevice.ts';
import { findCheck } from '../src/health/health.ts';
import type { HealthReport } from '../src/health/health.ts';
import { HealthMonitor } from '../src/health/monitor.ts';
import { BleBridgeState, BleLinkState, CalibrateAction, ClientKind, LaneState, NodeState, Status } from '../src/protocol/generated.ts';
import { named, recorder, rig } from './helpers.ts';
import type { Recorder, Rig } from './helpers.ts';

async function setup(t: Rig, lanes = 2): Promise<{ dev: SimDevice; api: DeviceApi; mon: HealthMonitor; rec: Recorder }> {
  const dev = t.sim({ lanes });
  let mon: HealthMonitor | null = null;
  const rec = recorder();
  const s = t.session(dev, { ...rec, onEvent: (e) => { rec.events.push(e); mon?.onEvent(e); } });
  await s.connect();
  const api = new DeviceApi(s);
  mon = new HealthMonitor({ api, scheduler: t.clock });
  mon.setConnected(true);
  mon.start();
  await api.lanePower(true);
  await t.until(() => named(rec, 'EVT_LANE_STATE').filter((e) => e.value?.['state'] === LaneState.CONFIGURED).length >= lanes);
  await t.sleep(1500);
  return { dev, api, mon, rec };
}

const lvl = (r: HealthReport, id: string, lane?: number): string => findCheck(r, id, lane)!.level;

describe('sim: lane signal and LANE_STATUS (0.6)', () => {
  test('a healthy lane: flags, baseline ≈ reference ≈ bare level, thresholds below, noise small', async () => {
    const t = rig();
    await t.run(async () => {
      const { dev, api, mon } = await setup(t);
      const s = await api.laneStatus(1);
      const want = LaneStatusFlag.SYNC_LOCKED | LaneStatusFlag.SYNC_ALIGNED | LaneStatusFlag.SENSOR_OK | LaneStatusFlag.CONFIG_VALID | LaneStatusFlag.CALIBRATED;
      assert.equal(s.flags, want);
      assert.ok(s.baseline > 2500 && s.baseline < 4000, `${s.baseline}`);
      assert.ok(Math.abs(s.reference - dev.laneBareLevel(1)) < 30);
      assert.ok(s.thrLow < s.thrHigh && s.thrHigh < s.baseline);
      assert.ok(s.rawMin <= s.raw && s.raw <= s.rawMax);
      assert.ok(s.noise > 0 && s.noise < 20);
      mon.stop();
    });
  });

  test('rawMin/rawMax: the window since the previous read; a read resets it; a slow poll still sees a passing car', async () => {
    const t = rig();
    await t.run(async () => {
      const { dev, api, mon } = await setup(t, 1);
      mon.stop();                                                   // this test is the only reader
      await api.laneStatus(1);
      const quick = await api.laneStatus(1);                        // window of a few ms
      assert.ok(quick.rawMax - quick.rawMin < 6 * quick.noise + 2, `${quick.rawMin}..${quick.rawMax}`);
      dev.carsPractice([1]);
      let sawCar = false;
      let rawBelow = 0;
      for (let i = 0; i < 30; i++) {
        await t.sleep(2000);
        const s = await api.laneStatus(1);
        if (s.rawMin < s.thrLow) sawCar = true;
        if (s.raw < s.thrLow) rawBelow++;
      }
      dev.carsStop();
      assert.ok(sawCar, 'rawMin caught a dip between polls');
      assert.ok(rawBelow < 5, 'the instantaneous raw rarely does');
    });
  });

  test('the signal does not consume the RNG: polling LANE_STATUS does not change what cars do', async () => {
    const run = async (poll: boolean): Promise<number[]> => {
      const t = rig();
      let out: number[] = [];
      await t.run(async () => {
        const dev = t.sim({ lanes: 1 });
        const s = t.session(dev);
        await s.connect();
        const api = new DeviceApi(s);
        await api.lanePower(true);
        await t.sleep(300);
        dev.carsPractice([1]);
        for (let i = 0; i < 40; i++) { await t.sleep(250); if (poll) await api.laneStatus(1); }
        out = dev.truth().map((c) => c.tEntryUs);
        dev.carsStop();
      });
      return out;
    };
    assert.deepEqual(await run(true), await run(false));
  });

  test('LANE_CALIBRATE: action checked; LEARN only configured lanes, FORGET never ERR_LANE_ABSENT', async () => {
    const t = rig();
    await t.run(async () => {
      const { api, rec, mon } = await setup(t, 2);
      mon.stop();
      await assert.rejects(api.laneCalibrate(1, 2 as CalibrateAction), /ERR_INVALID_ARG/);
      await assert.rejects(api.laneCalibrate(0b110000, CalibrateAction.LEARN), /ERR_LANE_ABSENT/);
      await api.laneCalibrate(0b110000, CalibrateAction.FORGET);
      await t.until(() => named(rec, 'EVT_LANE_CALIBRATED').length === 2);
      assert.deepEqual(named(rec, 'EVT_LANE_CALIBRATED').map((e) => e.value?.['lane']), [5, 6]);
    });
  });
});

describe('sim: STATUS 0.6 fields', () => {
  test('defaults: bridge OK; RSSI 127 without a phone; with a phone the knob value after the first STATS', async () => {
    const t = rig();
    await t.run(async () => {
      const dev = t.sim();
      const s = t.session(dev);
      await s.connect();
      const api = new DeviceApi(s);
      let st = await api.status();
      assert.deepEqual([st.bleBridge, st.bleRssi, st.bleCrcErrors, st.settingsDefaults], [BleBridgeState.OK, 127, 0, 0]);
      const ble = t.session(dev, {}, { kind: 'ble', clientKind: ClientKind.TOOL });
      void ble.connect().catch(() => undefined);
      await t.sleep(1200);
      dev.setBleRssi(-71);
      dev.setBleStats({ bleCrcErrors: 70000, nrfLinkCrcErrors: 3 });
      st = await api.status();
      assert.deepEqual([st.bleLink, st.bleRssi, st.bleCrcErrors, st.nrfLinkCrcErrors], [BleLinkState.UP, -71, 65535, 3]);
    });
  });
});

describe('sim fault knobs → health checks', () => {
  test('nRF silent: ABSENT after 3 s, phone gone, EVT_BLE_LINK DOWN, BLE node ABSENT; alive again → OK', async () => {
    const t = rig();
    await t.run(async () => {
      const { dev, api, mon, rec } = await setup(t, 1);
      const ble = new DeviceSession(dev.transport('ble'), {
        clientId: new Uint8Array(16).fill(7), clientName: 'p', clientKind: ClientKind.TOOL, scheduler: t.clock, requestTimeoutMs: 500,
      });
      void ble.connect().catch(() => undefined);
      await t.sleep(300);
      let r = await mon.runSelfCheck();
      assert.deepEqual([r.ble.online, r.ble.connected], [true, true]);
      dev.nrfAlive(false);
      await t.sleep(1000);
      r = await mon.runSelfCheck();
      assert.deepEqual([r.ble.online, r.ble.connected], [true, false]);         // not declared absent yet
      await t.sleep(2500);
      r = await mon.runSelfCheck();
      assert.deepEqual([r.ble.online, lvl(r, 'ble.bridge')], [false, 'fail']);
      assert.equal(named(rec, 'EVT_BLE_LINK').at(-1)!.value?.['state'], BleLinkState.DOWN);
      assert.equal(mon.nodes.ble?.state, NodeState.ABSENT);   // listed as ABSENT, like host_manager
      assert.throws(() => dev.attach('ble', { onBytes: () => undefined, onClose: () => undefined }), /advertising/);
      dev.nrfAlive(true);
      r = await mon.runSelfCheck();
      assert.equal(lvl(r, 'ble.bridge'), 'ok');
      await ble.close();
      mon.stop();
    });
  });

  test('version mismatch → ble.version fail; RSSI / counters → ble checks', async () => {
    const t = rig();
    await t.run(async () => {
      const { dev, mon } = await setup(t, 1);
      dev.setBleStats({ bleUartErrors: 60, bleDataDropped: 2 });
      let r = await mon.runSelfCheck();
      assert.deepEqual([lvl(r, 'ble.uart'), lvl(r, 'ble.dropped'), lvl(r, 'ble.rssi')], ['fail', 'warn', 'unknown']);
      dev.nrfVersionMismatch(true);
      r = await mon.runSelfCheck();
      assert.equal(lvl(r, 'ble.version'), 'fail');
      mon.stop();
    });
  });

  test('noisy sensor → lane.noise warn / fail', async () => {
    const t = rig();
    await t.run(async () => {
      const { dev, mon } = await setup(t, 2);
      dev.setLaneNoise(1, 30);                                       // 7 × 30 = 210 of a ~920 margin
      dev.setLaneNoise(2, 60);                                       // 8 × 60 = 480 of a ~950 margin
      const r = await mon.runSelfCheck();
      assert.deepEqual([lvl(r, 'lane.noise', 1), lvl(r, 'lane.noise', 2)], ['warn', 'fail']);
      mon.stop();
    });
  });

  test('saturated sensor → lane.signal fail (lower the IR current); the dip of a car is invisible', async () => {
    const t = rig();
    await t.run(async () => {
      const { dev, mon } = await setup(t, 1);
      dev.setLaneSaturated(1, true);
      await t.sleep(500);
      const r = await mon.runSelfCheck();
      assert.equal(lvl(r, 'lane.signal', 1), 'fail');
      assert.equal(mon.lastLaneStatus(1)!.rawMax, 65535);
      mon.stop();
    });
  });

  test('broken sensor → lane.fault + lane.sensor fail, raw 0; repaired → CONFIGURED, ok again; LEARN fails meanwhile', async () => {
    const t = rig();
    await t.run(async () => {
      const { dev, mon } = await setup(t, 2);
      dev.sensorFault(2, true);
      await t.sleep(100);
      let r = await mon.runSelfCheck();
      assert.deepEqual([lvl(r, 'lane.fault', 2), lvl(r, 'lane.sensor', 2)], ['fail', 'fail']);
      assert.equal(mon.lastLaneStatus(2)!.raw, 0);
      assert.equal(mon.nodes.lanes[2]!.phase, 'fault');
      const out = await mon.calibrate([2]);
      assert.equal(out[0]!.result, 'failed');
      dev.sensorFault(2, false);
      await t.sleep(100);
      assert.equal(mon.nodes.lanes[2]!.phase, 'configured');
      r = await mon.runSelfCheck();
      assert.deepEqual([lvl(r, 'lane.fault', 2), lvl(r, 'lane.sensor', 2)], ['ok', 'ok']);
      assert.equal(lvl(r, 'ctrl.errors'), 'warn');                    // the fault counted
      mon.stop();
    });
  });

  test('lane fault + recovery hook; lane left → lane.present fail', async () => {
    const t = rig();
    await t.run(async () => {
      const { dev, mon } = await setup(t, 3);
      dev.laneFault(1, Status.ERR_LANE_NACK);
      await t.sleep(50);
      assert.equal(lvl(mon.report(), 'lane.fault', 1), 'fail');
      dev.laneRecover(1);
      await t.sleep(50);
      assert.equal(lvl(mon.report(), 'lane.fault', 1), 'ok');
      dev.unplugLane(3);
      await t.sleep(200);
      const r = await mon.runSelfCheck();
      assert.deepEqual([r.lanes[2]!.online, lvl(r, 'lane.present', 3)], [false, 'fail']);
      assert.ok(mon.markers(3).some((m) => m.kind === 'left'));
      mon.stop();
    });
  });

  test('flash writes failing → NOT_SAVED (lane.savedReference warn); loop time; corrupt settings at boot', async () => {
    const t = rig();
    await t.run(async () => {
      const { dev, api, mon } = await setup(t, 1);
      dev.failFlashWrites(true);
      const [o] = await mon.calibrate([1]);
      assert.equal(o!.result, 'notSaved');
      assert.ok(o!.reference > 0);
      assert.equal(dev.savedReference(1), 0);
      // The control module keeps the learned value (and retries the write): LANE_STATUS says so.
      const ls = await api.laneStatus(1);
      assert.deepEqual([ls.reference, ls.savedReference], [o!.reference, o!.reference]);
      dev.setLoopMaxMs(12);
      let r = await mon.runSelfCheck();
      assert.deepEqual([lvl(r, 'lane.savedReference', 1), lvl(r, 'ctrl.loopTime')], ['warn', 'fail']);
      dev.corruptSettings();
      dev.powerCycle();
      mon.setConnected(false);
      mon.stop();
      await t.sleep(500);                                            // USB re-enumerates after a boot

      const s = t.session(dev);
      await s.connect();
      const st = await new DeviceApi(s).status();
      assert.equal(st.settingsDefaults, 1);
      const m2 = new HealthMonitor({ api: new DeviceApi(s), scheduler: t.clock });
      m2.setConnected(true);
      r = await m2.runSelfCheck();
      assert.equal(lvl(r, 'ctrl.settings'), 'warn');
      assert.equal(lvl(r, 'ctrl.lanePower'), 'warn');
      m2.stop();
    });
  });
});
