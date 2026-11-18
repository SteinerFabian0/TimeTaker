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
 *   0. before anything: the image passes the bootloader's own header check
 *      (imageHeader.ts: stamped, this board, linked for 0x0800_8000);
 *   3. UPDATE_BEGIN{board, size, crc32} (erases: long timeout) → chunkMax.
 *      The same BEGIN again resumes (no erase), so the first chunk's answer
 *      may fast-forward;
 *   4. UPDATE_CHUNK at contiguous offsets; every answer's nextOffset is
 *      followed (it may be further on than the chunk's end). A timeout or a
 *      lost link: reconnect and re-send bytes below the last nextOffset,
 *      which answers the bootloader's real nextOffset; if the bootloader lost
 *      the update (it was reset: ERR_UPDATE_SEQUENCE), start again with
 *      UPDATE_BEGIN;
 *   5. UPDATE_END (CRC check; ERR_IMAGE_CRC fails the update). Link lost
 *      during END: reconnect; the bootloader answers a repeated END OK, a
 *      reset bootloader ERR_UPDATE_SEQUENCE (start over), and if the new app
 *      already runs (power lost after END) step 6 skips BOOT_APP;
 *   6. BOOT_APP, reconnect, and check NODE_LIST: CTRL is CONTROL again and,
 *      when the firmware reports one, imageCrc equals the image's CRC-32.
 *
 * The resume relies on the UPDATE_CHUNK resume rule (host_protocol.toml,
 * protocol 0.3): matching re-sent bytes are acknowledged with the real
 * nextOffset.
 */

import type { DeviceApi } from '../device/deviceApi.ts';
import { DeviceError, LinkClosedError, TimeoutError } from '../device/deviceSession.ts';
import { BoardType, NodeClass, Status, statusName } from '../protocol/generated.ts';
import type { NodeInfo } from '../protocol/generated.ts';
import { crc32 } from '../util/bytes.ts';
import { checkImage } from './imageHeader.ts';
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
  const recover = async (want: 'boot' | 'any' = 'boot'): Promise<UpdateLink> => {
    if (++recoveries > maxRecoveries) throw new UpdateError(`gave up after ${maxRecoveries} recoveries`);
    try { await link?.close(); } catch { /* gone */ }
    link = null;
    return connect(want);
  };

  // The bootloader's own END check, before anything is touched: a wrong file
  // (unstamped, another board, the standalone .bin linked at 0x0800_0000)
  // never gets as far as an erase.
  const verdict = checkImage(image, board);
  if (verdict !== Status.OK) throw new UpdateError(`not an image for this slot: ${statusName(verdict)}`);

  try {
    report('connect');
    link = await connect('any');
    let booted = (await detectMode(link.api, link.boardType)).mode === 'app';
    if (booted) {
      report('enterBootloader');
      await link.api.reboot('bootloader');
      await sleep(sched, 300);
      await link.close();
      link = await connect('boot');
      booted = false;
    }

    // ---- BEGIN, CHUNKs (resumable), END ----
    let chunkMax = 0;
    let offset = 0;
    let lastAck: { offset: number; len: number } | null = null;
    /** An OK nextOffset: at least past the chunk, fast-forwarded at most to the end. */
    const advance = (at: number, len: number, next: number): number => {
      if (next < at + len || next > image.length) throw new UpdateError(`chunk at ${at} answered nextOffset ${next}`);
      lastAck = { offset: next - len, len };   // bytes below next: a re-send answers the real nextOffset
      return next;
    };
    const begin = async (): Promise<void> => {
      report('erase');
      chunkMax = await link!.api.updateBegin(board, image.length, crc);
      if (chunkMax < 1) throw new UpdateError(`bootloader offers chunkMax ${chunkMax}`);
      offset = 0;            // a resumed BEGIN keeps its position: the first chunk's answer fast-forwards
      lastAck = null;
    };
    const beginRetrying = async (): Promise<void> => {
      for (;;) {
        try { await begin(); return; } catch (e) { if (isLinkLoss(e)) link = await recover(); else throw e; }
      }
    };
    /** After a lost link: where does the bootloader stand? */
    const resume = async (): Promise<void> => {
      link = await recover();
      if (!lastAck) { await beginRetrying(); return; }
      try {
        const a: { offset: number; len: number } = lastAck;
        offset = advance(a.offset, a.len, await link.api.updateChunk(a.offset, image.subarray(a.offset, a.offset + a.len)));
      } catch (e) {
        if (e instanceof DeviceError && e.status === Status.ERR_UPDATE_SEQUENCE) await beginRetrying();   // the bootloader lost it
        else if (!isLinkLoss(e)) throw e;   // lost again: the write loop's next chunk recovers once more
      }
    };
    /** 'ok', 'app' (power lost after a good END: the new app runs), 'restart' (the bootloader lost the update). */
    const finish = async (): Promise<'ok' | 'app' | 'restart'> => {
      for (;;) {
        try {
          await link!.api.updateEnd();
          return 'ok';
        } catch (e) {
          if (e instanceof DeviceError && e.status === Status.ERR_IMAGE_CRC) throw new UpdateError('the bootloader computed another CRC: image damaged in transfer, nothing booted');
          if (e instanceof DeviceError && e.status === Status.ERR_UPDATE_SEQUENCE) return 'restart';
          if (!isLinkLoss(e)) throw e;
          link = await recover('any');        // a repeated END after success answers OK
          if ((await detectMode(link.api, link.boardType)).mode === 'app') return 'app';
        }
      }
    };

    for (;;) {
      await beginRetrying();
      while (offset < image.length) {
        report('write', offset);
        const len = Math.min(chunkMax, image.length - offset);
        try {
          offset = advance(offset, len, await link!.api.updateChunk(offset, image.subarray(offset, offset + len)));
        } catch (e) {
          if (isLinkLoss(e) || (e instanceof DeviceError && e.status === Status.ERR_UPDATE_SEQUENCE)) await resume();
          else throw e;
        }
      }
      report('verify', offset);
      const r = await finish();
      if (r === 'restart') {
        if (++recoveries > maxRecoveries) throw new UpdateError(`gave up after ${maxRecoveries} recoveries`);
        continue;
      }
      booted = r === 'app';
      break;
    }

    // ---- boot the new app and check it ----
    report('boot', image.length);
    if (!booted) {
      await link!.api.bootApp();
      await sleep(sched, 300);
      await link!.close();
      link = await connect('app');
    }
    const { ctrl } = await detectMode(link.api, link.boardType);
    if (!ctrl) throw new UpdateError('the app did not list its CTRL node');
    if (ctrl.imageCrc !== 0 && ctrl.imageCrc !== crc) throw new UpdateError(`running image CRC ${ctrl.imageCrc.toString(16)} ≠ ${crc.toString(16)}`);
    report('done', image.length);
    return { ctrl, crc32: crc, recoveries };
  } finally {
    try { await (link as UpdateLink | null)?.close(); } catch { /* gone */ }
  }
}
