/*
 * Packets: a frame's raw bytes split into the 4-byte header and the payload.
 * The device session (later) builds on these two functions only.
 */

import { frameEncode } from './codec.ts';
import { decodeHeader, encodeHeader, PROTO } from './generated.ts';
import type { Header } from './generated.ts';
import { ProtocolError } from './wire.ts';

export interface Packet {
  readonly header: Header;
  readonly payload: Uint8Array;
}

/** Header + payload → one frame on the wire, delimiter included. */
export function encodePacket(header: Header, payload: Uint8Array = new Uint8Array(0)): Uint8Array {
  if (payload.length > PROTO.maxPayload) {
    throw new ProtocolError(`payload of ${payload.length} bytes exceeds ${PROTO.maxPayload}`);
  }
  return frameEncode(encodeHeader(header), payload);
}

/** A decoded frame (FrameDecoder.frame) → header + payload. */
export function decodePacket(raw: Uint8Array): Packet {
  if (raw.length < PROTO.headerBytes) {
    throw new ProtocolError(`frame of ${raw.length} bytes has no room for a header`);
  }
  if (raw.length - PROTO.headerBytes > PROTO.maxPayload) {
    throw new ProtocolError(`payload of ${raw.length - PROTO.headerBytes} bytes exceeds ${PROTO.maxPayload}`);
  }
  return {
    header: decodeHeader(raw.subarray(0, PROTO.headerBytes)),
    payload: raw.subarray(PROTO.headerBytes),
  };
}
