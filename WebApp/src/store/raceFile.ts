/*
 * Race files: one race as a self-contained JSON document, for export, import,
 * sharing, and as the regression fixtures in test/fixtures/races/.
 *
 * Format "timetaker-race", version 1:
 *
 *   {
 *     "format": "timetaker-race",
 *     "version": 1,
 *     "protocol": "0.2",                 host protocol the events were decoded with
 *     "exportedAt": "2026-11-17T20:00:00.000Z",   informational
 *     "race":   { "id", "config": RaceConfig, "from": {bootId, evSeq} | null, "createdAt" },
 *     "boots":  [ bootId, ... ],          in log order (the store's ordinals)
 *     "events": [ { "bootId", "evSeq", "name", "msgId", "value" }, ... ]   log order
 *     "actions": [ HostAction, ... ]      by seq
 *     "expect": { ... }                   optional: fixtures only, see test/raceFixtures.test.ts
 *   }
 *
 * Values are the decoded event fields, JSON-safe:
 *   u64/i64 → decimal string ("81234567"), u8[N] → lowercase hex string,
 *   str → string, nested structs → objects, tails → arrays (u8[] → hex).
 * An unknown event keeps "value": null (a newer firmware's event: ignored).
 * Readers accept any version <= their own and refuse newer ones.
 */

import { MESSAGE_BY_ID, PROTO } from '../protocol/generated.ts';
import type { FieldDesc, StructDesc, TailItem } from '../protocol/wire.ts';
import { fromHex, toHex } from '../util/bytes.ts';
import type { HostAction, LogPos, RaceConfig, RaceInputEvent } from '../race/types.ts';

export const RACE_FILE_FORMAT = 'timetaker-race';
export const RACE_FILE_VERSION = 1;

export interface RaceFileEvent {
  bootId: number;
  evSeq: number;
  name: string;
  msgId: number;
  value: Record<string, unknown> | null;
}

export interface RaceFile {
  format: typeof RACE_FILE_FORMAT;
  version: number;
  protocol: string;
  exportedAt: string;
  race: { id: string; config: RaceConfig; from: LogPos | null; createdAt: string };
  boots: number[];
  events: RaceFileEvent[];
  actions: HostAction[];
  expect?: unknown;
}

const is64 = (t: string): boolean => t === 'u64' || t === 'i64';

function itemToJson(item: TailItem, v: unknown): unknown {
  switch (item.kind) {
    case 'bytes': return toHex(v as Uint8Array);
    case 'str': return v;
    case 'scalar': return (v as unknown[]).map((x) => (is64(item.type) ? String(x) : x));
    case 'struct': return (v as Record<string, unknown>[]).map((x) => valueToJson(item.struct, x));
  }
}

function fieldToJson(f: FieldDesc, v: unknown): unknown {
  switch (f.kind) {
    case 'scalar': return is64(f.type) ? String(v) : v;
    case 'bytes': return toHex(v as Uint8Array);
    case 'str': return v;
    case 'struct': return valueToJson(f.struct, v as Record<string, unknown>);
    case 'tail': return itemToJson(f.item, v);
  }
}

export function valueToJson(desc: StructDesc, value: Record<string, unknown>): Record<string, unknown> {
  const out: Record<string, unknown> = {};
  for (const f of desc.fields) if (f.name in value) out[f.name] = fieldToJson(f, value[f.name]);
  return out;
}

function itemFromJson(item: TailItem, j: unknown): unknown {
  switch (item.kind) {
    case 'bytes': return fromHex(j as string);
    case 'str': return j;
    case 'scalar': return (j as unknown[]).map((x) => (is64(item.type) ? BigInt(x as string) : x));
    case 'struct': return (j as Record<string, unknown>[]).map((x) => valueFromJson(item.struct, x));
  }
}

function fieldFromJson(f: FieldDesc, j: unknown): unknown {
  switch (f.kind) {
    case 'scalar': return is64(f.type) ? BigInt(j as string) : j;
    case 'bytes': return fromHex(j as string);
    case 'str': return j;
    case 'struct': return valueFromJson(f.struct, j as Record<string, unknown>);
    case 'tail': return itemFromJson(f.item, j);
  }
}

export function valueFromJson(desc: StructDesc, j: Record<string, unknown>): Record<string, unknown> {
  const out: Record<string, unknown> = {};
  for (const f of desc.fields) if (f.name in j) out[f.name] = fieldFromJson(f, j[f.name]);
  return out;
}

export function eventToJson(e: RaceInputEvent): RaceFileEvent {
  const msg = MESSAGE_BY_ID.get(e.msgId);
  return {
    bootId: e.bootId, evSeq: e.evSeq, msgId: e.msgId, name: e.name || msg?.name || `EVT_0x${e.msgId.toString(16)}`,
    value: e.value && msg?.body ? valueToJson(msg.body, e.value) : null,
  };
}

export function eventFromJson(j: RaceFileEvent): RaceInputEvent {
  const msg = MESSAGE_BY_ID.get(j.msgId);
  return {
    bootId: j.bootId, evSeq: j.evSeq, msgId: j.msgId, name: j.name,
    value: j.value && msg?.body ? valueFromJson(msg.body, j.value) : null,
  };
}

export interface ExportInput {
  race: { id: string; config: RaceConfig; from: LogPos | null; createdAt: string };
  events: readonly RaceInputEvent[];
  actions: readonly HostAction[];
  expect?: unknown;
  exportedAt?: string;
}

export function exportRace(x: ExportInput): RaceFile {
  const boots: number[] = [];
  for (const e of x.events) if (!boots.includes(e.bootId)) boots.push(e.bootId);
  const file: RaceFile = {
    format: RACE_FILE_FORMAT, version: RACE_FILE_VERSION, protocol: `${PROTO.major}.${PROTO.minor}`,
    exportedAt: x.exportedAt ?? new Date().toISOString(),
    race: x.race, boots, events: x.events.map(eventToJson), actions: [...x.actions].sort((a, b) => a.seq - b.seq),
  };
  if (x.expect !== undefined) file.expect = x.expect;
  return file;
}

export function stringifyRace(f: RaceFile): string {
  return JSON.stringify(f, null, 1) + '\n';
}

export interface ImportedRace {
  race: RaceFile['race'];
  events: RaceInputEvent[];
  actions: HostAction[];
  expect?: unknown;
}

export function parseRace(text: string): ImportedRace {
  const j = JSON.parse(text) as Partial<RaceFile>;
  if (j.format !== RACE_FILE_FORMAT) throw new Error('not a TimeTaker race file');
  if (typeof j.version !== 'number' || j.version > RACE_FILE_VERSION) throw new Error(`race file version ${String(j.version)} is newer than this app (${RACE_FILE_VERSION})`);
  if (!j.race || !Array.isArray(j.events) || !Array.isArray(j.actions)) throw new Error('race file incomplete');
  const order = new Map((j.boots ?? []).map((b, i) => [b, i]));
  const events = j.events.map(eventFromJson);
  events.sort((a, b) => (order.get(a.bootId) ?? 0) - (order.get(b.bootId) ?? 0) || a.evSeq - b.evSeq);
  return { race: j.race, events, actions: j.actions, expect: j.expect };
}
