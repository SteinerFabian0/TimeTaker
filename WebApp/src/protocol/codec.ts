/*
 * Frame codec: COBS framing with a CRC-16, the web app's twin of the
 * firmware's frame_codec.c.
 *
 *     wire = COBS( raw | crc16 LE ) 0x00
 *
 * CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no xorout).
 * The encoder must stay byte-identical to frame_codec.c and gen_protocol.py:
 * a 0xFF COBS block is always followed by a new block, even at the end of the
 * data. test/protocol.test.ts checks it against protocol/golden/golden.json.
 */

export const CRC_BYTES = 2;
export const DELIMITER = 0x00;

export const FrameResult = {
  NONE: 0,
  READY: 1,
  ERR_CRC: -1,
  ERR_COBS: -2,
  ERR_OVERFLOW: -3,
  ERR_SHORT: -4,
} as const;
export type FrameResult = (typeof FrameResult)[keyof typeof FrameResult];

export function crc16(data: Uint8Array, crc = 0xffff): number {
  for (const byte of data) {
    crc ^= byte << 8;
    for (let bit = 0; bit < 8; bit++) {
      crc = crc & 0x8000 ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
    }
  }
  return crc;
}

/** Worst-case wire length for rawLen bytes of raw. */
export function wireBound(rawLen: number): number {
  return rawLen + CRC_BYTES + Math.floor((rawLen + CRC_BYTES) / 254) + 2;
}

/** Encodes head | body as one frame, delimiter included. */
export function frameEncode(head: Uint8Array, body: Uint8Array = new Uint8Array(0)): Uint8Array {
  if (head.length + body.length === 0) throw new RangeError('frameEncode: empty frame');
  const crc = crc16(body, crc16(head));
  const out = new Uint8Array(wireBound(head.length + body.length));
  let pos = 1;
  let codeIdx = 0;
  let code = 1;

  const byte = (b: number): void => {
    if (b === 0) {
      out[codeIdx] = code;
      codeIdx = pos++;
      code = 1;
      return;
    }
    out[pos++] = b;
    code++;
    if (code === 0xff) {
      out[codeIdx] = code;
      codeIdx = pos++;
      code = 1;
    }
  };

  for (const b of head) byte(b);
  for (const b of body) byte(b);
  byte(crc & 0xff);
  byte(crc >> 8);
  out[codeIdx] = code;
  out[pos++] = DELIMITER;
  return out.slice(0, pos);
}

export interface FrameStats {
  frames: number;
  crcErrors: number;
  cobsErrors: number;
  overflows: number;
  shortFrames: number;
}

/**
 * Streaming decoder: feed it bytes as they arrive (Web Serial chunks, BLE
 * notifications), get raw frames out. Resynchronises at every 0x00.
 */
export class FrameDecoder {
  readonly stats: FrameStats = { frames: 0, crcErrors: 0, cobsErrors: 0, overflows: 0, shortFrames: 0 };
  /** The raw frame (CRC stripped) after push() returned READY. A copy: it stays valid. */
  frame: Uint8Array = new Uint8Array(0);

  #buf: Uint8Array;
  #len = 0;
  #blockCode = 0;
  #blockLeft = 0;
  #discarding = false;

  /** maxRaw: the largest raw frame accepted (header + payload for the host protocol). */
  constructor(maxRaw: number) {
    this.#buf = new Uint8Array(maxRaw + CRC_BYTES);
  }

  /** Drops any partial frame, keeps the statistics (link reset, reconnect). */
  reset(): void {
    this.#len = 0;
    this.#blockCode = 0;
    this.#blockLeft = 0;
    this.#discarding = false;
  }

  push(byte: number): FrameResult {
    if (byte === DELIMITER) return this.#end();
    if (this.#discarding) return FrameResult.NONE;

    if (this.#blockLeft === 0) {
      if (this.#blockCode !== 0 && this.#blockCode !== 0xff) this.#append(0);
      this.#blockCode = byte;
      this.#blockLeft = byte - 1;
      return FrameResult.NONE;
    }
    this.#append(byte);
    this.#blockLeft--;
    return FrameResult.NONE;
  }

  /**
   * Feeds a chunk. Calls onFrame for every good frame and returns the errors
   * seen (empty when all was well), for the link diagnostics.
   */
  feed(chunk: Uint8Array, onFrame: (raw: Uint8Array) => void): FrameResult[] {
    const errors: FrameResult[] = [];
    for (const b of chunk) {
      const r = this.push(b);
      if (r === FrameResult.READY) onFrame(this.frame);
      else if (r !== FrameResult.NONE) errors.push(r);
    }
    return errors;
  }

  #append(b: number): void {
    if (this.#len >= this.#buf.length) {
      this.#discarding = true;
      return;
    }
    this.#buf[this.#len++] = b;
  }

  #end(): FrameResult {
    let result: FrameResult;
    if (this.#discarding) {
      this.stats.overflows++;
      result = FrameResult.ERR_OVERFLOW;
    } else if (this.#blockCode === 0) {
      result = FrameResult.NONE; // empty frame
    } else if (this.#blockLeft !== 0) {
      this.stats.cobsErrors++;
      result = FrameResult.ERR_COBS;
    } else if (this.#len < CRC_BYTES + 1) {
      this.stats.shortFrames++;
      result = FrameResult.ERR_SHORT;
    } else {
      const rawLen = this.#len - CRC_BYTES;
      const want = (this.#buf[rawLen] ?? 0) | ((this.#buf[rawLen + 1] ?? 0) << 8);
      if (crc16(this.#buf.subarray(0, rawLen)) === want) {
        this.frame = this.#buf.slice(0, rawLen);
        this.stats.frames++;
        result = FrameResult.READY;
      } else {
        this.stats.crcErrors++;
        result = FrameResult.ERR_CRC;
      }
    }
    this.reset();
    return result;
  }
}
