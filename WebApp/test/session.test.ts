/*
 * DeviceSession against the simulated control module: the app-side contract
 * of SYSTEM_ARCHITECTURE §7.4 and §9.5, end to end through real frames, on
 * virtual time.
 *
 *   - connect: HELLO, then the history replays from EVT_BOOT;
 *   - requests resolve, error statuses reject with their names;
 *   - events arrive in evSeq order, exactly once, even when frames are lost;
 *   - a reconnect (or a transport switch) continues where it stopped;
 *   - a device reboot shows as a new bootId, also under a BLE link that
 *     stays up (ERR_NO_SESSION → HELLO again);
 *   - binding: UNBOUND waits, binds live, gets kicked after 60 s;
 *   - one real-timer test keeps the realScheduler path honest.
 */

import assert from 'node:assert/strict';
import { describe, test } from 'node:test';

import { DeviceError, DeviceSession } from '../src/device/deviceSession.ts';
import { SimDevice } from '../src/device/simDevice.ts';
import {
  BindChange, BindState, BoardType, ClientKind, LANE_POWER_REQ, LANE_POWER_RSP, LANE_STATUS_REQ, LANE_STATUS_RSP, LaneState,
  MESSAGE_BY_ID, MsgId, NODE_LIST_REQ, NODE_LIST_RSP, PressKind, REBOOT_REQ, REBOOT_RSP, RebootTarget, Status,
} from '../src/protocol/generated.ts';
import type { NodeListRsp } from '../src/protocol/generated.ts';
import { named, recorder, rig } from './helpers.ts';
import type { Recorder } from './helpers.ts';

const seqs = (r: Recorder): number[] => r.events.map((e) => e.evSeq);
const isExactly = (r: Recorder, n: number): boolean => seqs(r).join() === Array.from({ length: n }, (_, i) => i).join();

describe('connect', () => {
  test('HELLO answers, the history replays from EVT_BOOT', async () => {
    const t = rig();
    await t.run(async () => {
      const dev = t.sim();
      const rec = recorder();
      const s = t.session(dev, rec);
      const hello = await s.connect();
      assert.equal(hello.bootId, dev.bootId);
      assert.equal(hello.bindState, BindState.TRUSTED);
      assert.equal(s.boardType, BoardType.CONTROL);
      assert.equal(s.state, 'open');
      assert.deepEqual(rec.boots, [dev.bootId]);
      await t.until(() => rec.events.length >= 1);
      assert.equal(rec.events[0]?.name, 'EVT_BOOT');
      assert.equal(rec.events[0]?.evSeq, 0);
    });
  });

  test('the clock estimate follows the device', async () => {
    const t = rig();
    await t.run(async () => {
      const dev = t.sim();
      const s = t.session(dev);
      await s.connect();
      await t.sleep(3000);
      const est = s.sysUsNow();
      assert.ok(est !== null);
      assert.ok(Math.abs(Number(est - dev.sysUs())) < 2000);
      assert.ok((s.rttMs() ?? 99) < 10);
    });
  });
});

describe('requests', () => {
  test('NODE_LIST and LANE_POWER resolve with decoded bodies', async () => {
    const t = rig();
    await t.run(async () => {
      const dev = t.sim();
      const rec = recorder();
      const s = t.session(dev, rec);
      await s.connect();
      const before = await s.request<NodeListRsp>(NODE_LIST_REQ, {}, MsgId.NODE_LIST, NODE_LIST_RSP);
      assert.equal(before.nodes.length, 2);                                     // CTRL + BLE
      await s.request(LANE_POWER_REQ, { on: 1 }, MsgId.LANE_POWER, LANE_POWER_RSP);
      await t.until(() => named(rec, 'EVT_LANE_STATE').filter((e) => e.value?.['state'] === LaneState.CONFIGURED).length === 6);
      const after = await s.request<NodeListRsp>(NODE_LIST_REQ, {}, MsgId.NODE_LIST, NODE_LIST_RSP);
      assert.equal(after.nodes.length, 8);
    });
  });

  test('an error status rejects with DeviceError and its name', async () => {
    const t = rig();
    await t.run(async () => {
      const s = t.session(t.sim());
      await s.connect();
      await assert.rejects(
        s.request(LANE_STATUS_REQ, { lane: 9 }, MsgId.LANE_STATUS, LANE_STATUS_RSP),
        (err: unknown) => err instanceof DeviceError && err.status === Status.ERR_INVALID_ARG && /ERR_INVALID_ARG/.test(err.message),
      );
    });
  });

  test('requestRaw: unknown ids and wrong lengths get their status', async () => {
    const t = rig();
    await t.run(async () => {
      const s = t.session(t.sim());
      await s.connect();
      assert.equal((await s.requestRaw(0x0f, new Uint8Array(0))).status, Status.ERR_UNSUPPORTED);
      assert.equal((await s.requestRaw(MsgId.PING, new Uint8Array(3))).status, Status.ERR_BAD_LENGTH);
      assert.equal((await s.requestRaw(MsgId.STATUS, new Uint8Array(0))).status, Status.OK);
    });
  });

  test('requestMsg drives any request by descriptor', async () => {
    const t = rig();
    await t.run(async () => {
      const s = t.session(t.sim());
      await s.connect();
      const msg = MESSAGE_BY_ID.get(MsgId.LANE_POWER);
      assert.ok(msg);
      assert.equal((await s.requestMsg(msg, { on: 0 }))['status'], Status.OK);
    });
  });
});

describe('events', () => {
  test('arrive in order, exactly once', async () => {
    const t = rig();
    await t.run(async () => {
      const dev = t.sim();
      const rec = recorder();
      await t.session(dev, rec).connect();
      for (let i = 0; i < 20; i++) dev.pressButton(1 + (i % 3), PressKind.SHORT);
      await t.until(() => rec.events.length === 21);
      assert.ok(isExactly(rec, 21));
    });
  });

  test('lost frames are noticed and RESUMEd: still exactly once, in order', async () => {
    const t = rig();
    await t.run(async () => {
      const dev = t.sim();
      const rec = recorder();
      await t.session(dev, rec).connect();
      await t.until(() => rec.events.length === 1);
      dev.dropNextEvents(3);
      for (let i = 0; i < 10; i++) dev.pressButton(2, PressKind.SHORT);
      await t.until(() => rec.events.length === 11);
      assert.ok(isExactly(rec, 11), seqs(rec).join());
      assert.deepEqual(rec.gaps, []);
    });
  });

  test('events overwritten on the device are reported as a gap, the rest delivered', async () => {
    const t = rig();
    await t.run(async () => {
      const dev = t.sim({ logCapacity: 8 });
      const rec = recorder();
      const s = t.session(dev, rec);
      await s.connect();
      await t.until(() => rec.events.length === 1);
      await s.close();
      for (let i = 0; i < 20; i++) dev.pressButton(1, PressKind.SHORT);   // 21 events in a log of 8
      const rec2 = recorder();
      await t.session(dev, rec2, { resume: s.resumePoint }).connect();
      await t.until(() => rec2.events.length === 8);
      assert.deepEqual(rec2.gaps, [[1, 13]]);
      assert.deepEqual(seqs(rec2), [13, 14, 15, 16, 17, 18, 19, 20]);
    });
  });
});

describe('reconnect', () => {
  test('USB → BLE with the resume point: no repeats, nothing lost (transport switch)', async () => {
    const t = rig();
    await t.run(async () => {
      const dev = t.sim();
      const rec = recorder();
      const s1 = t.session(dev, rec, { clientKind: ClientKind.WEBAPP });   // USB HELLO binds this browser
      await s1.connect();
      dev.pressButton(1, PressKind.SHORT);
      await t.until(() => named(rec, 'EVT_BUTTON').length === 1);
      dev.unplugUsb();
      await t.until(() => s1.state === 'closed');

      dev.pressButton(2, PressKind.SHORT);                                  // while nobody listens
      dev.pressButton(3, PressKind.SHORT);
      const s2 = t.session(dev, rec, { kind: 'ble', clientKind: ClientKind.WEBAPP, resume: s1.resumePoint });
      const hello = await s2.connect();
      assert.equal(hello.bindState, BindState.BOUND);
      dev.pressButton(1, PressKind.LONG);
      await t.until(() => named(rec, 'EVT_BUTTON').length === 4);
      const n = rec.events.length;
      assert.ok(isExactly(rec, n), seqs(rec).join());
      assert.deepEqual(rec.boots, [dev.bootId]);
    });
  });

  test('a device reboot closes USB and shows a new bootId on reconnect', async () => {
    const t = rig();
    await t.run(async () => {
      const dev = t.sim();
      const rec = recorder();
      const s1 = t.session(dev, rec);
      await s1.connect();
      const firstBoot = dev.bootId;
      await s1.request(REBOOT_REQ, { target: RebootTarget.APP }, MsgId.REBOOT, REBOOT_RSP);
      await t.until(() => s1.state === 'closed');
      await assert.rejects(t.session(dev, rec, { resume: s1.resumePoint }).connect(), /re-enumerating/);
      await t.sleep(500);
      const s2 = t.session(dev, rec, { resume: s1.resumePoint });
      await s2.connect();
      assert.notEqual(dev.bootId, firstBoot);
      assert.deepEqual(rec.boots, [firstBoot, dev.bootId]);
      await t.until(() => rec.events.some((e) => e.bootId === dev.bootId && e.name === 'EVT_BOOT'));
    });
  });

  test('a reboot under a BLE link: ERR_NO_SESSION, HELLO again, the new boot replays', async () => {
    const t = rig();
    await t.run(async () => {
      const dev = t.sim();
      const rec = recorder();
      const s = t.session(dev, rec, { kind: 'ble', clientKind: ClientKind.WEBAPP });
      await t.session(dev, {}, { clientKind: ClientKind.WEBAPP }).connect();   // binds over USB first
      await s.connect();
      const first = dev.bootId;
      await s.request(REBOOT_REQ, { target: RebootTarget.APP }, MsgId.REBOOT, REBOOT_RSP);
      await t.until(() => dev.bootId !== first);
      assert.equal(s.state, 'open');                                     // the BLE link stayed up
      await t.until(() => rec.boots.length === 2, 5000, 'second boot');
      await t.until(() => rec.events.some((e) => e.bootId === dev.bootId && e.name === 'EVT_BOOT'));
      dev.pressButton(1, PressKind.SHORT);
      await t.until(() => rec.events.some((e) => e.bootId === dev.bootId && e.name === 'EVT_BUTTON'));
    });
  });
});

describe('binding over BLE', () => {
  test('unknown client waits UNBOUND; the BLE button binds it live; events flow', async () => {
    const t = rig();
    await t.run(async () => {
      const dev = t.sim();
      const rec = recorder();
      const s = t.session(dev, rec, { kind: 'ble', clientKind: ClientKind.WEBAPP });
      const connecting = s.connect();
      await t.until(() => s.state === 'unbound');
      await assert.rejects(s.request(NODE_LIST_REQ, {}, MsgId.NODE_LIST, NODE_LIST_RSP),
        (e: unknown) => e instanceof DeviceError && e.status === Status.ERR_NOT_BOUND);
      await t.sleep(5000);
      dev.pressBleButton(PressKind.LONG);
      const hello = await connecting;
      assert.equal(hello.bindState, BindState.BOUND);
      assert.equal(s.state, 'open');
      await t.until(() => named(rec, 'EVT_BIND_CHANGED').some((e) => e.value?.['change'] === BindChange.ADDED));
      assert.ok(isExactly(rec, rec.events.length));
    });
  });

  test('BIND_ALLOW over USB opens the window for a waiting BLE client', async () => {
    const t = rig();
    await t.run(async () => {
      const dev = t.sim();
      const usb = t.session(dev);
      await usb.connect();
      const ble = t.session(dev, {}, { kind: 'ble', clientId: new Uint8Array(16).fill(9), clientKind: ClientKind.WEBAPP });
      const connecting = ble.connect();
      await t.until(() => ble.state === 'unbound');
      await usb.requestMsg(MESSAGE_BY_ID.get(MsgId.BIND_ALLOW)!, { windowS: 30 });
      assert.equal((await connecting).bindState, BindState.BOUND);
      assert.equal(dev.bindList.length, 1);
    });
  });

  test('not bound within 60 s: kicked, connect rejects, REJECTED is logged', async () => {
    const t = rig();
    await t.run(async () => {
      const dev = t.sim();
      const s = t.session(dev, {}, { kind: 'ble', clientKind: ClientKind.WEBAPP });
      const started = t.clock.now();
      await assert.rejects(s.connect(), /binding|closed/);
      assert.ok(t.clock.now() - started >= 60000);
      assert.ok(dev.eventLog().some((e) => e.name === 'EVT_BIND_CHANGED' && e.value['change'] === BindChange.REJECTED));
    });
  });

  test('binds cleared while connected (button 10 s): back to waiting, then bound again, nothing repeats', async () => {
    const t = rig();
    await t.run(async () => {
      const dev = t.sim();
      const rec = recorder();
      await t.session(dev, {}, { clientKind: ClientKind.WEBAPP }).connect();   // bind via USB
      const s = t.session(dev, rec, { kind: 'ble', clientKind: ClientKind.WEBAPP });
      await s.connect();
      dev.pressBleButton(PressKind.VERY_LONG);
      await t.sleep(1500);                                                       // PINGs still work...
      await assert.rejects(s.request(NODE_LIST_REQ, {}, MsgId.NODE_LIST, NODE_LIST_RSP));   // ...the rest does not
      await t.until(() => s.state === 'unbound');
      dev.pressButton(1, PressKind.SHORT);
      dev.pressBleButton(PressKind.LONG);
      await t.until(() => s.state === 'open');
      await t.until(() => named(rec, 'EVT_BUTTON').length === 3);                // VERY_LONG, BTN1, LONG
      assert.ok(isExactly(rec, rec.events.length), seqs(rec).join());
    });
  });
});

describe('stale link', () => {
  test('silence for 3 s is reported, and the next byte clears it', async () => {
    const t = rig();
    await t.run(async () => {
      const dev = t.sim();
      const stale: boolean[] = [];
      const transport = dev.transport('usb');
      let mute = false;
      const muted = {
        kind: transport.kind, label: transport.label,
        open: (h: Parameters<typeof transport.open>[0]) => transport.open({ onBytes: (b) => { if (!mute) h.onBytes(b); }, onClose: h.onClose }),
        write: (b: Uint8Array) => transport.write(b),
        close: () => transport.close(),
      };
      const s = new DeviceSession(muted, { clientId: new Uint8Array(16).fill(1), clientName: 't', scheduler: t.clock, requestTimeoutMs: 300 },
        { onStale: (x) => stale.push(x) });
      t.sessions.push(s);
      await s.connect();
      mute = true;
      await t.sleep(4500);
      assert.deepEqual(stale, [true]);
      mute = false;
      await t.sleep(1500);
      assert.deepEqual(stale, [true, false]);
    });
  });
});

describe('real timers', () => {
  test('the session works on the real scheduler too', async () => {
    const dev = new SimDevice({ seed: 1 });
    const s = new DeviceSession(dev.transport(), { clientId: new Uint8Array(16).fill(3), clientName: 'rt', pingIntervalMs: 50 });
    try {
      const hello = await s.connect();
      assert.equal(hello.bootId, dev.bootId);
      await new Promise((r) => setTimeout(r, 120));
      assert.ok(s.rttMs() !== null);
    } finally {
      await s.close();
      dev.dispose();
    }
  });
});
