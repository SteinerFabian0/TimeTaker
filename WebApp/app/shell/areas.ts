// The areas (tabs) of the app, in sidebar order (GUI_SPEC §2.2), and the
// hash router: #/grand-prix etc. The hash keeps the area across reloads.
import { signal } from '@preact/signals';
import type { Key } from '../i18n/i18n.ts';

export type Group = 'drive' | 'plan' | 'system';

export interface Area {
  id: string;            // URL slug
  icon: string;
  group: Group;
  title: Key;
  subtitle: Key;
  needsPowerPack?: boolean;
  /** The icon's colour shows a state: the device link, or the Power Pack. */
  indicator?: 'link' | 'powerPack';
}

export const AREAS: readonly Area[] = [
  { id: 'grand-prix',   icon: 'grandPrix',   group: 'drive',  title: 'area.grandPrix',   subtitle: 'sub.grandPrix' },
  { id: 'time-attack',  icon: 'timeAttack',  group: 'drive',  title: 'area.timeAttack',  subtitle: 'sub.timeAttack' },
  { id: 'training',     icon: 'training',    group: 'drive',  title: 'area.training',    subtitle: 'sub.training' },
  { id: 'tournaments',  icon: 'tournaments', group: 'plan',   title: 'area.tournaments', subtitle: 'sub.tournaments' },
  { id: 'race-presets', icon: 'racePresets', group: 'plan',   title: 'area.racePresets', subtitle: 'sub.racePresets' },
  { id: 'status',       icon: 'status',      group: 'system', title: 'area.status',      subtitle: 'sub.status', indicator: 'link' },
  { id: 'track-setup',  icon: 'trackSetup',  group: 'system', title: 'area.trackSetup',  subtitle: 'sub.trackSetup', needsPowerPack: true, indicator: 'powerPack' },
  { id: 'settings',     icon: 'settings',    group: 'system', title: 'area.settings',    subtitle: 'sub.settings' },
];

export const GROUPS: readonly { id: Group; label: Key }[] = [
  { id: 'drive', label: 'nav.group.drive' },
  { id: 'plan', label: 'nav.group.plan' },
  { id: 'system', label: 'nav.group.system' },
];

const DEFAULT = AREAS[0].id;

function fromHash(): string {
  const id = location.hash.replace(/^#\/?/, '');
  return AREAS.some(a => a.id === id) ? id : DEFAULT;
}

export const currentArea = signal<string>(fromHash());

addEventListener('hashchange', () => { currentArea.value = fromHash(); });

export function navigate(id: string): void {
  if (location.hash !== `#/${id}`) location.hash = `#/${id}`;
  currentArea.value = id;
}
