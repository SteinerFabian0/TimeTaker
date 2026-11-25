/*
 * Race presets (GUI_SPEC §4): defaults, validation, normalization, the
 * group arithmetic of the summary card, and the bridge to the race engine.
 */

import assert from 'node:assert/strict';
import { describe, test } from 'node:test';

import {
  BUILTIN_PRESETS, BUILTIN_IDS, LIMITS, MAX_PLACES, POINTS_TABLES,
  duplicatePreset, isAtDefaults, groupEstimateMs, warmupBefore, warmupsIn, newPreset, normalizePreset,
  isQualifying, isTiebreaker, pointsFor, pointsTableOf, racesFor, toRaceConfig, uniqueName, validatePreset,
} from '../src/presets/preset.ts';
import { raceScheduleSeated } from '../src/presets/schedule.ts';
import { resolveConfig } from '../src/race/rules/config.ts';
import { MemoryLibraryStorage } from '../src/store/libraryStorage.ts';
import { DEFAULT_TRACK_TIMING } from '../src/track/trackTiming.ts';
import { DEFAULT_RULES, effectiveResultBasis, normalizeRules } from '../src/presets/rules.ts';

const group = BUILTIN_PRESETS.find(p => p.id === BUILTIN_IDS.default)!;
const short = BUILTIN_PRESETS.find(p => p.id === BUILTIN_IDS.short)!;

describe('built-in presets', () => {
  test('default race, short race, qualifying, tiebreaker: races/lane, laps, warm-up laps', () => {
    const tb = BUILTIN_PRESETS.find(p => p.id === BUILTIN_IDS.tiebreaker)!;
    const q = BUILTIN_PRESETS.find(p => p.id === BUILTIN_IDS.qualifying)!;
    assert.deepEqual(BUILTIN_PRESETS.map(p => p.name), ['Default race', 'Short race', 'Qualifying', 'Tiebreaker']);
    assert.deepEqual([isQualifying(q), q.durationMs, q.warmupLaps], [true, 5 * 60_000, 2]);
    assert.equal(isQualifying(group), false);
    assert.equal(isTiebreaker(tb.id), true);
    assert.deepEqual([short.racesPerLane, short.laps, short.warmupLaps], [2, 3, 1]);
    assert.deepEqual([group.racesPerLane, group.laps, group.warmupLaps], [3, 5, 2]);
    assert.deepEqual([tb.racesPerLane, tb.laps, tb.warmupLaps], [1, 4, 1]);
  });
  test('an edited shipped preset stays shipped through storage, and knows when it is back at its defaults', () => {
    assert.equal(isAtDefaults(group), true);
    const edited = { ...group, laps: 7 };
    assert.equal(isAtDefaults(edited), false);
    const stored = normalizePreset(JSON.parse(JSON.stringify(edited)))!;
    assert.equal(stored.builtIn, true);
    assert.equal(stored.laps, 7);
    assert.equal(isAtDefaults({ ...edited, laps: 5 }), true);
    assert.equal(isAtDefaults(newPreset('x', 'X')), false);
  });
  test('a stored shipped preset missing a field takes the shipped value', () => {
    assert.equal(normalizePreset({ id: BUILTIN_IDS.default, laps: 6 })!.warmupLaps, 2);
  });
  test('all valid, built in; 6…1 points, the qualifying 10-7-5-3-1-0; crash and false-start rules are not theirs', () => {
    for (const p of BUILTIN_PRESETS) {
      assert.deepEqual(validatePreset(p, BUILTIN_PRESETS), [], p.name);
      assert.equal(p.builtIn, true);
      assert.equal('crashPolicy' in p || 'falseStart' in p, false);
      assert.deepEqual(p.points, isQualifying(p) ? [10, 7, 5, 3, 1, 0] : [6, 5, 4, 3, 2, 1]);
    }
    assert.equal(pointsTableOf([10, 7, 5, 3, 1, 0]), 'wide');
  });
  test('one kind each: qualifying on time, race and tiebreaker on laps', () => {
    assert.deepEqual(BUILTIN_PRESETS.map(p => p.kind), ['race', 'race', 'qualifying', 'tiebreaker']);
    for (const p of BUILTIN_PRESETS) assert.equal(p.mode, p.kind === 'qualifying' ? 'time' : 'laps');
    const q = newPreset('q', 'Q', 'qualifying');
    assert.deepEqual([q.kind, q.mode, q.points], ['qualifying', 'time', [10, 7, 5, 3, 1, 0]]);
    assert.deepEqual(validatePreset({ ...q, mode: 'laps' }), ['enum']);
  });
});

describe('group arithmetic', () => {
  test('reference evening: 18 races of 5 laps per group, a 2-lap warm-up only on each of the 6 lanes, about 64 minutes', () => {
    assert.equal(racesFor(group, 6), 18);
    const ms = groupEstimateMs(group, 6, 27_000, 60_000);
    assert.equal(ms, 18 * (5 * 27_000 + 60_000) + 6 * 2 * 27_000);
    assert.equal(Math.round(ms / 60_000), 64);
  });
  test('a warm-up only when the drivers sit down cold or change lanes', () => {
    // Block, 3 races per lane: races 0, 3, 6 … change lanes.
    assert.deepEqual([0, 1, 2, 3, 4, 5, 6].map(i => warmupBefore(group, 6, i)), [true, false, false, true, false, false, true]);
    // A section starting mid-lane (another group raced in between): cold again.
    assert.equal(warmupBefore(group, 6, 4, 4), true);
    assert.equal(warmupsIn(group, 6, 9, 9), 3);
    // Every-race rotation: every race changes lanes.
    const every = { ...group, rotation: 'everyRace' as const };
    assert.equal(warmupsIn(every, 6, 0, 18), 18);
    // A single driver never changes lanes.
    assert.equal(warmupsIn(every, 1, 0, 3), 1);
  });
  test('the warm-ups match the schedule: a race starts with one exactly when a seat changed', () => {
    for (const rotation of ['block', 'everyRace'] as const)
      for (const k of [1, 2, 3])
        for (const n of [1, 2, 4, 6]) {
          const races = raceScheduleSeated(Array.from({ length: n }, (_, d) => d + 1), k, rotation);
          races.forEach((r, i) => {
            const changed = i === 0 || r.seats.some((z, j) => z.driver !== races[i - 1].seats[j].driver);
            assert.equal(warmupBefore({ racesPerLane: k, rotation }, n, i), changed, `${rotation} k=${k} n=${n} race ${i}`);
          });
        }
  });
  test('fewer drivers than lanes use fewer lanes: 4 drivers → k × 4 races', () => {
    assert.equal(racesFor(short, 4), 8);
  });
  test('a group larger than the track does not fit', () => {
    assert.equal(racesFor(group, 5, 4), 0);
    assert.equal(racesFor(group, 0), 0);
  });
  test('a qualifying rotates over the lanes like races: k × N sessions of warm-up + duration + changeover', () => {
    const p = { ...group, mode: 'time' as const, durationMs: 4 * 60_000 };
    assert.equal(racesFor(p, 6), 18);
    assert.equal(racesFor(p, 5, 4), 0);
    assert.equal(groupEstimateMs(p, 2, 27_000, 60_000), 6 * (240_000 + 60_000) + 2 * 2 * 27_000);
  });
});

describe('points', () => {
  test('places beyond the field are unused', () => {
    assert.deepEqual([1, 2, 3, 4, 5].map(pl => pointsFor(group, pl, 4)), [6, 5, 4, 3, 0]);
    assert.equal(pointsFor(group, 0), 0);
    assert.equal(pointsFor(group, 7), 0);
  });
  test('named tables are recognised, custom ones are not', () => {
    assert.equal(pointsTableOf(POINTS_TABLES.f1), 'f1');
    assert.equal(pointsTableOf([6, 5, 4, 3, 2, 2]), null);
  });
});

describe('tournament rules', () => {
  test('stored rules read back; bad fields take the defaults, the penalty is clamped', () => {
    assert.deepEqual(normalizeRules(undefined), DEFAULT_RULES);
    const r = normalizeRules({ crashPolicy: 'replacementAllowed', falseStart: { action: 'nope', penaltyMs: 99_000 } });
    assert.equal(r.crashPolicy, 'replacementAllowed');
    assert.equal(r.falseStart.action, 'timePenalty');
    assert.equal(r.falseStart.penaltyMs, 60_000);
  });
  test('an old stored preset loses its crash and false-start fields', () => {
    const p = normalizePreset({ id: 'x', name: 'Old', crashPolicy: 'replacementAllowed', falseStart: { action: 'flag' } })!;
    assert.equal('crashPolicy' in p || 'falseStart' in p, false);
  });
});

describe('result basis (tournament rules)', () => {
  test('only matters when a crash puts the car out', () => {
    assert.equal(effectiveResultBasis({ crashPolicy: 'replacementAllowed', resultBasis: 'distance' }, true), null);
  });
  test('distance falls back to crash order without the Power Pack', () => {
    assert.equal(effectiveResultBasis({ crashPolicy: 'crashIsDnf', resultBasis: 'distance' }, false), 'crashOrder');
    assert.equal(effectiveResultBasis({ crashPolicy: 'crashIsDnf', resultBasis: 'distance' }, true), 'distance');
  });
});

describe('validation', () => {
  test('names: not empty, unique (case and spaces ignored)', () => {
    const a = newPreset('a', 'Sprint');
    assert.deepEqual(validatePreset({ ...a, name: '  ' }), ['nameEmpty']);
    assert.deepEqual(validatePreset(a, [newPreset('b', ' sprint ')]), ['nameTaken']);
    assert.deepEqual(validatePreset(a, [a]), []);
  });
  test('ranges', () => {
    const a = newPreset('a', 'A');
    assert.deepEqual(validatePreset({ ...a, laps: 0 }), ['laps']);
    assert.deepEqual(validatePreset({ ...a, warmupLaps: 11 }), ['warmupLaps']);
    assert.deepEqual(validatePreset({ ...a, racesPerLane: 11 }), ['racesPerLane']);
    assert.deepEqual(validatePreset({ ...a, points: [6, 5, 4] }), ['points']);
  });
});

describe('names and copies', () => {
  test('uniqueName adds the smallest free number', () => {
    assert.equal(uniqueName('New preset', []), 'New preset');
    assert.equal(uniqueName('New preset', ['new preset', 'New preset 2']), 'New preset 3');
  });
  test('a duplicate of a built-in is editable', () => {
    const d = duplicatePreset(group, 'x', 'Group race 2');
    assert.equal(d.builtIn, false);
    assert.equal(d.laps, 5);
    assert.equal(d.id, 'x');
  });
});

describe('normalizePreset', () => {
  test('a round trip keeps a valid preset', () => {
    const p = { ...newPreset('a', 'A'), laps: 12, points: [10, 7, 5, 3, 2, 1] };
    assert.deepEqual(normalizePreset(JSON.parse(JSON.stringify(p))), p);
  });
  test('a preset stored before start lights and minimum lap moved to the track drops them', () => {
    const p = normalizePreset({ ...newPreset('a', 'A'), minLapMs: 15_000, startLights: { stepMs: 800 } })! as unknown as Record<string, unknown>;
    assert.equal('minLapMs' in p, false);
    assert.equal('startLights' in p, false);
  });
  test('kind: an older time preset is a qualifying; a stored tiebreaker kind is only the shipped one', () => {
    assert.deepEqual([normalizePreset({ id: 'a', mode: 'time' })!.kind, normalizePreset({ id: 'a', mode: 'time' })!.mode], ['qualifying', 'time']);
    assert.equal(normalizePreset({ id: 'a' })!.kind, 'race');
    assert.equal(normalizePreset({ id: 'a', kind: 'tiebreaker' })!.kind, 'race');
    assert.equal(normalizePreset({ id: BUILTIN_IDS.tiebreaker, kind: 'race' })!.kind, 'tiebreaker');
    assert.equal(normalizePreset({ id: 'a', kind: 'qualifying', mode: 'laps' })!.mode, 'time');
  });
  test('not a preset → null', () => {
    assert.equal(normalizePreset(null), null);
    assert.equal(normalizePreset({ name: 'x' }), null);
  });
  test('broken fields take defaults or are clamped; never built-in', () => {
    const p = normalizePreset({ id: 'z', builtIn: true, laps: 5000, mode: 'banana', points: [9, 'x'] })!;
    assert.equal(p.builtIn, false);
    assert.equal(p.laps, LIMITS.laps.max);
    assert.equal(p.mode, 'laps');
    assert.equal(p.points.length, MAX_PLACES);
    assert.deepEqual(p.points.slice(0, 3), [9, 0, 0]);
    assert.deepEqual(validatePreset(p), []);
  });
});

describe('toRaceConfig', () => {
  test('the race engine accepts it; start lights and minimum lap come from the track', () => {
    const track = { ...DEFAULT_TRACK_TIMING, minLapMs: 15_000, startLights: { ...DEFAULT_TRACK_TIMING.startLights, stepMs: 800 } };
    const rules = { ...DEFAULT_RULES, falseStart: { ...DEFAULT_RULES.falseStart, action: 'flag' as const } };
    const c = resolveConfig(toRaceConfig(group, rules, track, 'r1', [{ lane: 1, driver: 'Anna' }, { lane: 2, driver: 'Ben' }]));
    assert.equal(c.laps, 5);
    assert.equal(c.minLapMs, 15_000);
    assert.equal(c.startLights.stepMs, 800);
    assert.equal(c.crashPolicy, 'crashIsDnf');
    assert.equal(c.falseStart.action, 'flag');            // the tournament's rules, not the preset's
    assert.equal(c.name, 'Default race');
  });
});

describe('MemoryLibraryStorage', () => {
  test('settings: put, get, delete; stored copies are independent', async () => {
    const s = new MemoryLibraryStorage();
    const v = { minLapMs: 1 };
    await s.putSetting('k', v);
    v.minLapMs = 2;
    assert.deepEqual(await s.getSetting('k'), { minLapMs: 1 });
    await s.deleteSetting('k');
    assert.equal(await s.getSetting('k'), undefined);
  });
  test('put, list, delete; stored copies are independent', async () => {
    const s = new MemoryLibraryStorage();
    const p = newPreset('a', 'A');
    await s.put(p);
    await s.put(newPreset('b', 'B'));
    await s.delete('b');
    const all = await s.list();
    assert.deepEqual(all, [p]);
    assert.notEqual(all[0], p);
  });
});
