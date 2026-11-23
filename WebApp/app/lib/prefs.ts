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
