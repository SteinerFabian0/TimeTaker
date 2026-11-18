/*
 * The firmware updater against the simulated bootloader: the happy path over
 * USB and over BLE, a link lost mid-transfer (resume), a power loss
 * mid-transfer (start again), a damaged image (refused, nothing booted),
 * lost answers (fast-forward, repeated END), power lost right after END, and
 * the simulated bootloader's own rules (firmware update_writer.c).
 */

import assert from 'node:assert/strict';
import { describe, test } from 'node:test';

import { DeviceApi } from '../src/device/deviceApi.ts';
import { DeviceError, DeviceSession, LinkClosedError } from '../src/device/deviceSession.ts';
import { makeSimImage, SIM_APP_FW } from '../src/device/simDevice.ts';
import type { SimDevice } from '../src/device/simDevice.ts';
import { BoardType, ClientKind, Status } from '../src/protocol/generated.ts';
import { updateFirmware, UpdateError } from '../src/update/firmwareUpdater.ts';
import type { UpdateLink, UpdateProgress } from '../src/update/firmwareUpdater.ts';
import { checkImage, IMAGE_HEADER_OFFSET, imageCrcSkipHeader } from '../src/update/imageHeader.ts';
import { crc32 } from '../src/util/bytes.ts';
import { rig } from './helpers.ts';
import type { Rig } from './helpers.ts';

const NEW_FW = { major: 0, minor: 3, patch: 1, gitRev: 0xabc1234 };

function connector(t: Rig, dev: SimDevice, kind: 'usb' | 'ble' = 'usb', onChunk?: (n: number) => void,
                   wrap?: (api: DeviceApi) => void): () => Promise<UpdateLink> {
  let chunks = 0;
  return async () => {
    const s = new DeviceSession(dev.transport(kind), {
      clientId: new Uint8Array(16).fill(0x31), clientName: 'updater', clientKind: ClientKind.WEBAPP, scheduler: t.clock, requestTimeoutMs: 400,
    });
    await s.connect();
    const api = new DeviceApi(s, { eraseTimeoutMs: 5000 });
    const orig = api.updateChunk.bind(api);
    api.updateChunk = async (offset, data) => { onChunk?.(++chunks); return orig(offset, data); };
    wrap?.(api);
    return { api, boardType: s.boardType, close: () => s.close() };
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

  test('wrong board type or an image linked for another address: refused before the bootloader is entered', async () => {
    const t = rig();
    await t.run(async () => {
      const dev = t.sim();
      await assert.rejects(updateFirmware(makeSimImage(NEW_FW, 1000, BoardType.LANE), { connect: connector(t, dev), scheduler: t.clock }),
        /ERR_IMAGE_BOARD/);
      const standalone = makeSimImage(NEW_FW, 2000);                         // the .bin linked at 0x0800_0000
      new DataView(standalone.buffer).setUint32(IMAGE_HEADER_OFFSET + 24, 0x08000000, true);
      new DataView(standalone.buffer).setUint32(IMAGE_HEADER_OFFSET + 16, imageCrcSkipHeader(standalone), true);
      await assert.rejects(updateFirmware(standalone, { connect: connector(t, dev), scheduler: t.clock }), /ERR_IMAGE_CRC/);
      assert.equal(dev.mode, 'app');
      assert.equal(dev.appValid, true);
      assert.deepEqual(dev.appFirmware, SIM_APP_FW);
    });
  });

  test('lost chunk answers: the real nextOffset fast-forwards the host', async () => {
    const t = rig();
    await t.run(async () => {
      const dev = t.sim();
      const image = makeSimImage(NEW_FW, 20_000);
      let n = 0;
      const r = await updateFirmware(image, {
        connect: connector(t, dev, 'usb', undefined, (api) => {
          const orig = api.updateChunk.bind(api);
          api.updateChunk = async (offset, data) => {
            const next = await orig(offset, data);
            if (++n === 1 || n === 30) throw new LinkClosedError('UPDATE_CHUNK', 'answer lost');   // written, answer lost
            return next;
          };
        }),
        scheduler: t.clock,
      });
      assert.equal(r.recoveries, 2);
      assert.equal(crc32(dev.flashedImage!), crc32(image));
    });
  });

  test('UPDATE_END answer lost: the repeated END answers OK', async () => {
    const t = rig();
    await t.run(async () => {
      const dev = t.sim();
      const image = makeSimImage(NEW_FW, 5000);
      let ends = 0;
      const r = await updateFirmware(image, {
        connect: connector(t, dev, 'usb', undefined, (api) => {
          const orig = api.updateEnd.bind(api);
          api.updateEnd = async () => { await orig(); if (++ends === 1) throw new LinkClosedError('UPDATE_END', 'answer lost'); };
        }),
        scheduler: t.clock,
      });
      assert.equal(ends, 2);
      assert.equal(r.recoveries, 1);
      assert.deepEqual(dev.appFirmware, NEW_FW);
    });
  });

  test('power lost right after a good UPDATE_END: the new app boots, the updater verifies it', async () => {
    const t = rig();
    await t.run(async () => {
      const dev = t.sim();
      const image = makeSimImage(NEW_FW, 5000);
      const r = await updateFirmware(image, {
        connect: connector(t, dev, 'usb', undefined, (api) => {
          const orig = api.updateEnd.bind(api);
          api.updateEnd = async () => { await orig(); dev.powerCycle(); throw new LinkClosedError('UPDATE_END', 'device lost power'); };
        }),
        scheduler: t.clock,
      });
      assert.equal(dev.mode, 'app');
      assert.equal(r.ctrl.imageCrc, crc32(image));
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

  test('simulated bootloader: resumed BEGIN, fast-forward, BAD_LENGTH, header check, BLE bind gate', async () => {
    const t = rig();
    await t.run(async () => {
      const dev = t.sim();
      const usb = t.session(dev, {}, { clientKind: ClientKind.WEBAPP, clientId: new Uint8Array(16).fill(0x31) });
      await usb.connect();
      const api = new DeviceApi(usb, { eraseTimeoutMs: 5000 });
      await api.reboot('bootloader');
      await usb.close();
      await t.sleep(2000);
      const s = t.session(dev, {}, { clientKind: ClientKind.WEBAPP, clientId: new Uint8Array(16).fill(0x31) });
      await s.connect();
      assert.equal(s.boardType, BoardType.CONTROL_BOOT);
      const b = new DeviceApi(s, { eraseTimeoutMs: 5000 });
      const image = makeSimImage(NEW_FW, 3000);
      const crc = crc32(image);
      assert.equal(checkImage(image), Status.OK);
      await assert.rejects(b.updateBegin(BoardType.CONTROL, 0, crc), (e) => e instanceof DeviceError && e.status === Status.ERR_INVALID_ARG);
      await assert.rejects(b.updateBegin(BoardType.CONTROL, 208 * 1024 + 1, crc), (e) => e instanceof DeviceError && e.status === Status.ERR_IMAGE_TOO_BIG);
      await b.updateBegin(BoardType.CONTROL, image.length, crc);
      assert.equal(await b.updateChunk(0, image.subarray(0, 224)), 224);
      assert.equal(await b.updateChunk(224, image.subarray(224, 448)), 448);
      await b.updateBegin(BoardType.CONTROL, image.length, crc);                     // resume: nothing erased
      assert.equal(await b.updateChunk(0, image.subarray(0, 224)), 448, 'fast-forwarded');
      assert.equal(await b.updateChunk(400, image.subarray(400, 600)), 600, 'overlap compared, rest written');
      await assert.rejects(b.updateChunk(600, new Uint8Array(225)), (e) => e instanceof DeviceError && e.status === Status.ERR_BAD_LENGTH);
      const bad = image.subarray(0, 224).map((x) => x ^ 1);
      await assert.rejects(b.updateChunk(0, bad), (e) => e instanceof DeviceError && e.status === Status.ERR_UPDATE_SEQUENCE);

      // A stamped image whose header names another board: the whole-file CRC matches, END refuses it.
      const lane = makeSimImage(NEW_FW, 3000, BoardType.LANE);
      await b.updateBegin(BoardType.CONTROL, lane.length, crc32(lane));
      for (let o = 0; o < lane.length; o += 224) await b.updateChunk(o, lane.subarray(o, o + 224));
      await assert.rejects(b.updateEnd(), (e) => e instanceof DeviceError && e.status === Status.ERR_IMAGE_BOARD);
      await assert.rejects(b.bootApp(), (e) => e instanceof DeviceError && e.status === Status.ERR_INVALID_STATE);
      await s.close();

      // BLE: a client not on the bind list stays UNBOUND in the bootloader.
      const stranger = t.session(dev, {}, { kind: 'ble', clientKind: ClientKind.WEBAPP, clientId: new Uint8Array(16).fill(0x77) });
      await assert.rejects(stranger.connect(), /binding/);                       // UNBOUND, never served
      assert.equal(dev.mode, 'bootloader');
    });
  });
});
