/*
 * Little-endian struct encoding for the host protocol, driven by the
 * descriptors in generated.ts.
 *
 * Mirrors the reference packer in protocol/gen_protocol.py and the packed C
 * structs of host_protocol.h. test/protocol.test.ts holds all three to the
 * same bytes (protocol/golden/golden.json).
 *
 * Value mapping:
 *   u8..u32, i8..i32  number (integer, range-checked)
 *   u64, i64          bigint (never number: sysUs passes 2^53 after ~285 years
 *                     of uptime, but evSeq-style IDs and hashes must not round)
 *   u8[N]             Uint8Array of exactly N bytes
 *   str[N]            string, UTF-8, zero-padded; decoding stops at the first 0
 *   struct            plain object
 *   tail T[]          array of T; u8[] → Uint8Array; str[] → string
 *
 * Encoding throws ProtocolError on any value that does not fit, and never
 * truncates silently. Decoding throws ProtocolError on a wrong length.
 */

export type ScalarType = 'u8' | 'i8' | 'u16' | 'i16' | 'u32' | 'i32' | 'u64' | 'i64';

export type TailItem =
  | { readonly kind: 'scalar'; readonly type: ScalarType }
  | { readonly kind: 'bytes' }
  | { readonly kind: 'str' }
  | { readonly kind: 'struct'; readonly struct: StructDesc };

export type FieldDesc =
  | { readonly name: string; readonly kind: 'scalar'; readonly type: ScalarType }
  | { readonly name: string; readonly kind: 'bytes'; readonly size: number }
  | { readonly name: string; readonly kind: 'str'; readonly size: number }
  | { readonly name: string; readonly kind: 'struct'; readonly struct: StructDesc }
  | { readonly name: string; readonly kind: 'tail'; readonly item: TailItem };

export interface StructDesc {
  readonly name: string;
  /** Fixed bytes: everything except the tail. */
  readonly size: number;
  readonly fields: readonly FieldDesc[];
}

export class ProtocolError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'ProtocolError';
  }
}

const SCALAR_BYTES: Readonly<Record<ScalarType, number>> = {
  u8: 1, i8: 1, u16: 2, i16: 2, u32: 4, i32: 4, u64: 8, i64: 8,
};

export function scalarBytes(type: ScalarType): number {
  return SCALAR_BYTES[type];
}

export function tailField(desc: StructDesc): Extract<FieldDesc, { kind: 'tail' }> | undefined {
  const last = desc.fields[desc.fields.length - 1];
  return last !== undefined && last.kind === 'tail' ? last : undefined;
}

export function tailItemBytes(item: TailItem): number {
  switch (item.kind) {
    case 'scalar':
      return SCALAR_BYTES[item.type];
    case 'struct':
      return item.struct.size;
    default:
      return 1;
  }
}

const utf8Encoder = new TextEncoder();
const utf8Decoder = new TextDecoder('utf-8');

interface Cursor {
  readonly bytes: Uint8Array;
  readonly view: DataView;
  pos: number;
}

function cursor(bytes: Uint8Array): Cursor {
  return { bytes, view: new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength), pos: 0 };
}

// ---- encoding ---------------------------------------------------------------

function intIn(x: unknown, lo: number, hi: number, path: string): number {
  if (typeof x !== 'number' || !Number.isInteger(x) || x < lo || x > hi) {
    throw new ProtocolError(`${path}: ${String(x)} is not an integer in ${lo}..${hi}`);
  }
  return x;
}

function bigIn(x: unknown, lo: bigint, hi: bigint, path: string): bigint {
  if (typeof x !== 'bigint' || x < lo || x > hi) {
    throw new ProtocolError(`${path}: ${String(x)} is not a bigint in ${lo}..${hi}`);
  }
  return x;
}

function asBytes(x: unknown, path: string): Uint8Array {
  if (!(x instanceof Uint8Array)) throw new ProtocolError(`${path}: expected a Uint8Array`);
  return x;
}

function asString(x: unknown, path: string): string {
  if (typeof x !== 'string') throw new ProtocolError(`${path}: expected a string`);
  return x;
}

function asArray(x: unknown, path: string): readonly unknown[] {
  if (!Array.isArray(x)) throw new ProtocolError(`${path}: expected an array`);
  return x;
}

function asRecord(x: unknown, path: string): Record<string, unknown> {
  if (typeof x !== 'object' || x === null || Array.isArray(x)) {
    throw new ProtocolError(`${path}: expected an object`);
  }
  return x as Record<string, unknown>;
}

function writeScalar(c: Cursor, type: ScalarType, x: unknown, path: string): void {
  const v = c.view;
  switch (type) {
    case 'u8': v.setUint8(c.pos, intIn(x, 0, 0xff, path)); break;
    case 'i8': v.setInt8(c.pos, intIn(x, -0x80, 0x7f, path)); break;
    case 'u16': v.setUint16(c.pos, intIn(x, 0, 0xffff, path), true); break;
    case 'i16': v.setInt16(c.pos, intIn(x, -0x8000, 0x7fff, path), true); break;
    case 'u32': v.setUint32(c.pos, intIn(x, 0, 0xffffffff, path), true); break;
    case 'i32': v.setInt32(c.pos, intIn(x, -0x80000000, 0x7fffffff, path), true); break;
    case 'u64': v.setBigUint64(c.pos, bigIn(x, 0n, (1n << 64n) - 1n, path), true); break;
    case 'i64': v.setBigInt64(c.pos, bigIn(x, -(1n << 63n), (1n << 63n) - 1n, path), true); break;
  }
  c.pos += SCALAR_BYTES[type];
}

function writeBytes(c: Cursor, data: Uint8Array): void {
  c.bytes.set(data, c.pos);
  c.pos += data.length;
}

function writeFields(c: Cursor, desc: StructDesc, value: Record<string, unknown>, path: string): void {
  for (const f of desc.fields) {
    const p = `${path}.${f.name}`;
    const x = value[f.name];
    switch (f.kind) {
      case 'scalar':
        writeScalar(c, f.type, x, p);
        break;
      case 'bytes': {
        const b = asBytes(x, p);
        if (b.length !== f.size) throw new ProtocolError(`${p}: needs exactly ${f.size} bytes, got ${b.length}`);
        writeBytes(c, b);
        break;
      }
      case 'str': {
        const b = utf8Encoder.encode(asString(x, p));
        if (b.length > f.size) throw new ProtocolError(`${p}: ${b.length} UTF-8 bytes do not fit str[${f.size}]`);
        writeBytes(c, b);
        c.pos += f.size - b.length; // the buffer is zeroed: padding is already there
        break;
      }
      case 'struct':
        writeFields(c, f.struct, asRecord(x, p), p);
        break;
      case 'tail':
        writeTail(c, f.item, x, p);
        break;
    }
  }
}

function writeTail(c: Cursor, item: TailItem, x: unknown, path: string): void {
  switch (item.kind) {
    case 'bytes':
      writeBytes(c, asBytes(x, path));
      break;
    case 'str':
      writeBytes(c, utf8Encoder.encode(asString(x, path)));
      break;
    case 'scalar':
      asArray(x, path).forEach((e, i) => writeScalar(c, item.type, e, `${path}[${i}]`));
      break;
    case 'struct':
      asArray(x, path).forEach((e, i) => writeFields(c, item.struct, asRecord(e, `${path}[${i}]`), `${path}[${i}]`));
      break;
  }
}

function tailLength(item: TailItem, x: unknown, path: string): number {
  switch (item.kind) {
    case 'bytes':
      return asBytes(x, path).length;
    case 'str':
      return utf8Encoder.encode(asString(x, path)).length;
    default:
      return asArray(x, path).length * tailItemBytes(item);
  }
}

export function encodeStruct<T extends object>(desc: StructDesc, value: T): Uint8Array {
  const v = asRecord(value, desc.name);
  const tail = tailField(desc);
  const length = desc.size + (tail ? tailLength(tail.item, v[tail.name], `${desc.name}.${tail.name}`) : 0);
  const c = cursor(new Uint8Array(length));
  writeFields(c, desc, v, desc.name);
  if (c.pos !== length) {
    throw new ProtocolError(`${desc.name}: wrote ${c.pos} of ${length} bytes (descriptor size is wrong)`);
  }
  return c.bytes;
}

// ---- decoding ---------------------------------------------------------------

function readScalar(c: Cursor, type: ScalarType): number | bigint {
  const v = c.view;
  const at = c.pos;
  c.pos += SCALAR_BYTES[type];
  switch (type) {
    case 'u8': return v.getUint8(at);
    case 'i8': return v.getInt8(at);
    case 'u16': return v.getUint16(at, true);
    case 'i16': return v.getInt16(at, true);
    case 'u32': return v.getUint32(at, true);
    case 'i32': return v.getInt32(at, true);
    case 'u64': return v.getBigUint64(at, true);
    case 'i64': return v.getBigInt64(at, true);
  }
}

function readBytes(c: Cursor, n: number): Uint8Array {
  const b = c.bytes.slice(c.pos, c.pos + n);
  c.pos += n;
  return b;
}

function textUpToNul(b: Uint8Array): string {
  const end = b.indexOf(0);
  return utf8Decoder.decode(end < 0 ? b : b.subarray(0, end));
}

function readFields(c: Cursor, desc: StructDesc, tailBytes: number): Record<string, unknown> {
  const out: Record<string, unknown> = {};
  for (const f of desc.fields) {
    switch (f.kind) {
      case 'scalar':
        out[f.name] = readScalar(c, f.type);
        break;
      case 'bytes':
        out[f.name] = readBytes(c, f.size);
        break;
      case 'str':
        out[f.name] = textUpToNul(readBytes(c, f.size));
        break;
      case 'struct':
        out[f.name] = readFields(c, f.struct, 0);
        break;
      case 'tail':
        out[f.name] = readTail(c, f.item, tailBytes);
        break;
    }
  }
  return out;
}

function readTail(c: Cursor, item: TailItem, n: number): unknown {
  switch (item.kind) {
    case 'bytes':
      return readBytes(c, n);
    case 'str':
      return textUpToNul(readBytes(c, n));
    case 'scalar':
      return Array.from({ length: n / SCALAR_BYTES[item.type] }, () => readScalar(c, item.type));
    case 'struct':
      return Array.from({ length: n / item.struct.size }, () => readFields(c, item.struct, 0));
  }
}

export function decodeStruct(desc: StructDesc, bytes: Uint8Array): Record<string, unknown> {
  const tail = tailField(desc);
  const extra = bytes.length - desc.size;
  if (extra < 0 || (tail === undefined && extra !== 0)) {
    throw new ProtocolError(
      `${desc.name}: ${bytes.length} bytes, expected ${tail ? 'at least ' : ''}${desc.size}`,
    );
  }
  if (tail !== undefined && extra % tailItemBytes(tail.item) !== 0) {
    throw new ProtocolError(`${desc.name}: tail of ${extra} bytes is not a whole number of items`);
  }
  return readFields(cursor(bytes), desc, extra);
}
