// Browser features that keep an evening safe:
//
//  - Persistent storage: without it Chrome may clear the presets, templates
//    and races (IndexedDB) when the disk runs low. Asked once at start;
//    Chrome grants it without a prompt to an installed app or a site the
//    user uses a lot, and may refuse otherwise (Settings → About shows it).
//  - Screen wake lock while a Grand Prix runs: a laptop that dims and then
//    sleeps mid-event drops the USB/Bluetooth link. The browser releases the
//    lock whenever the page is hidden, so it is taken again on return.
import { signal, effect } from '@preact/signals';
import { gpStarted } from './grandPrix.ts';
import { raceSessionActive } from '../shell/connection.ts';

/** true: kept; false: may be cleared; null: unknown (not supported, or not asked yet). */
export const storagePersisted = signal<boolean | null>(null);

void (async () => {
  try {
    const s = navigator.storage;
    if (!s?.persisted) return;
    storagePersisted.value = (await s.persisted()) || (await s.persist?.()) || false;
  } catch { /* unknown */ }
})();

interface WakeLockSentinelLike { released: boolean; release(): Promise<void>; addEventListener(t: 'release', f: () => void): void }
interface WakeLockLike { request(type: 'screen'): Promise<WakeLockSentinelLike> }

export const canKeepAwake = typeof navigator !== 'undefined' && 'wakeLock' in navigator;
/** The screen is held awake right now. */
export const keptAwake = signal(false);

/** While true the screen stays on: a Grand Prix is under way, or a race session runs. */
const wanted = () => gpStarted.value || raceSessionActive.value;

let lock: WakeLockSentinelLike | null = null;
let asking = false;

async function sync(): Promise<void> {
  const want = wanted() && document.visibilityState === 'visible';
  if (want && !lock && !asking && canKeepAwake) {
    asking = true;
    try {
      const l = await (navigator as Navigator & { wakeLock: WakeLockLike }).wakeLock.request('screen');
      lock = l;
      keptAwake.value = true;
      l.addEventListener('release', () => { if (lock === l) { lock = null; keptAwake.value = false; } });
      if (!wanted()) void sync();                   // ended while asking
    } catch { /* refused (e.g. battery saver): the screen may sleep */ }
    finally { asking = false; }
  } else if (!want && lock) {
    const l = lock;
    lock = null;
    keptAwake.value = false;
    await l.release().catch(() => {});
  }
}

effect(() => { void wanted(); void sync(); });
document.addEventListener('visibilitychange', () => { void sync(); });
