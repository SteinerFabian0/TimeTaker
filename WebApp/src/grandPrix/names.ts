/*
 * Driver names for a Grand Prix (GUI_SPEC §6.1). Pure functions, DOM-free.
 *
 * A name is letters and single spaces only (any alphabet, so "Jürgen" and
 * "Zoë" are fine; no digits, no punctuation), at most NAME_MAX long. The one
 * exception is the number the app adds itself: a name that is already taken
 * gets " 2" (or the next free number), and the host can click in and change
 * it to, say, the initial of the last name.
 */

import { uniqueName } from '../presets/preset.ts';

export const NAME_MAX = 12;

/** What may stay in a name while typing: letters and single spaces, no leading space. */
export function typedName(raw: string): string {
  return raw.replace(/[^\p{L} ]/gu, '').replace(/ {2,}/g, ' ').replace(/^ /, '').slice(0, NAME_MAX);
}

/** A name as it is kept: typed rules, trimmed. '' when nothing is left. */
export function cleanName(raw: string): string {
  return typedName(raw).trim();
}

/**
 * The name to keep for a driver: cleaned, and with " 2", " 3" … added when
 * another driver already has it (case-insensitive).
 */
export function keepName(raw: string, others: Iterable<string>): string {
  const name = cleanName(raw);
  if (!name) return '';
  const taken = [...others];
  const kept = uniqueName(name, taken);
  if (kept.length <= NAME_MAX) return kept;
  // The number does not fit after a long name: shorten the name to make room for it.
  const used = new Set(taken.map(x => x.trim().toLocaleLowerCase()));
  for (let n = 2; ; n++) {
    const suffix = ` ${n}`;
    const c = name.slice(0, NAME_MAX - suffix.length).trimEnd() + suffix;
    if (!used.has(c.toLocaleLowerCase())) return c;
  }
}

/**
 * Three-letter codes for the race page's position table, F1 style ("Anna
 * Berger" → ANN). Unique within the list: names that would share a code try
 * other letters (first letter + start of the last name, then the name's last
 * letter, then a number the name carries), and a number as the last resort.
 */
export function shortCodes(names: readonly string[]): string[] {
  const up = (s: string) => s.toUpperCase().replace(/[^\p{L}\p{N}]/gu, '');
  const candidates = names.map(n => {
    const words = n.trim().split(/\s+/).filter(Boolean);
    const letters = up(words.filter(w => !/^\d+$/.test(w)).join(''));
    const first = up(words[0] ?? '');
    const lastWord = up(words.filter(w => !/^\d+$/.test(w)).at(-1) ?? '');
    const num = words.find(w => /^\d+$/.test(w));
    const out = [(first.length >= 3 ? first : letters).slice(0, 3) || '???'];
    if (words.length > 1 && lastWord !== first) out.push((first[0] ?? '') + lastWord.slice(0, 2));
    if (letters.length > 3) out.push(letters.slice(0, 2) + letters.at(-1));
    if (num) out.push(letters.slice(0, 3 - Math.min(2, num.length)) + num.slice(-2));
    return out.map(c => c.slice(0, 3));
  });
  const codes: (string | null)[] = names.map(() => null);
  for (let level = 0; level < 4; level++) {
    const open = codes.map((c, i) => c === null ? i : -1).filter(i => i >= 0);
    if (!open.length) break;
    const tryCode = new Map(open.map(i => [i, candidates[i][Math.min(level, candidates[i].length - 1)]]));
    for (const i of open) {
      const c = tryCode.get(i)!;
      const clash = codes.includes(c) || open.some(j => j !== i && tryCode.get(j) === c);
      if (!clash) codes[i] = c;
    }
  }
  // Last resort: two letters and a number.
  return codes.map((c, i) => {
    if (c !== null) return c;
    for (let n = 2; ; n++) {
      const k = (candidates[i][0].slice(0, 2) + n).slice(0, 3);
      if (!codes.includes(k)) { codes[i] = k; return k; }
    }
  });
}
