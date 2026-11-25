/*
 * The library file (GUI_SPEC §11 Data): export, parsing, and the import
 * plan (never overwrites; copies on id clashes; templates follow their
 * presets to new ids).
 */

import assert from 'node:assert/strict';
import { describe, test } from 'node:test';

import { BUILTIN_PRESETS, BUILTIN_IDS, newPreset } from '../src/presets/preset.ts';
import { BUILTIN_TEMPLATES, newTemplate } from '../src/tournaments/template.ts';
import type { CarPreset } from '../src/grandPrix/cars.ts';
import {
  LIBRARY_FILE_FORMAT, LIBRARY_FILE_VERSION, libraryFileName, makeLibraryFile, parseLibraryFile, planImport, type Library,
} from '../src/store/libraryFile.ts';

const NOW = new Date(2026, 10, 25, 9, 30);
const shipped: Library = { presets: BUILTIN_PRESETS, templates: BUILTIN_TEMPLATES, carPresets: [] };

function ids() {
  let n = 0;
  return (kind: 'preset' | 'template' | 'car') => `${kind[0]}-new-${++n}`;
}

const mine = newPreset('p-mine', 'Evening race');
const tplMine = { ...newTemplate('t-mine', 'Club night'), groupPreset: 'p-mine', finalPreset: 'p-mine' };
const cars: CarPreset = { id: 'c-1', name: 'Club cars', cars: { 1: { type: 'open', color: '#ff0000', label: '7' } } };

const roundTrip = (lib: Library) => {
  const r = parseLibraryFile(JSON.stringify(makeLibraryFile(lib, NOW, '0.1.0')));
  assert.ok(r.ok);
  return r.file;
};

describe('library file', () => {
  test('export: format, version, date, items; survives a round trip', () => {
    const f = makeLibraryFile({ presets: [mine], templates: [tplMine], carPresets: [cars] }, NOW, '0.1.0');
    assert.equal(f.format, LIBRARY_FILE_FORMAT);
    assert.equal(f.version, LIBRARY_FILE_VERSION);
    assert.equal(f.exportedAt, NOW.toISOString());
    const back = roundTrip(f);
    assert.deepEqual(back.presets, [mine]);
    assert.deepEqual(back.templates, [tplMine]);
    assert.deepEqual(back.carPresets.map(c => c.name), ['Club cars']);
  });

  test('file name carries the local date', () => {
    assert.equal(libraryFileName(NOW), 'timetaker-library-2026-11-25.json');
  });

  test('parse: not JSON, another format, a newer version', () => {
    assert.deepEqual(parseLibraryFile('{nope'), { ok: false, error: 'json' });
    assert.deepEqual(parseLibraryFile('{"format":"timetaker-race"}'), { ok: false, error: 'format' });
    assert.deepEqual(parseLibraryFile('[]'), { ok: false, error: 'format' });
    assert.deepEqual(parseLibraryFile(JSON.stringify({ format: LIBRARY_FILE_FORMAT, version: 99 })), { ok: false, error: 'version' });
  });

  test('parse: unreadable items are skipped, missing lists are empty, duplicate ids kept once', () => {
    const r = parseLibraryFile(JSON.stringify({
      format: LIBRARY_FILE_FORMAT, version: 1,
      presets: [null, 42, { name: 'no id' }, mine, { ...mine, name: 'again' }],
    }));
    assert.ok(r.ok);
    assert.deepEqual(r.file.presets.map(p => p.name), ['Evening race']);
    assert.deepEqual([r.file.templates, r.file.carPresets], [[], []]);
  });
});

describe('import plan', () => {
  const file = roundTrip({ presets: [mine], templates: [tplMine], carPresets: [cars] });

  test('into an empty library: everything added as it is', () => {
    const plan = planImport(file, shipped, { newId: ids() });
    assert.deepEqual(plan.presets.map(p => p.id), ['p-mine']);
    assert.deepEqual(plan.templates.map(x => [x.id, x.groupPreset, x.finalPreset]), [['t-mine', 'p-mine', 'p-mine']]);
    assert.deepEqual(plan.carPresets.map(c => c.id), ['c-1']);
    assert.deepEqual([plan.skipped, plan.renamed], [0, 0]);
  });

  test('importing the same file again adds nothing', () => {
    const plan = planImport(file, { presets: [...BUILTIN_PRESETS, mine], templates: [...BUILTIN_TEMPLATES, tplMine], carPresets: [cars] }, { newId: ids() });
    assert.deepEqual([plan.presets, plan.templates, plan.carPresets], [[], [], []]);
    assert.equal(plan.skipped, 3);
  });

  test('same id, changed here: the file\'s version comes in as a copy; its template follows it', () => {
    const changedHere = { ...mine, laps: 9 };
    const plan = planImport(file, { presets: [...BUILTIN_PRESETS, changedHere], templates: BUILTIN_TEMPLATES, carPresets: [] }, { newId: ids() });
    assert.equal(plan.presets.length, 1);
    const copy = plan.presets[0];
    assert.equal(copy.id, 'p-new-1');
    assert.equal(copy.name, 'Evening race 2');
    assert.equal(copy.laps, mine.laps);
    assert.deepEqual([plan.templates[0].groupPreset, plan.templates[0].finalPreset], ['p-new-1', 'p-new-1']);
    assert.equal(plan.renamed, 1);
  });

  test('a name taken by another item gets a number; translated names count as taken', () => {
    const other = newPreset('p-other', 'Abendrennen');
    const f = roundTrip({ presets: [{ ...mine, name: 'Abendrennen' }], templates: [], carPresets: [] });
    const plan = planImport(f, { presets: [other], templates: [], carPresets: [] }, { newId: ids() });
    assert.deepEqual(plan.presets.map(p => [p.id, p.name]), [['p-mine', 'Abendrennen 2']]);
    const f2 = roundTrip({ presets: [{ ...mine, name: 'Standardrennen' }], templates: [], carPresets: [] });
    const plan2 = planImport(f2, shipped, { newId: ids(), takenPresetNames: ['Standardrennen'] });
    assert.equal(plan2.presets[0].name, 'Standardrennen 2');
  });

  test('an edited shipped preset becomes a copy (not shipped); an edited tiebreaker is skipped', () => {
    const def = BUILTIN_PRESETS.find(p => p.id === BUILTIN_IDS.default)!;
    const tb = BUILTIN_PRESETS.find(p => p.id === BUILTIN_IDS.tiebreaker)!;
    const tpl = { ...BUILTIN_TEMPLATES[0], groupPreset: BUILTIN_IDS.default, smallFinal: false };
    const f = roundTrip({ presets: [{ ...def, laps: 12 }, { ...tb, laps: 9 }], templates: [tpl], carPresets: [] });
    const plan = planImport(f, shipped, { newId: ids() });
    assert.equal(plan.presets.length, 1);
    assert.deepEqual([plan.presets[0].id, plan.presets[0].builtIn, plan.presets[0].laps, plan.presets[0].name], ['p-new-1', false, 12, 'Default race 2']);
    assert.equal(plan.skipped, 1);
    // The edited shipped template comes in as a copy, pointing at the copy of its preset.
    const x = plan.templates[0];
    assert.deepEqual([x.id, x.builtIn, x.groupPreset, x.finalPreset, x.name],
      ['t-new-2', false, 'p-new-1', 'p-new-1', 'Groups → Second chance → Final 2']);
  });

  test('a template whose preset is missing gets the default preset for that stage', () => {
    const lonely = { ...newTemplate('t-lonely', 'Lonely'), groupPreset: 'p-gone', repechagePreset: 'p-gone', finalPreset: 'p-gone' };
    const plan = planImport(roundTrip({ presets: [], templates: [lonely], carPresets: [] }), shipped, { newId: ids() });
    const fresh = newTemplate('', '');
    assert.deepEqual([plan.templates[0].groupPreset, plan.templates[0].repechagePreset, plan.templates[0].finalPreset],
      [fresh.groupPreset, fresh.repechagePreset, fresh.finalPreset]);
  });

  test('car presets: same cars under another id are already here; a changed one with the same id is a copy', () => {
    const sameOtherId = { ...cars, id: 'c-other' };
    assert.equal(planImport(file, { ...shipped, carPresets: [sameOtherId] }, { newId: ids() }).carPresets.length, 0);
    const changed: CarPreset = { ...cars, cars: { 1: { type: 'closed', color: null, label: '' } } };
    const plan = planImport(file, { ...shipped, carPresets: [changed] }, { newId: ids() });
    assert.deepEqual(plan.carPresets.map(c => [c.id, c.name]), [['c-new-1', 'Club cars 2']]);
  });

  test('a template already in the library under its id with the same content is skipped even after preset remapping', () => {
    const plan = planImport(file, { presets: [...BUILTIN_PRESETS, mine], templates: [...BUILTIN_TEMPLATES, tplMine], carPresets: [] }, { newId: ids() });
    assert.deepEqual(plan.templates, []);
  });
});
