/*
 * The web app's protocol code against the golden bytes.
 *
 * protocol/golden/golden.json comes from gen_protocol.py's reference packer and
 * encoder. The firmware's C structs and frame_codec are held to the same bytes
 * (ControlModule/.../test/host_protocol), so passing here means TypeScript and
 * C agree on every example in host_protocol.toml.
 *
 *   cd WebApp && npm test        (Node >= 22.18: runs .ts directly)
 */

import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { describe, test } from 'node:test';

import { crc16, FrameDecoder, frameEncode, FrameResult, wireBound } from '../src/protocol/codec.ts';
import { decodePacket, encodePacket } from '../src/protocol/packet.ts';
import type { Packet } from '../src/protocol/packet.ts';
import {
  decodeEvtCrossing, encodeHelloReq, HELLO_REQ, MESSAGES, PROTO, responseStatus, Status, statusName, STRUCTS,
} from '../src/protocol/generated.ts';
import { decodeStruct, encodeStruct, ProtocolError, scalarBytes } from '../src/protocol/wire.ts';
import type { FieldDesc, StructDesc, TailItem } from '../src/protocol/wire.ts';

interface GoldenCodec { name: string; raw: string; crc: number; wire: string }
interface GoldenMsg {
  name: string;
  struct: string;
  header: { msgId: number; flags: number; tag: number };
  statusOnly: boolean;
  value: Record<string, unknown>;
  payload: string;
  wire: string;
}
interface Golden { protocol: { major: number; minor: number }; codec: GoldenCodec[]; messages: GoldenMsg[] }

const golden = JSON.parse(
  readFileSync(new URL('../../protocol/golden/golden.json', import.meta.url), 'utf8'),
) as Golden;

const hex = (b: Uint8Array): string => Buffer.from(b).toString('hex');
const unhex = (s: string): Uint8Array => new Uint8Array(Buffer.from(s, 'hex'));

// golden.json writes 64-bit values as decimal strings and bytes as hex; these
// convert between that form and the values wire.ts works with.
function itemFromJson(item: TailItem, j: unknown): unknown {
  switch (item.kind) {
    case 'bytes': return unhex(j as string);
    case 'str': return j;
    case 'scalar': return (j as unknown[]).map((x) => (scalarBytes(item.type) === 8 ? BigInt(x as string) : x));
    case 'struct': return (j as Record<string, unknown>[]).map((x) => fromJson(item.struct, x));
  }
}

function fieldFromJson(f: FieldDesc, j: unknown): unknown {
  switch (f.kind) {
    case 'scalar': return scalarBytes(f.type) === 8 ? BigInt(j as string) : j;
    case 'bytes': return unhex(j as string);
    case 'str': return j;
    case 'struct': return fromJson(f.struct, j as Record<string, unknown>);
    case 'tail': return itemFromJson(f.item, j);
  }
}

function fromJson(desc: StructDesc, j: Record<string, unknown>): Record<string, unknown> {
  return Object.fromEntries(desc.fields.map((f) => [f.name, fieldFromJson(f, j[f.name])]));
}

function toJson(x: unknown): unknown {
  if (typeof x === 'bigint') return x.toString();
  if (x instanceof Uint8Array) return hex(x);
  if (Array.isArray(x)) return x.map(toJson);
  if (typeof x === 'object' && x !== null) {
    return Object.fromEntries(Object.entries(x).map(([k, v]) => [k, toJson(v)]));
  }
  return x;
}

describe('golden file', () => {
  test('matches the protocol version this code was generated for', () => {
    assert.deepEqual(golden.protocol, { major: PROTO.major, minor: PROTO.minor });
  });
});

describe('codec: reference vectors', () => {
  for (const v of golden.codec) {
    test(v.name, () => {
      const raw = unhex(v.raw);
      assert.equal(crc16(raw), v.crc, 'crc');
      assert.equal(hex(frameEncode(raw)), v.wire, 'encode');

      const dec = new FrameDecoder(1024);
      const frames: string[] = [];
      assert.deepEqual(dec.feed(unhex(v.wire), (f) => frames.push(hex(f))), []);
      assert.deepEqual(frames, [v.raw]);
    });
  }
});

describe('messages: reference examples', () => {
  for (const m of golden.messages) {
    test(m.name, () => {
      const desc = STRUCTS[m.struct];
      assert.ok(desc, `no descriptor ${m.struct}`);
      const payload = unhex(m.payload);

      if (m.statusOnly) {
        assert.equal(payload.length, 2);
        assert.equal(responseStatus(payload), m.value['status']);
        assert.notEqual(responseStatus(payload), Status.OK);
      } else {
        assert.equal(hex(encodeStruct(desc, fromJson(desc, m.value))), m.payload, 'encode');
        assert.deepEqual(toJson(decodeStruct(desc, payload)), m.value, 'decode');
      }

      assert.equal(hex(encodePacket(m.header, payload)), m.wire, 'frame');

      const dec = new FrameDecoder(PROTO.headerBytes + PROTO.maxPayload);
      const packets: Packet[] = [];
      dec.feed(unhex(m.wire), (raw) => packets.push(decodePacket(raw)));
      const [got] = packets;
      assert.ok(got !== undefined && packets.length === 1, 'exactly one frame decoded');
      assert.deepEqual(got.header, m.header);
      assert.equal(hex(got.payload), m.payload);
    });
  }

  test('every message with an example is covered', () => {
    const covered = new Set(golden.messages.map((m) => m.struct));
    for (const msg of MESSAGES) {
      const structs = [msg.req, msg.rsp, msg.body].filter((s): s is StructDesc => s !== undefined);
      assert.ok(structs.some((s) => covered.has(s.name)), `${msg.name} has no golden example`);
    }
  });
});

describe('codec: decoder rules', () => {
  const raw = new Uint8Array([0x01, 0x00, 0x22, 0x33, 0x00, 0x44]);

  const results = (dec: FrameDecoder, bytes: Uint8Array): FrameResult[] =>
    Array.from(bytes, (b) => dec.push(b)).filter((r) => r !== FrameResult.NONE);

  test('flipped bit -> ERR_CRC, then back in sync', () => {
    const dec = new FrameDecoder(64);
    const wire = frameEncode(raw);
    wire[3] = (wire[3] ?? 0) ^ 0x01;
    assert.deepEqual(results(dec, wire), [FrameResult.ERR_CRC]);
    assert.deepEqual(results(dec, frameEncode(raw)), [FrameResult.READY]);
  });

  test('truncated block -> ERR_COBS', () => {
    const dec = new FrameDecoder(64);
    const wire = frameEncode(raw);
    const cut = wire.slice(0, wire.length - 1);
    cut[cut.length - 1] = 0;
    assert.deepEqual(results(dec, cut), [FrameResult.ERR_COBS]);
  });

  test('one byte over the buffer -> ERR_OVERFLOW; exact fit -> READY', () => {
    const dec = new FrameDecoder(8);
    const nine = Uint8Array.from({ length: 9 }, (_, i) => i + 1);
    assert.deepEqual(results(dec, frameEncode(nine)), [FrameResult.ERR_OVERFLOW]);
    assert.deepEqual(results(dec, frameEncode(nine.subarray(0, 8))), [FrameResult.READY]);
  });

  test('CRC only, no raw byte -> ERR_SHORT; empty frames are silent', () => {
    const dec = new FrameDecoder(64);
    assert.deepEqual(results(dec, new Uint8Array([0x03, 0xff, 0xff, 0x00])), [FrameResult.ERR_SHORT]);
    assert.deepEqual(results(dec, new Uint8Array([0x00, 0x00, 0x00])), []);
  });

  test('round trips: every length to 1200 at mixed zero density stays within wireBound', () => {
    let seed = 0x12345678;
    const rnd = (): number => {
      seed ^= seed << 13; seed >>>= 0;
      seed ^= seed >>> 17;
      seed ^= seed << 5; seed >>>= 0;
      return seed;
    };
    const dec = new FrameDecoder(1200);
    for (let len = 1; len <= 1200; len++) {
      const data = Uint8Array.from({ length: len }, () => (rnd() % 3 === 0 ? 0 : 1 + (rnd() % 255)));
      const wire = frameEncode(data.subarray(0, len >> 1), data.subarray(len >> 1));
      assert.ok(wire.length <= wireBound(len));
      assert.equal(wire.indexOf(0), wire.length - 1);
      const out: Uint8Array[] = [];
      dec.feed(wire, (f) => out.push(f));
      assert.equal(out.length, 1);
      assert.equal(hex(out[0] ?? new Uint8Array(0)), hex(data));
    }
  });
});

describe('wire: rejections', () => {
  const hello = {
    protoMajor: 0, protoMinor: 1, clientKind: 1, reserved: 0,
    clientId: new Uint8Array(16), clientName: 'Laptop',
  };

  test('a valid HELLO encodes to its fixed size', () => {
    assert.equal(encodeHelloReq(hello).length, HELLO_REQ.size);
  });

  test('u8 out of range', () => {
    assert.throws(() => encodeHelloReq({ ...hello, protoMajor: 256 }), ProtocolError);
  });

  test('fixed bytes of the wrong length', () => {
    assert.throws(() => encodeHelloReq({ ...hello, clientId: new Uint8Array(15) }), ProtocolError);
  });

  test('string longer than its field (never truncated silently)', () => {
    assert.throws(() => encodeHelloReq({ ...hello, clientName: 'x'.repeat(25) }), ProtocolError);
  });

  test('64-bit field given as a number', () => {
    const desc = STRUCTS['PingRsp'];
    assert.ok(desc);
    assert.throws(() => encodeStruct(desc, { status: 0, hostMs: 1, sysUs: 5 }), ProtocolError);
  });

  test('payload of the wrong length', () => {
    assert.throws(() => decodeEvtCrossing(new Uint8Array(23)), ProtocolError);
    assert.throws(() => decodeEvtCrossing(new Uint8Array(25)), ProtocolError);
  });

  test('tail that is not a whole number of items', () => {
    const desc = STRUCTS['NodeListRsp'];
    assert.ok(desc);
    assert.throws(() => decodeStruct(desc, new Uint8Array(2 + 17)), ProtocolError);
  });

  test('status names resolve, unknown codes still print', () => {
    assert.equal(statusName(Status.ERR_NOT_BOUND), 'ERR_NOT_BOUND');
    assert.equal(statusName(-999), 'STATUS_-999');
  });
});
