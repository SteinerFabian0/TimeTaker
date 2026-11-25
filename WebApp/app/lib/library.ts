// Export and import of the library (GUI_SPEC §11 Data): the glue between the
// three stores (race presets, tournament templates, car presets) and the
// DOM-free file format in src/store/libraryFile.ts.
import { computed } from '@preact/signals';
import { isAtDefaults } from '../../src/presets/preset.ts';
import { isTemplateAtDefaults } from '../../src/tournaments/template.ts';
import {
  libraryFileName, makeLibraryFile, parseLibraryFile, planImport, type ImportPlan, type Library, type ParseError,
} from '../../src/store/libraryFile.ts';
import { presets, presetsLoaded, addPresets, presetNames } from './presets.ts';
import { templates, templatesLoaded, addTemplates, templateNames } from './tournaments.ts';
import { carPresets, carPresetsLoaded, addCarPresets } from './carPresets.ts';
import { APP_VERSION } from './version.ts';

/** All three stores have loaded (before that, an export would miss items and an import could clash). */
export const libraryReady = computed(() => presetsLoaded.value && templatesLoaded.value && carPresetsLoaded.value);

const current = (): Library => ({ presets: presets.value, templates: templates.value, carPresets: carPresets.value });

/** What an export holds: own items, edited shipped ones, every car preset. */
export function exportCounts(): { presets: number; templates: number; carPresets: number } {
  const l = exportable();
  return { presets: l.presets.length, templates: l.templates.length, carPresets: l.carPresets.length };
}

function exportable(): Library {
  return {
    presets: presets.value.filter(p => !p.builtIn || !isAtDefaults(p)),
    templates: templates.value.filter(x => !x.builtIn || !isTemplateAtDefaults(x)),
    carPresets: carPresets.value,
  };
}

/** Downloads the library as a JSON file. */
export function exportLibrary(): void {
  const now = new Date();
  const blob = new Blob([JSON.stringify(makeLibraryFile(exportable(), now, APP_VERSION), null, 2)], { type: 'application/json' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = libraryFileName(now);
  document.body.append(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

const uid = () => crypto.randomUUID?.() ?? Date.now().toString(36) + Math.random().toString(36).slice(2);
const PREFIX = { preset: 'p-', template: 't-', car: 'c-' } as const;

export type ReadResult = { ok: true; plan: ImportPlan } | { ok: false; error: ParseError };

/** Reads a chosen file and works out what importing it would add (nothing changes yet). */
export async function readImport(file: File): Promise<ReadResult> {
  const parsed = parseLibraryFile(await file.text());
  if (!parsed.ok) return parsed;
  return {
    ok: true,
    plan: planImport(parsed.file, current(), {
      newId: kind => PREFIX[kind] + uid(),
      takenPresetNames: presetNames(),
      takenTemplateNames: templateNames(),
    }),
  };
}

/** Adds a plan's items to the library. */
export function applyImport(plan: ImportPlan): void {
  addPresets(plan.presets);
  addTemplates(plan.templates);
  addCarPresets(plan.carPresets);
}
