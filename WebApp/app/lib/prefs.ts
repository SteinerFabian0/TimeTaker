// App preferences (per browser). localStorage may be missing or throw
// (private window, blocked storage): every access is guarded and the app
// works with the defaults.
import { signal, effect } from '@preact/signals';

export type Lang = 'en' | 'de';
export type Theme = 'dark' | 'light' | 'system';

function read(key: string): string | null {
  try { return localStorage.getItem(key); } catch { return null; }
}
function write(key: string, value: string): void {
  try { localStorage.setItem(key, value); } catch { /* not persisted */ }
}

const savedLang = read('tt.lang');
const savedTheme = read('tt.theme');

export const lang = signal<Lang>(savedLang === 'de' ? 'de' : 'en');
export const theme = signal<Theme>(
  savedTheme === 'light' || savedTheme === 'system' ? savedTheme : 'dark');

effect(() => {
  write('tt.lang', lang.value);
  document.documentElement.lang = lang.value;
});

effect(() => {
  write('tt.theme', theme.value);
  const root = document.documentElement;
  if (theme.value === 'system') delete root.dataset.theme;
  else root.dataset.theme = theme.value;
});

/**
 * Developer tools (Settings): e.g. a button on the race page that acts like a
 * car crossing the line, to try the pages without a time taker. On by
 * default during development; set DEV_TOOLS_DEFAULT to false before the first release.
 */
export const DEV_TOOLS_DEFAULT = true;
const savedDev = read('tt.devTools');
export const devTools = signal<boolean>(savedDev === null ? DEV_TOOLS_DEFAULT : savedDev === '1');
effect(() => write('tt.devTools', devTools.value ? '1' : '0'));

// Duration estimates (GUI_SPEC §4.2, §11): average lap and changeover, ms.
function readMs(key: string, fallback: number, min: number, max: number): number {
  const v = Number(read(key));
  return read(key) !== null && Number.isFinite(v) && v >= min && v <= max ? v : fallback;
}
export const ESTIMATE_LIMITS = { avgLapMs: { min: 5_000, max: 120_000 }, changeoverMs: { min: 15_000, max: 600_000 } } as const;
export const avgLapMs = signal(readMs('tt.avgLapMs', 27_000, ESTIMATE_LIMITS.avgLapMs.min, ESTIMATE_LIMITS.avgLapMs.max));
export const changeoverMs = signal(readMs('tt.changeoverMs', 60_000, ESTIMATE_LIMITS.changeoverMs.min, ESTIMATE_LIMITS.changeoverMs.max));
effect(() => write('tt.avgLapMs', String(avgLapMs.value)));
effect(() => write('tt.changeoverMs', String(changeoverMs.value)));
