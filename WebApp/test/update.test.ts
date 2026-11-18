/*
 * The firmware updater against the simulated bootloader: the happy path over
 * USB and over BLE, a link lost mid-transfer (resume), a power loss
 * mid-transfer (start again), a damaged image (refused, nothing booted).
 */

import assert from 'node:assert/strict';
import { describe, test } from 'node:test';

import { DeviceApi } from '../src/device/deviceApi.ts';
import { DeviceSession } from '../src/device/deviceSession.ts';
import { makeSimImage, SIM_APP_FW } from '../src/device/simDevice.ts';
import type { SimDevice } from '../src/device/simDevice.ts';
import { BoardType, ClientKind } from '../src/protocol/generated.ts';
import { updateFirmware, UpdateError } from '../src/update/firmwareUpdater.ts';
import type { UpdateLink, UpdateProgress } from '../src/update/firmwareUpdater.ts';
import { crc32 } from '../src/util/bytes.ts';
import { rig } from './helpers.ts';
import type { Rig } from './helpers.ts';

const NEW_FW = { major: 0, minor: 3, patch: 1, gitRev: 0xabc1234 };

function connector(t: Rig, dev: SimDevice, kind: 'usb' | 'ble' = 'usb', onChunk?: (n: number) => void): () => Promise<UpdateLink> {
  let chunks = 0;
  return async () => {
    const s = new DeviceSession(dev.transport(kind), {
      clientId: new Uint8Array(16).fill(0x31), clientName: 'updater', clientKind: ClientKind.WEBAPP, scheduler: t.clock, requestTimeoutMs: 400,
    });
    await s.connect();
    const api = new DeviceApi(s, { eraseTimeoutMs: 5000 });
    const orig = api.updateChunk.bind(api);
    api.updateChunk = async (offset, data) => { onChunk?.(++chunks); return orig(offset, data); };
    return { api, close: () => s.close() };
  };
}

describe('firmware update', () => {
  test('USB: app → bootloader → image → new app with the image CRC', async () => {
    const t = rig();
    await t.run(async () => {
      const dev = t.sim();
      const image = makeSimImage(NEW_FW, 20_000);
      const phases: UpdateProgress['phase'][] = [];
      const r = await updateFirmware(image, { connect: connector(t, dev), scheduler: t.clock, onProgress: (p) => { if (phases.at(-1) !== p.phase) phases.push(p.phase); } });
      assert.deepEqual(phases, ['connect', 'enterBootloader', 'erase', 'write', 'verify', 'boot', 'done']);
      assert.equal(dev.mode, 'app');
      assert.equal(r.ctrl.boardType, BoardType.CONTROL);
      assert.equal(r.ctrl.imageCrc, crc32(image));
      assert.deepEqual(dev.appFirmware, NEW_FW);
      assert.equal(r.recoveries, 0);
    });
  });

  test('BLE: the link survives the reset, the update works the same', async () => {
    const t = rig();
    await t.run(async () => {
      const dev = t.sim();
      const usb = t.session(dev, {}, { clientKind: ClientKind.WEBAPP, clientId: new Uint8Array(16).fill(0x31) });
      await usb.connect();                                        // binds this client for BLE
      await usb.close();
      const image = makeSimImage(NEW_FW, 3000);
      await updateFirmware(image, { connect: connector(t, dev, 'ble'), scheduler: t.clock });
      assert.deepEqual(dev.appFirmware, NEW_FW);
    });
  });

  test('USB pulled mid-transfer: reconnect and resume from the last acknowledged chunk', async () => {
    const t = rig();
    await t.run(async () => {
      const dev = t.sim();
      const image = makeSimImage(NEW_FW, 30_000);
      const r = await updateFirmware(image, {
        connect: connector(t, dev, 'usb', (n) => {
          if (n === 40) { dev.unplugUsb(); t.clock.setTimeout(() => dev.plugUsb(), 1500); }
        }),
        scheduler: t.clock,
      });
      assert.equal(r.recoveries, 1);
      assert.equal(dev.flashedImage?.length, image.length);
      assert.equal(crc32(dev.flashedImage!), crc32(image));
    });
  });

  test('power lost mid-transfer: the bootloader stays, the update starts over', async () => {
    const t = rig();
    await t.run(async () => {
      const dev = t.sim();
      const image = makeSimImage(NEW_FW, 30_000);
      const r = await updateFirmware(image, {
        connect: connector(t, dev, 'usb', (n) => { if (n === 50) dev.powerCycle(); }),
        scheduler: t.clock,
      });
      assert.ok(r.recoveries >= 1);
      assert.deepEqual(dev.appFirmware, NEW_FW);
    });
  });

  test('wrong board type: refused before anything is erased... after entering the bootloader', async () => {
    const t = rig();
    await t.run(async () => {
      const dev = t.sim();
      await assert.rejects(updateFirmware(makeSimImage(NEW_FW, 1000), { connect: connector(t, dev), scheduler: t.clock, boardType: BoardType.LANE }),
        /ERR_IMAGE_BOARD/);
      assert.equal(dev.appValid, true);
      assert.deepEqual(dev.appFirmware, SIM_APP_FW);
    });
  });

  test('an image damaged in transfer fails UPDATE_END and is never booted', async () => {
    const t = rig();
    await t.run(async () => {
      const dev = t.sim();
      const image = makeSimImage(NEW_FW, 5000);
      const connect = connector(t, dev);
      const damaging = async (): Promise<UpdateLink> => {
        const l = await connect();
        const orig = l.api.updateChunk.bind(l.api);
        l.api.updateChunk = (offset, data) => orig(offset, offset === 224 ? data.map((x) => x ^ 1) : data);
        return l;
      };
      await assert.rejects(updateFirmware(image, { connect: damaging, scheduler: t.clock }), UpdateError);
      assert.equal(dev.mode, 'bootloader');
      assert.equal(dev.appValid, false);
    });
  });
});
