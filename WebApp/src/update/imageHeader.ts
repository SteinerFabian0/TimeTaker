/*
 * The control module's firmware image header, as the bootloader checks it
 * (ControlModule/.../app/modules/image_header.h, filled by tools/image_stamp.py):
 *
 *   +0x400, 32 bytes LE: magic 'TTIM' u32, boardType u8, fw major/minor/patch
 *   u8, gitRev u32, size u32 (whole .bin), imageCrc u32 (CRC-32 over the .bin
 *   with these 32 bytes skipped), headerVersion u16 (1), reserved u16,
 *   loadAddr u32 (link address), reserved u32.
 *
 * The app slot starts at 0x0800_8000 and holds 208 KB. checkImage() is the
 * bootloader's END check (image_header.c imageCheck): the updater runs it
 * before entering the bootloader, the simulated bootloader at UPDATE_END.
 */

import { BoardType, Status } from '../protocol/generated.ts';
import { crc32 } from '../util/bytes.ts';

export const IMAGE_HEADER_OFFSET = 0x400;
export const IMAGE_HEADER_BYTES = 32;
export const IMAGE_HEADER_MAGIC = 0x4d495454;   // "TTIM"
export const IMAGE_HEADER_VERSION = 1;
export const IMAGE_APP_BASE = 0x08008000;
export const IMAGE_APP_SLOT_SIZE = 208 * 1024;
export const IMAGE_RAM_BASE = 0x20000000;
export const IMAGE_RAM_SIZE = 0x10000;

export interface ImageHeader {
  readonly magic: number;
  readonly boardType: number;
  readonly fwMajor: number;
  readonly fwMinor: number;
  readonly fwPatch: number;
  readonly gitRev: number;
  readonly size: number;
  readonly imageCrc: number;
  readonly headerVersion: number;
  readonly loadAddr: number;
}

export function parseImageHeader(img: Uint8Array): ImageHeader | null {
  if (img.length < IMAGE_HEADER_OFFSET + IMAGE_HEADER_BYTES) return null;
  const v = new DataView(img.buffer, img.byteOffset + IMAGE_HEADER_OFFSET, IMAGE_HEADER_BYTES);
  return {
    magic: v.getUint32(0, true), boardType: v.getUint8(4), fwMajor: v.getUint8(5), fwMinor: v.getUint8(6),
    fwPatch: v.getUint8(7), gitRev: v.getUint32(8, true), size: v.getUint32(12, true), imageCrc: v.getUint32(16, true),
    headerVersion: v.getUint16(20, true), loadAddr: v.getUint32(24, true),
  };
}

/** CRC-32 over size bytes with the header's 32 bytes skipped (imageCrcSkipHeader). */
export function imageCrcSkipHeader(img: Uint8Array, size = img.length): number {
  const head = crc32(img.subarray(0, Math.min(size, IMAGE_HEADER_OFFSET)));
  const after = IMAGE_HEADER_OFFSET + IMAGE_HEADER_BYTES;
  return size > after ? crc32(img.subarray(after, size), head) : head;
}

/**
 * The bootloader's verdict on a complete image: Status.OK, ERR_IMAGE_CRC
 * (bad magic/version/size, CRC, link address or vector table),
 * ERR_IMAGE_BOARD, ERR_IMAGE_TOO_BIG.
 */
export function checkImage(img: Uint8Array, boardType: number = BoardType.CONTROL): number {
  const h = parseImageHeader(img);
  if (!h || h.magic !== IMAGE_HEADER_MAGIC || h.headerVersion !== IMAGE_HEADER_VERSION
      || h.size < IMAGE_HEADER_OFFSET + IMAGE_HEADER_BYTES) return Status.ERR_IMAGE_CRC;
  if (h.boardType !== boardType) return Status.ERR_IMAGE_BOARD;
  if (h.size > IMAGE_APP_SLOT_SIZE) return Status.ERR_IMAGE_TOO_BIG;
  if (h.size !== img.length || h.loadAddr !== IMAGE_APP_BASE || imageCrcSkipHeader(img, h.size) !== h.imageCrc) return Status.ERR_IMAGE_CRC;
  const v = new DataView(img.buffer, img.byteOffset, 8);
  const sp = v.getUint32(0, true);
  const reset = v.getUint32(4, true);
  const spOk = (sp & 3) === 0 && sp > IMAGE_RAM_BASE && sp <= IMAGE_RAM_BASE + IMAGE_RAM_SIZE;
  const resetOk = (reset & 1) === 1 && reset > IMAGE_APP_BASE && reset < IMAGE_APP_BASE + h.size;
  return spOk && resetOk ? Status.OK : Status.ERR_IMAGE_CRC;
}

/** A stamped image as image_stamp.py writes it: vectors, header, filler. For the simulator and tests. */
export function makeImage(fw: { major: number; minor: number; patch: number; gitRev: number }, size = 4096,
                          boardType: number = BoardType.CONTROL, fill = 0x5a): Uint8Array {
  const img = new Uint8Array(Math.max(size, IMAGE_HEADER_OFFSET + IMAGE_HEADER_BYTES + 16));
  for (let i = 0; i < img.length; i++) img[i] = (fill + i * 31) & 0xff;
  const v = new DataView(img.buffer);
  v.setUint32(0, IMAGE_RAM_BASE + IMAGE_RAM_SIZE, true);
  v.setUint32(4, (IMAGE_APP_BASE + IMAGE_HEADER_OFFSET + IMAGE_HEADER_BYTES + 1) >>> 0, true);
  img.fill(0, IMAGE_HEADER_OFFSET, IMAGE_HEADER_OFFSET + IMAGE_HEADER_BYTES);
  const h = IMAGE_HEADER_OFFSET;
  v.setUint32(h, IMAGE_HEADER_MAGIC, true);
  img.set([boardType, fw.major, fw.minor, fw.patch], h + 4);
  v.setUint32(h + 8, fw.gitRev >>> 0, true);
  v.setUint32(h + 12, img.length, true);
  v.setUint16(h + 20, IMAGE_HEADER_VERSION, true);
  v.setUint32(h + 24, IMAGE_APP_BASE, true);
  v.setUint32(h + 16, imageCrcSkipHeader(img), true);
  return img;
}
