/*
 * The device layer above the session: lane masks, clientId storage, the node
 * model reducer, and DeviceManager's transport preference and reconnects
 * (SYSTEM_ARCHITECTURE §7.4 "pulling the USB cable mid-race" — required test).
 */

import assert from 'node:assert/strict';
import { describe, test } from 'node:test';

import { DeviceApi, laneMask, lanesOf } from '../src/device/deviceApi.ts';
import { CLIENT_ID_KEY, loadClientId, memoryStorage, truncateUtf8 } from '../src/device/clientId.ts';
import { DeviceManager } from '../src/device/deviceManager.ts';
import type { ManagerState } from '../src/device/deviceManager.ts';
import { DeviceError, TimeoutError } from '../src/device/deviceSession.ts';
import type { DeviceEvent } from '../src/device/deviceSession.ts';
import { SerialTransport } from '../src/transport/serialTransport.ts';
import { applyNodeEvent, applyNodeList, emptyNodeModel, presentLanes, readyLanes } from '../src/device/nodeModel.ts';
import type { SimDevice } from '../src/device/simDevice.ts';
import {
  BoardType, ClientKind, LaneState, MsgId, Status, NodeClass, NodeState, PressKind,
} from '../src/protocol/generated.ts';
import { rig } from './helpers.ts';
import type { Rig } from './helpers.ts';

describe('lane masks', () => {
  test('lanes ↔ mask', () => {
    assert.equal(laneMask([1, 3, 6]), 0b100101);
    assert.deepEqual(lanesOf(0b100101), [1, 3, 6]);
    assert.throws(() => laneMask([7]), RangeError);
  });
});

describe('clientId', () => {
  test('created once, kept, never all-zero', () => {
    const st = memoryStorage();
    let calls = 0;
    const rnd = (n: number): Uint8Array => (calls++ === 0 ? new Uint8Array(n) : new Uint8Array(n).fill(5));
    const a = loadClientId(st, rnd);
    assert.deepEqual([...a], new Array(16).fill(5));
    assert.deepEqual(loadClientId(st, () => new Uint8Array(16).fill(9)), a);
    assert.equal(st.getItem(CLIENT_ID_KEY), '05'.repeat(16));
  });

  test('names are cut at 24 UTF-8 bytes without splitting a character', () => {
    assert.equal(truncateUtf8('Laptop Großonkel Wilhelm XYZ', 24), 'Laptop Großonkel Wilhel');   // ß is 2 bytes
    assert.equal(new TextEncoder().encode(truncateUtf8('ääääääääääääää', 24)).length, 24);
  });
});

const ev = (msgId: number, value: Record<string, unknown>, evSeq = 1): DeviceEvent =>
  ({ bootId: 7, evSeq, msgId, name: '', value });

describe('node model', () => {
  test('lane states, NODE_LIST versions, boot resets the lanes', () => {
    let m = emptyNodeModel();
    m = applyNodeEvent(m, ev(MsgId.EVT_LANE_STATE, { lane: 1, state: LaneState.JOINED, status: 0, sysUs: 10n }));
    m = applyNodeEvent(m, ev(MsgId.EVT_LANE_STATE, { lane: 1, state: LaneState.CONFIGURED, status: 0, sysUs: 11n }));
    m = applyNodeEvent(m, ev(MsgId.EVT_LANE_STATE, { lane: 2, state: LaneState.JOINED, status: 0, sysUs: 12n }));
    assert.deepEqual(presentLanes(m), [1, 2]);
    assert.deepEqual(readyLanes(m), [1]);
    m = applyNodeEvent(m, ev(MsgId.EVT_LANE_STATE, { lane: 1, state: LaneState.SYNC_LOST, status: 0, sysUs: 13n }));
    assert.deepEqual(readyLanes(m), []);
    m = applyNodeList(m, [
      { nodeClass: NodeClass.CTRL, index: 0, boardType: BoardType.CONTROL, state: NodeState.OK, fwMajor: 0, fwMinor: 2, fwPatch: 0, protoMinor: 2, fwGitRev: 1, imageCrc: 0 },
      { nodeClass: NodeClass.LANE, index: 1, boardType: BoardType.LANE, state: NodeState.FAULT, fwMajor: 1, fwMinor: 0, fwPatch: 0, protoMinor: 0, fwGitRev: 2, imageCrc: 0 },
    ]);
    assert.equal(m.lanes[1]?.phase, 'fault');
    assert.equal(m.lanes[1]?.info?.fwMajor, 1);
    assert.equal(m.lanes[2]?.phase, 'absent');                      // not listed any more
    m = applyNodeEvent(m, ev(MsgId.EVT_SEQ_STARTED, { lights: 6, armSampling: 1, stepMs: 1000, t0Us: 5n }));
    assert.equal(m.seqRunning, true);
    m = applyNodeEvent(m, ev(MsgId.EVT_BOOT, { resetCause: 1, bootId: 9, sysUs: 0n }));
    assert.deepEqual(presentLanes(m), []);
    assert.equal(m.seqRunning, false);
    assert.equal(m.ctrl?.fwMinor, 2);
  });
});

function manager(t: Rig, dev: SimDevice, events: DeviceEvent[], states: string[]): DeviceManager {
  const m = new DeviceManager({
    clientId: new Uint8Array(16).fill(4), clientName: 'mgr', clientKind: ClientKind.WEBAPP, scheduler: t.clock,
    requestTimeoutMs: 500, reconnectMs: 500, preferCheckMs: 1000,
    sources: [
      { kind: 'ble', find: async () => dev.transport('ble') },
      { kind: 'usb', find: async () => (dev.usbPlugged ? dev.transport('usb') : null) },
    ],
  }, {
    onEvent: (e) => events.push(e),
    onState: (s: ManagerState, k) => states.push(`${s}:${k ?? '-'}`),
  });
  return m;
}

describe('DeviceManager', () => {
  test('USB first; unplug → BLE; replug → back to USB; every event exactly once', async () => {
    const t = rig();
    await t.run(async () => {
      const dev = t.sim();
      const events: DeviceEvent[] = [];
      const states: string[] = [];
      const m = manager(t, dev, events, states);
      m.start();
      await t.until(() => m.state === 'connected');
      assert.equal(m.transportKind, 'usb');                         // the USB HELLO also binds us for BLE
      await t.until(() => m.nodes.ctrl !== null);

      for (let i = 0; i < 5; i++) { dev.pressButton(1, PressKind.SHORT); await t.sleep(300); }
      dev.unplugUsb();
      for (let i = 0; i < 5; i++) { dev.pressButton(2, PressKind.SHORT); await t.sleep(300); }
      await t.until(() => m.state === 'connected' && m.transportKind === 'ble', 5000, 'BLE');
      for (let i = 0; i < 5; i++) { dev.pressButton(3, PressKind.SHORT); await t.sleep(300); }
      dev.plugUsb();
      await t.until(() => m.transportKind === 'usb', 5000, 'back on USB');
      for (let i = 0; i < 5; i++) { dev.pressButton(1, PressKind.LONG); await t.sleep(300); }
      await t.sleep(500);

      assert.equal(events.filter((e) => e.name === 'EVT_BUTTON').length, 20);
      assert.deepEqual(events.map((e) => e.evSeq), events.map((_, i) => i));
      assert.deepEqual(dev.eventLog().map((e) => e.evSeq), events.map((e) => e.evSeq));
      assert.ok(states.includes('connected:ble'));
      await m.stop();
    });
  });

  test('a device reboot: reconnects by itself and reports the new boot', async () => {
    const t = rig();
    await t.run(async () => {
      const dev = t.sim();
      const events: DeviceEvent[] = [];
      const m = manager(t, dev, events, []);
      const boots: number[] = [];
      m.start();
      await t.until(() => m.state === 'connected');
      const first = dev.bootId;
      await m.api.reboot('app');
      await t.until(() => dev.bootId !== first);
      await t.until(() => events.some((e) => e.bootId === dev.bootId && e.name === 'EVT_BOOT'), 5000);
      boots.push(...new Set(events.map((e) => e.bootId)));
      assert.deepEqual(boots, [first, dev.bootId]);
      await m.stop();
    });
  });
});

describe('DeviceApi retry policy', () => {
  const fake = (script: (n: number) => Error | null) => {
    const calls: number[] = [];
    const requester = {
      request: (_rq: unknown, _r: unknown, msgId: number): Promise<never> => {
        calls.push(msgId);
        const e = script(calls.length);
        return e ? Promise.reject(e) : Promise.resolve({ status: 0 } as never);
      },
    };
    return { calls, api: new DeviceApi(requester as never, { sleep: async () => undefined }) };
  };

  test('ERR_BUSY (a deferred answer still owed) is retried for an idempotent request', async () => {
    const { calls, api } = fake((n) => (n < 3 ? new DeviceError('LANE_STATUS', Status.ERR_BUSY) : null));
    await api.laneStatus(1);
    assert.equal(calls.length, 3);
  });

  test('UPDATE_END, START_SEQUENCE and REBOOT are never repeated after a timeout', async () => {
    for (const run of [(a: DeviceApi) => a.updateEnd(), (a: DeviceApi) => a.abortSequence(), (a: DeviceApi) => a.reboot()]) {
      const { calls, api } = fake(() => new TimeoutError('X', 1));
      await assert.rejects(run(api), TimeoutError);
      assert.equal(calls.length, 1);
    }
  });
});

describe('SerialTransport', () => {
  const chunkStream = (chunks: (Uint8Array | Error)[]): ReadableStream<Uint8Array> => {
    let i = 0;
    return new ReadableStream<Uint8Array>({
      pull(c) {
        const x = chunks[i++];
        if (x === undefined) return new Promise(() => undefined);
        if (x instanceof Error) c.error(x); else c.enqueue(x);
        return undefined;
      },
    });
  };

  test('a non-fatal read error (overrun) continues on the new readable instead of ending the link', async () => {
    const got: number[] = [];
    let closed = '';
    const port = {
      readable: chunkStream([Uint8Array.of(1), Object.assign(new Error('overrun'), { name: 'BufferOverrunError' })]) as ReadableStream<Uint8Array> | null,
      writable: new WritableStream<Uint8Array>(),
      async open() { /* */ }, async close() { /* */ },
      getInfo: () => ({}),
    };
    const t = new SerialTransport(port as never);
    await t.open({ onBytes: (b) => got.push(...b), onClose: (r) => { closed = r; } });
    port.readable = chunkStream([Uint8Array.of(2)]);   // the browser's replacement stream
    await new Promise((r) => setTimeout(r, 20));
    assert.deepEqual(got, [1, 2]);
    assert.equal(closed, '');
    await t.close();
  });

  test('open() failing after the port opened closes the port again', async () => {
    let closes = 0;
    const port = { readable: null, writable: null, async open() { /* */ }, async close() { closes++; }, getInfo: () => ({}) };
    await assert.rejects(new SerialTransport(port as never).open({ onBytes: () => undefined, onClose: () => undefined }), /no streams/);
    assert.equal(closes, 1);
  });
});
