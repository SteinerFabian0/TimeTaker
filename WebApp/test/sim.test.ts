/*
 * SimDevice behaviour through DeviceApi: the device side of CONTROL guide §5
 * as the simulator implements it (its file header lists every rule). These
 * cases are what the firmware's behaviour gets compared against.
 */

import assert from 'node:assert/strict';
import { describe, test } from 'node:test';

import { DeviceApi, laneMask, ALL_LANES } from '../src/device/deviceApi.ts';
import { DeviceError } from '../src/device/deviceSession.ts';
import { defaultLaneConfig } from '../src/device/simDevice.ts';
import type { SimDevice } from '../src/device/simDevice.ts';
import {
  BindChange, BoardType, ButtonId, ClientKind, LaneBusState, LaneEventFlag, LaneEventType, LaneState, LedMode,
  MsgId, NodeClass, PressKind, RebootTarget, SamplingMode, SeqAbortReason, Status,
} from '../src/protocol/generated.ts';
import { named, recorder, rig } from './helpers.ts';
import type { Recorder, Rig } from './helpers.ts';

const status = (code: number) => (e: unknown): boolean => e instanceof DeviceError && e.status === code;

async function connected(t: Rig, o: Parameters<Rig['sim']>[0] = {}): Promise<{ dev: SimDevice; api: DeviceApi; rec: Recorder }> {
  const dev = t.sim(o);
  const rec = recorder();
  const s = t.session(dev, rec);
  await s.connect();
  return { dev, api: new DeviceApi(s), rec };
}

const laneStates = (rec: Recorder): string[] => named(rec, 'EVT_LANE_STATE').map((e) => {
  const st = Object.entries(LaneState).find(([, v]) => v === e.value?.['state'])?.[0] ?? '?';
  return `${String(e.value?.['lane'])}${st[0]}`;
});

async function powered(t: Rig, rec: Recorder, api: DeviceApi, n = 6): Promise<void> {
  await api.lanePower(true);
  await t.until(() => named(rec, 'EVT_LANE_STATE').filter((e) => e.value?.['state'] === LaneState.CONFIGURED).length >= n);
}

describe('lane chain', () => {
  test('power on: JOINED then CONFIGURED, in chain order; power off: LEFT for all', async () => {
    const t = rig();
    await t.run(async () => {
      const { api, rec } = await connected(t, { lanes: 4 });
      assert.equal(await api.lanePower(true), false);            // still settling
      await t.sleep(500);
      assert.deepEqual(laneStates(rec), ['1J', '1C', '2J', '2C', '3J', '3C', '4J', '4C']);
      const nodes = await api.nodeList();
      assert.deepEqual(nodes.filter((n) => n.nodeClass === NodeClass.LANE).map((n) => n.index), [1, 2, 3, 4]);
      assert.equal((await api.status()).lanesPresent, 4);
      await api.lanePower(false);
      await t.sleep(10);
      assert.deepEqual(laneStates(rec).slice(8), ['1L', '2L', '3L', '4L']);
    });
  });

  test('a broken ribbon at lane 3: 3..6 LEFT, replug: they re-join in order', async () => {
    const t = rig();
    await t.run(async () => {
      const { dev, api, rec } = await connected(t);
      await powered(t, rec, api);
      rec.events.length = 0;
      dev.unplugLane(3);
      await t.sleep(100);
      assert.deepEqual(laneStates(rec), ['3L', '4L', '5L', '6L']);
      assert.equal((await api.status()).lanesPresent, 2);
      dev.replugLane(3);
      await t.sleep(300);
      assert.deepEqual(laneStates(rec).slice(4), ['3J', '3C', '4J', '4C', '5J', '5C', '6J', '6C']);
    });
  });

  test('a lane reset comes back configured and sampling, laneSeq from 0', async () => {
    const t = rig();
    await t.run(async () => {
      const { dev, api, rec } = await connected(t);
      await powered(t, rec, api);
      await api.laneSampling(laneMask([2]), SamplingMode.RAW);
      rec.events.length = 0;
      dev.resetLane(2);
      await t.sleep(400);
      assert.deepEqual(laneStates(rec).slice(0, 5), ['2L', '3L', '4L', '5L', '6L']);
      assert.ok(laneStates(rec).includes('2C') && laneStates(rec).includes('6C'));
      assert.equal(dev.laneSamplingMode(2), SamplingMode.RAW);
      const st = await api.laneStatus(2);
      assert.equal(st.state, LaneBusState.SAMPLING);
    });
  });

  test('LANE_SCAN: needs power; LEFT for all, then the chain again', async () => {
    const t = rig();
    await t.run(async () => {
      const { api, rec } = await connected(t, { lanes: 2 });
      await assert.rejects(api.laneScan(), status(Status.ERR_INVALID_STATE));
      await powered(t, rec, api, 2);
      rec.events.length = 0;
      await api.laneScan();
      await t.sleep(300);
      assert.deepEqual(laneStates(rec), ['1L', '2L', '1J', '1C', '2J', '2C']);
    });
  });
});

describe('lane requests', () => {
  test('config table: defaults, set with mask, persisted only with save', async () => {
    const t = rig();
    await t.run(async () => {
      const { dev, api } = await connected(t);
      assert.deepEqual(await api.laneConfigGet(3), defaultLaneConfig(3));
      const cfg = { ...defaultLaneConfig(1), holdoffUs: 2_000_000 };
      await api.laneConfigSet(laneMask([1, 2]), cfg, false);
      await api.laneConfigSet(laneMask([3]), cfg, true);
      assert.equal((await api.laneConfigGet(2)).holdoffUs, 2_000_000);
      await assert.rejects(api.laneConfigSet(0, cfg), status(Status.ERR_INVALID_ARG));
      await assert.rejects(api.laneConfigSet(0x40, cfg), status(Status.ERR_INVALID_ARG));
      await assert.rejects(api.laneConfigSet(1, { ...cfg, samplePeriodUs: 0 }), status(Status.ERR_INVALID_ARG));
      await assert.rejects(api.laneConfigGet(7), status(Status.ERR_INVALID_ARG));
      dev.powerCycle();
      await t.sleep(500);
      const { api: api2 } = await (async () => { const s = t.session(dev); await s.connect(); return { api: new DeviceApi(s) }; })();
      assert.equal((await api2.laneConfigGet(2)).holdoffUs, 0);         // RAM only
      assert.equal((await api2.laneConfigGet(3)).holdoffUs, 2_000_000); // saved
    });
  });

  test('LED and calibrate act on present lanes only; status and raw need a present lane', async () => {
    const t = rig();
    await t.run(async () => {
      const { dev, api, rec } = await connected(t, { lanes: 2 });
      await assert.rejects(api.laneLed(1, { mode: LedMode.ON }), status(Status.ERR_LANE_ABSENT));
      await assert.rejects(api.laneStatus(1), status(Status.ERR_LANE_ABSENT));
      await powered(t, rec, api, 2);
      await api.laneLed(ALL_LANES, { mode: LedMode.BLINK, blinkPeriodMs: 500, blinkOnPermille: 500, blinkCount: 3 });
      assert.equal(dev.laneLed(2).mode, LedMode.BLINK);
      await assert.rejects(api.laneLed(1, { mode: LedMode.BLINK }), status(Status.ERR_INVALID_ARG));   // period 0
      await assert.rejects(api.laneLed(1, { mode: LedMode.ON, level: 1001 }), status(Status.ERR_INVALID_ARG));
      await api.laneCalibrate(ALL_LANES);
      await assert.rejects(api.laneCalibrate(laneMask([5])), status(Status.ERR_LANE_ABSENT));
      await assert.rejects(api.laneRawStream(4, 1), status(Status.ERR_LANE_ABSENT));
      const st = await api.laneStatus(1);
      assert.equal(st.samplingMode, SamplingMode.ON);
      assert.ok(st.thrLow < st.baseline && st.thrHigh > st.thrLow);
    });
  });

  test('sampling OFF hides crossings; raw stream: chunks with counting chunkSeq, mode restored', async () => {
    const t = rig();
    await t.run(async () => {
      const { dev, api, rec } = await connected(t, { lanes: 2 });
      await powered(t, rec, api, 2);
      await api.laneSampling(laneMask([2]), SamplingMode.OFF);
      dev.carsPractice([1, 2]);
      await t.sleep(20000);
      const lanes = new Set(named(rec, 'EVT_CROSSING').map((e) => e.value?.['lane']));
      assert.deepEqual([...lanes], [1]);
      assert.ok(dev.truth().some((c) => c.lane === 2 && !c.detected));

      await api.laneRawStream(1, 2);
      assert.equal(dev.laneSamplingMode(1), SamplingMode.RAW);
      await t.sleep(400);
      const raw = rec.streams.filter((s) => s.name === 'STR_RAW_SAMPLES');
      assert.ok(raw.length >= 6);
      assert.deepEqual(raw.map((r) => r.value?.['chunkSeq']), raw.map((_, i) => i));
      assert.equal(raw[0]?.value?.['periodUs'], 2000);
      await api.laneRawStream(0);
      assert.equal(dev.laneSamplingMode(1), SamplingMode.ON);
    });
  });

  test('crossings: holdoff flag, blocked/cleared, lane sequence', async () => {
    const t = rig();
    await t.run(async () => {
      const { dev, api, rec } = await connected(t, { lanes: 1 });
      await powered(t, rec, api, 1);
      await api.laneConfigSet(1, { ...defaultLaneConfig(1), holdoffUs: 4_000_000 });
      dev.setCarScript(1, { incidents: [{ afterCrossing: 2, kind: 'double' }, { afterCrossing: 4, kind: 'block' }] });
      dev.carsPractice([1]);
      await t.sleep(40000);
      const ev = named(rec, 'EVT_CROSSING').map((e) => e.value!);
      assert.deepEqual(ev.map((v) => v['laneSeq']), ev.map((_, i) => i));
      const holdoff = ev.filter((v) => ((v['flags'] as number) & LaneEventFlag.IN_HOLDOFF) !== 0);
      assert.equal(holdoff.length, 1);                                  // the double crossing
      assert.ok(ev.some((v) => v['type'] === LaneEventType.BLOCKED));
      assert.ok(ev.some((v) => v['type'] === LaneEventType.CLEARED));
      const truth = dev.truth().filter((c) => c.kind === 'double' || c.kind === 'blocked').map((c) => c.kind);
      assert.deepEqual(truth, ['double', 'blocked']);
    });
  });
});

describe('start sequence', () => {
  test('SEQ_STARTED now, LIGHTS_OUT at Tout within the hold range, never earlier', async () => {
    const t = rig();
    await t.run(async () => {
      const { dev, api, rec } = await connected(t, { lanes: 4 });
      await powered(t, rec, api, 4);
      for (let i = 0; i < 5; i++) {
        const t0 = await api.startSequence({ lights: 6, stepMs: 500, holdMinMs: 200, holdMaxMs: 1500 });
        await t.until(() => named(rec, 'EVT_SEQ_STARTED').length === i + 1);
        const started = named(rec, 'EVT_SEQ_STARTED').at(-1)!.value!;
        assert.equal(started['t0Us'], t0);
        assert.equal(started['lights'], 4);                           // n = lanes present
        assert.ok(Number(t0) - Number(dev.sysUs()) >= 140_000);
        await t.sleep(1500);
        assert.equal(dev.laneLed(1).mode, LedMode.ON);
        await assert.rejects(api.startSequence({ lights: 6, stepMs: 500, holdMinMs: 200, holdMaxMs: 1500 }), status(Status.ERR_SEQ_RUNNING));
        await assert.rejects(api.laneLed(1, { mode: LedMode.OFF }), status(Status.ERR_SEQ_RUNNING));
        await t.until(() => named(rec, 'EVT_LIGHTS_OUT').length === i + 1, 5000);
        const tout = Number(named(rec, 'EVT_LIGHTS_OUT').at(-1)!.value!['toutUs']);
        const hold = tout - Number(t0) - 4 * 500_000;
        assert.ok(hold >= 200_000 && hold <= 1_500_000, `hold ${hold}`);
        assert.ok(Number(dev.sysUs()) >= tout);
        assert.equal(dev.laneLed(1).mode, LedMode.OFF);
      }
    });
  });

  test('argument checks, abort by host and by BTN2, power off aborts', async () => {
    const t = rig();
    await t.run(async () => {
      const { dev, api, rec } = await connected(t, { lanes: 2 });
      const p = { lights: 6, stepMs: 300, holdMinMs: 100, holdMaxMs: 200 };
      await assert.rejects(api.startSequence(p), status(Status.ERR_LANE_ABSENT));
      await powered(t, rec, api, 2);
      await assert.rejects(api.startSequence({ ...p, lights: 0 }), status(Status.ERR_INVALID_ARG));
      await assert.rejects(api.startSequence({ ...p, holdMinMs: 300 }), status(Status.ERR_INVALID_ARG));
      await assert.rejects(api.startSequence({ ...p, leadMs: 100 }), status(Status.ERR_TOO_LATE));
      await assert.rejects(api.abortSequence(), status(Status.ERR_INVALID_STATE));
      dev.failLaneSchedule(2);
      await assert.rejects(api.startSequence(p), status(Status.ERR_LANE_NACK));

      await api.startSequence(p);
      await api.abortSequence();
      dev.pressButton(ButtonId.BTN2, PressKind.SHORT);
      await api.startSequence(p);
      dev.pressButton(ButtonId.BTN2, PressKind.LONG);
      await api.startSequence(p);
      await api.lanePower(false);
      await t.sleep(50);
      const reasons = named(rec, 'EVT_SEQ_ABORTED').map((e) => e.value?.['reason']);
      assert.deepEqual(reasons, [SeqAbortReason.HOST, SeqAbortReason.BUTTON, SeqAbortReason.POWER]);
      assert.equal(named(rec, 'EVT_LIGHTS_OUT').length, 0);
    });
  });
});

describe('binding and settings', () => {
  test('USB HELLO of a web app binds it; BIND_ALLOW/REMOVE/LIST; LRU eviction at 8', async () => {
    const t = rig();
    await t.run(async () => {
      const dev = t.sim();
      const rec = recorder();
      const s = t.session(dev, rec, { clientKind: ClientKind.WEBAPP, clientName: 'Laptop' });
      await s.connect();
      const api = new DeviceApi(s);
      let list = await api.bindList();
      assert.equal(list.entries.length, 1);
      assert.equal(list.entries[0]?.clientName, 'Laptop');
      await api.bindAllow(30);
      assert.equal((await api.bindList()).windowS, 30);
      await api.bindAllow(0);
      for (let i = 1; i <= 8; i++) {                                // 8 more web apps over USB
        const o = t.session(dev, {}, { clientKind: ClientKind.WEBAPP, clientId: new Uint8Array(16).fill(0x10 + i), clientName: `c${i}` });
        await s.close();
        await o.connect();
        await o.close();
      }
      const s2 = t.session(dev, rec, { clientKind: ClientKind.TOOL });
      await s2.connect();
      const api2 = new DeviceApi(s2);
      list = await api2.bindList();
      assert.equal(list.entries.length, 8);
      assert.ok(!list.entries.some((e) => e.clientName === 'Laptop'));    // the oldest went
      await api2.bindRemove(new Uint8Array(16).fill(0x11));
      await api2.bindRemove(new Uint8Array(16).fill(0x77));               // unknown: OK, no event
      assert.equal((await api2.bindList()).entries.length, 7);
      await api2.bindClear();
      assert.equal((await api2.bindList()).entries.length, 0);
      const changes = dev.eventLog().filter((e) => e.name === 'EVT_BIND_CHANGED').map((e) => e.value['change']);
      assert.deepEqual(changes.slice(0, 3), [BindChange.ADDED, BindChange.WINDOW_OPENED, BindChange.WINDOW_CLOSED]);
      assert.equal(changes.filter((c) => c === BindChange.REMOVED).length, 2);
      assert.equal(changes.at(-1), BindChange.CLEARED);
    });
  });

  test('settings round-trip, deferred flash writes, BLE disabled refuses BLE', async () => {
    const t = rig();
    await t.run(async () => {
      const { dev, api } = await connected(t);
      assert.equal((await api.settingsGet()).deviceName, 'TimeTaker');
      await api.settingsSet({ deviceName: 'TimeTaker Opa', bleEnabled: false });
      await api.settingsSet({ deviceName: 'TimeTaker Opa', bleEnabled: false });
      assert.equal((await api.settingsGet()).deviceName, 'TimeTaker Opa');
      await t.sleep(100);
      assert.equal(dev.settingsWrites, 1);
      await api.settingsSet({ deviceName: 'TimeTaker Oma', bleEnabled: false });
      await t.sleep(100);
      assert.equal(dev.settingsWrites, 1);                            // at most every 10 s
      await t.sleep(10000);
      assert.equal(dev.settingsWrites, 2);
      await assert.rejects(t.session(dev, {}, { kind: 'ble' }).connect(), /advertising/);
    });
  });
});

describe('no bootloader (protocol 0.4)', () => {
  test('REBOOT{BOOTLOADER} and the old update ids are unsupported; CTRL reports CONTROL, imageCrc 0', async () => {
    const t = rig();
    await t.run(async () => {
      const dev = t.sim();
      const s = t.session(dev);
      await s.connect();
      const api = new DeviceApi(s);
      assert.equal(s.boardType, BoardType.CONTROL);
      const ctrl = (await api.nodeList())[0];
      assert.equal(ctrl?.boardType, BoardType.CONTROL);
      assert.equal(ctrl?.imageCrc, 0);
      const bootBefore = dev.bootId;
      assert.equal((await s.requestRaw(MsgId.REBOOT, new Uint8Array([RebootTarget.BOOTLOADER]))).status, Status.ERR_UNSUPPORTED);
      for (const id of [0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xaf]) {
        assert.equal((await s.requestRaw(id, new Uint8Array(0))).status, Status.ERR_UNSUPPORTED, `id 0x${id.toString(16)}`);
      }
      await t.sleep(600);
      assert.equal(dev.bootId, bootBefore);                               // nothing rebooted
    });
  });
});
