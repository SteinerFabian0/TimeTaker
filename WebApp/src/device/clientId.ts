/*
 * The web app's clientId: 16 random bytes per browser profile, the BLE bind
 * key (SYSTEM_ARCHITECTURE §9.5). Kept in an injectable key-value storage:
 * localStorage in the browser, memory in tests and the CLI.
 */

import { fromHex, toHex } from '../util/bytes.ts';

export interface KeyValueStorage {
  getItem(key: string): string | null;
  setItem(key: string, value: string): void;
  removeItem?(key: string): void;
}

export function memoryStorage(initial: Record<string, string> = {}): KeyValueStorage {
  const m = new Map(Object.entries(initial));
  return {
    getItem: (k) => m.get(k) ?? null,
    setItem: (k, v) => { m.set(k, v); },
    removeItem: (k) => { m.delete(k); },
  };
}

/** localStorage when it exists and works (not in a sandboxed iframe or Node), else memory. */
export function browserStorage(): KeyValueStorage {
  try {
    const ls = (globalThis as { localStorage?: KeyValueStorage }).localStorage;
    if (ls) {
      ls.setItem('timetaker.probe', '1');
      ls.removeItem?.('timetaker.probe');
      return ls;
    }
  } catch {
    /* blocked: fall through */
  }
  return memoryStorage();
}

export const CLIENT_ID_KEY = 'timetaker.clientId';
export const CLIENT_NAME_KEY = 'timetaker.clientName';

function randomBytes(n: number): Uint8Array {
  const b = new Uint8Array(n);
  globalThis.crypto.getRandomValues(b);
  return b;
}

/** The stored clientId, or a new random one (stored). Never all-zero (zero means "clear" in BIND_REMOVE). */
export function loadClientId(storage: KeyValueStorage, random: (n: number) => Uint8Array = randomBytes): Uint8Array {
  const stored = storage.getItem(CLIENT_ID_KEY);
  if (stored && /^[0-9a-f]{32}$/.test(stored) && !/^0+$/.test(stored)) return fromHex(stored);
  let id = random(16);
  while (id.every((x) => x === 0)) id = random(16);
  storage.setItem(CLIENT_ID_KEY, toHex(id));
  return id;
}

/** The name shown in the bind list (max 24 bytes UTF-8 on the wire). */
export function loadClientName(storage: KeyValueStorage, fallback = 'TimeTaker app'): string {
  return storage.getItem(CLIENT_NAME_KEY) ?? fallback;
}

export function saveClientName(storage: KeyValueStorage, name: string): void {
  storage.setItem(CLIENT_NAME_KEY, truncateUtf8(name, 24));
}

export function truncateUtf8(s: string, maxBytes: number): string {
  const enc = new TextEncoder();
  let out = '';
  for (const ch of s) {
    if (enc.encode(out + ch).length > maxBytes) break;
    out += ch;
  }
  return out;
}
