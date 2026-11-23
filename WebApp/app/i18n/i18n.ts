// Translations. Every UI string goes through t(key); English is the source.
// A key missing in German falls back to English, a key missing everywhere
// shows the key itself (easy to spot).
import { lang } from '../lib/prefs.ts';
import { en } from './en.ts';
import { de } from './de.ts';

export type Key = keyof typeof en;

const tables: Record<string, Partial<Record<Key, string>>> = { en, de };

/** Reads lang.value, so a component using t() re-renders on a language change. */
export function t(key: Key, vars?: Record<string, string | number>): string {
  let s = tables[lang.value]?.[key] ?? en[key] ?? key;
  if (vars) for (const [k, v] of Object.entries(vars)) s = s.replaceAll(`{${k}}`, String(v));
  return s;
}
