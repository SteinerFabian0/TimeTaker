/*
 * BleTransport over a fake navigator.bluetooth bridged to the simulator's
 * BLE link: chooser filter, 20-byte writes, notifications in, a lost link,
 * close() without onClose, reconnect via getDevices().
 */

import assert from 'node:assert/strict';
import { describe, test } from 'node:test';

import { DeviceSession } from '../src/device/deviceSession.ts';
import { BindState, ClientKind, PressKind } from '../src/protocol/generated.ts';
import { BLE_NAME_PREFIX, BleTransport, NUS_SERVICE } from '../src/transport/bleTransport.ts';
import { FakeBleDevice, FakeBluetooth } from './fakeBluetooth.ts';
import { named, recorder, rig } from './helpers.ts';

describe('BleTransport', () => {
  test('chooser filter, chunked writes, a full session with events, link loss, reconnect without the chooser', async () => {
    const t = rig();
    await t.run(async () => {
      const dev = t.sim();
      await t.session(dev, {}, { clientKind: ClientKind.WEBAPP }).connect();     // USB binds this client
      const fakeDev = new FakeBleDevice(dev);
      const bt = new FakeBluetooth([fakeDev]);
      const transport = await BleTransport.request(bt);
      assert.deepEqual(bt.lastRequest?.filters, [{ services: [NUS_SERVICE], namePrefix: BLE_NAME_PREFIX }]);
      assert.equal(transport.kind, 'ble');

      const rec = recorder();
      const closes: string[] = [];
      const s = new DeviceSession(transport, { clientId: new Uint8Array(16).fill(7), clientName: 'test', clientKind: ClientKind.WEBAPP, scheduler: t.clock },
        { ...rec, onState: (st, r) => { if (st === 'closed') closes.push(r ?? ''); } });
      t.sessions.push(s);
      const hello = await s.connect();
      assert.equal(hello.bindState, BindState.BOUND);
      assert.ok(fakeDev.writes.length > 0 && fakeDev.writes.every((n) => n <= 20), String(fakeDev.writes));
      dev.pressButton(1, PressKind.SHORT);
      await t.until(() => named(rec, 'EVT_BUTTON').length === 1);

      dev.bleOutOfRange();
      await t.until(() => s.state === 'closed');
      assert.deepEqual(closes, ['BLE link lost']);
      dev.bleInRange();

      const again = await BleTransport.remembered(bt);
      assert.ok(again);
      const s2 = new DeviceSession(again, { clientId: new Uint8Array(16).fill(7), clientName: 'test', clientKind: ClientKind.WEBAPP, scheduler: t.clock },
        rec, s.resumePoint);
      t.sessions.push(s2);
      await s2.connect();
      dev.pressButton(2, PressKind.SHORT);
      await t.until(() => named(rec, 'EVT_BUTTON').length === 2);
      assert.deepEqual(rec.events.map((e) => e.evSeq), rec.events.map((_, i) => i));
      await s2.close();
      assert.equal(fakeDev.gatt.connected, false);
    });
  });

  test('no Web Bluetooth: a clear error; no granted device: null', async () => {
    await assert.rejects(BleTransport.request(undefined), /Web Bluetooth is not available/);
    assert.equal(await BleTransport.remembered(new FakeBluetooth([])), null);
  });
});
