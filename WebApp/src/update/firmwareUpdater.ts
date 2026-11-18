/*
 * Control module firmware update (SYSTEM_ARCHITECTURE §10.1, CONTROL guide
 * §5.7), over USB or BLE alike:
 *
 *   1. connect; find out who answers: HELLO boardType CONTROL (the app) or
 *      CONTROL_BOOT (the bootloader). Firmware older than 0.3 reports 0: then
 *      NODE_LIST CTRL boardType decides (ERR_UNSUPPORTED means bootloader);
 *   2. app: REBOOT{BOOTLOADER}, drop the link, reconnect until the
 *      bootloader answers (USB re-enumerates; a BLE link survives the reset
 *      but its session does not, so it is reopened too);
 *   3. UPDATE_BEGIN{board, size, crc32} (erases: long timeout) → chunkMax;
 *   4. UPDATE_CHUNK at contiguous offsets. A timeout is retried with the
 *      same chunk (the bootloader acknowledges a repeated chunk again). A lost
 *      link: reconnect and RESUME by re-sending the last acknowledged chunk,
 *      which answers the bootloader's real nextOffset (it may be further on,
 *      when a later chunk was written but its answer lost); if the bootloader lost the update (it was
 *      reset: ERR_UPDATE_SEQUENCE), start again with UPDATE_BEGIN;
 *   5. UPDATE_END (CRC check; ERR_IMAGE_CRC fails the update);
 *   6. BOOT_APP, reconnect, and check NODE_LIST: CTRL is CONTROL again and,
 *      when the firmware reports one, imageCrc equals the image's CRC-32.
 *
 * The resume relies on the UPDATE_CHUNK resume rule (host_protocol.toml,
 * protocol 0.3): matching re-sent bytes are acknowledged with the real
 * nextOffset.
 */

import type { DeviceApi } from '../device/deviceApi.ts';
import { DeviceError, LinkClosedError, TimeoutError } from '../device/deviceSession.ts';
import { BoardType, NodeClass, Status } from '../protocol/generated.ts';
import type { NodeInfo } from '../protocol/generated.ts';
import { crc32 } from '../util/bytes.ts';
import { realScheduler, sleep } from '../util/clock.ts';
import type { Scheduler } from '../util/clock.ts';

/** One open connection the updater can use. */
export interface UpdateLink {
  readonly api: DeviceApi;
  /** HELLO boardType of this connection (DeviceSession.boardType); null/0 = unknown, ask NODE_LIST. */
  readonly boardType?: number | null;
  close(): Promise<void>;
}

export interface UpdateProgress {
  readonly phase: 'connect' | 'enterBootloader' | 'erase' | 'write' | 'verify' | 'boot' | 'done';
  readonly offset: number;
  readonly size: number;
  /** Link losses and restarts so far. */
  readonly recoveries: number;
}

export interface UpdateOptions {
  /** Opens a fresh connection (throws when the device is not there yet). */
  readonly connect: () => Promise<UpdateLink>;
  readonly boardType?: number;
  readonly scheduler?: Scheduler;
  readonly onProgress?: (p: UpdateProgress) => void;
  /** Link losses / restarts tolerated before giving up. */
  readonly maxRecoveries?: number;
  /** How long to keep trying to reconnect after a reboot or a lost link. */
  readonly reconnectTimeoutMs?: number;
}

export interface UpdateResult {
  readonly ctrl: NodeInfo;
  readonly crc32: number;
  readonly recoveries: number;
}

export class UpdateError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'UpdateError';
  }
}

const isLinkLoss = (e: unknown): boolean => e instanceof LinkClosedError || e instanceof TimeoutError
  || (e instanceof Error && /session not open|link closed|not connected/.test(e.message));

/** 'app' | 'boot', from the HELLO board type; NODE_LIST for firmware that does not report it. */
export async function detectMode(api: DeviceApi, boardType?: number | null): Promise<{ mode: 'app' | 'boot'; ctrl: NodeInfo | null }> {
  if (boardType === BoardType.CONTROL_BOOT) return { mode: 'boot', ctrl: null };
  if (boardType === BoardType.CONTROL) {
    const ctrl = (await api.nodeList()).find((n) => n.nodeClass === NodeClass.CTRL) ?? null;
    return { mode: 'app', ctrl };
  }
  try {
    const ctrl = (await api.nodeList()).find((n) => n.nodeClass === NodeClass.CTRL) ?? null;
    return { mode: ctrl?.boardType === BoardType.CONTROL_BOOT ? 'boot' : 'app', ctrl };
  } catch (e) {
    if (e instanceof DeviceError && e.status === Status.ERR_UNSUPPORTED) return { mode: 'boot', ctrl: null };
    throw e;
  }
}

export async function updateFirmware(image: Uint8Array, o: UpdateOptions): Promise<UpdateResult> {
  const sched = o.scheduler ?? realScheduler;
  const board = o.boardType ?? BoardType.CONTROL;
  const crc = crc32(image);
  const maxRecoveries = o.maxRecoveries ?? 8;
  const reconnectMs = o.reconnectTimeoutMs ?? 15000;
  let recoveries = 0;
  let link: UpdateLink | null = null;
  const report = (phase: UpdateProgress['phase'], offset = 0): void =>
    o.onProgress?.({ phase, offset, size: image.length, recoveries });

  const connect = async (want: 'app' | 'boot' | 'any'): Promise<UpdateLink> => {
    const end = sched.now() + reconnectMs;
    let lastErr: unknown = null;
    for (;;) {
      try {
        const l = await o.connect();
        const { mode } = await detectMode(l.api, l.boardType);
        if (want === 'any' || mode === want) return l;
        await l.close();
        lastErr = new UpdateError(`device answers as ${mode}, waiting for ${want}`);
      } catch (e) {
        lastErr = e;
      }
      if (sched.now() >= end) throw new UpdateError(`no connection within ${reconnectMs} ms: ${String(lastErr instanceof Error ? lastErr.message : lastErr)}`);
      await sleep(sched, 250);
    }
  };
  const recover = async (): Promise<UpdateLink> => {
    if (++recoveries > maxRecoveries) throw new UpdateError(`gave up after ${maxRecoveries} recoveries`);
    try { await link?.close(); } catch { /* gone */ }
    link = null;
    return connect('boot');
  };

  try {
    report('connect');
    link = await connect('any');
    if ((await detectMode(link.api, link.boardType)).mode === 'app') {
      report('enterBootloader');
      await link.api.reboot('bootloader');
      await sleep(sched, 300);
      await link.close();
      link = await connect('boot');
    }

    // ---- BEGIN, CHUNKs (resumable), END ----
    let chunkMax = 0;
    let offset = 0;
    let lastAck: { offset: number; len: number } | null = null;
    const begin = async (): Promise<void> => {
      report('erase');
      chunkMax = await link!.api.updateBegin(board, image.length, crc);
      if (chunkMax < 1) throw new UpdateError(`bootloader offers chunkMax ${chunkMax}`);
      offset = 0;
      lastAck = null;
    };
    /** After a lost link: where does the bootloader stand? */
    const resume = async (): Promise<void> => {
      link = await recover();
      if (!lastAck) { await begin(); return; }
      try {
        const next = await link.api.updateChunk(lastAck.offset, image.subarray(lastAck.offset, lastAck.offset + lastAck.len));
        if (next < lastAck.offset + lastAck.len || next > image.length) throw new UpdateError(`resume answered offset ${next}`);
        offset = next;
      } catch (e) {
        if (e instanceof DeviceError && e.status === Status.ERR_UPDATE_SEQUENCE) await begin();   // the bootloader lost it
        else throw e;
      }
    };

    for (;;) {
      try { await begin(); break; } catch (e) { if (isLinkLoss(e)) link = await recover(); else throw e; }
    }
    while (offset < image.length) {
      report('write', offset);
      const len = Math.min(chunkMax, image.length - offset);
      try {
        const next = await link!.api.updateChunk(offset, image.subarray(offset, offset + len));
        if (next !== offset + len) throw new UpdateError(`chunk at ${offset} answered nextOffset ${next}`);
        lastAck = { offset, len };
        offset = next;
      } catch (e) {
        if (isLinkLoss(e) || (e instanceof DeviceError && e.status === Status.ERR_UPDATE_SEQUENCE)) await resume();
        else throw e;
      }
    }
    report('verify', offset);
    for (;;) {
      try {
        await link!.api.updateEnd();
        break;
      } catch (e) {
        if (e instanceof DeviceError && e.status === Status.ERR_IMAGE_CRC) throw new UpdateError('the bootloader computed another CRC: image damaged in transfer, nothing booted');
        if (!isLinkLoss(e)) throw e;
        link = await recover();                 // END is idempotent after success
      }
    }

    // ---- boot the new app and check it ----
    report('boot', offset);
    await link!.api.bootApp();
    await sleep(sched, 300);
    await link!.close();
    link = await connect('app');
    const { ctrl } = await detectMode(link.api, link.boardType);
    if (!ctrl) throw new UpdateError('the app did not list its CTRL node');
    if (ctrl.imageCrc !== 0 && ctrl.imageCrc !== crc) throw new UpdateError(`running image CRC ${ctrl.imageCrc.toString(16)} ≠ ${crc.toString(16)}`);
    report('done', offset);
    return { ctrl, crc32: crc, recoveries };
  } finally {
    try { await (link as UpdateLink | null)?.close(); } catch { /* gone */ }
  }
}
