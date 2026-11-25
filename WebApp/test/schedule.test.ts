import { test } from 'node:test';
import assert from 'node:assert/strict';
import { defaultActiveLanes, defaultSeating, inactiveLanes, moveToLane, raceSchedule, raceScheduleSeated, validSeating } from '../src/presets/schedule.ts';
import { racesFor } from '../src/presets/preset.ts';

test('a partial field leaves the last lanes empty by default', () => {
  assert.deepEqual(defaultActiveLanes(6, 6), [1, 2, 3, 4, 5, 6]);
  assert.deepEqual(defaultActiveLanes(5, 6), [1, 2, 3, 4, 5]);
  assert.deepEqual(inactiveLanes(defaultActiveLanes(5, 6), 6), [6]);
  assert.deepEqual(defaultActiveLanes(4, 6), [1, 2, 3, 4]);
  assert.deepEqual(defaultActiveLanes(7, 6), []);
});

test('moving a car onto the empty lane: that driver starts there, his old lane is empty', () => {
  const s = defaultSeating(5, 6);                       // lanes 1–5, lane 6 empty
  const moved = moveToLane(s, 2, 6, 6);                 // driver on lane 3 → lane 6
  assert.deepEqual(moved, [1, 2, 6, 4, 5]);
  assert.deepEqual(inactiveLanes(moved, 6), [3]);
  assert.deepEqual(moveToLane(s, 0, 2, 6), s);          // taken: unchanged
  assert.deepEqual(moveToLane(s, 0, 7, 6), s);          // no such lane
  assert.equal(validSeating(moved, 6), true);
  assert.equal(validSeating([1, 1], 6), false);
});

test('a moved seating: first race as seated, everyone drives every used lane k times, lane 3 never', () => {
  const races = raceScheduleSeated([1, 2, 6, 4, 5], 2, 'block');
  assert.equal(races.length, 10);
  assert.deepEqual(races[0].seats, [{ lane: 1, driver: 0 }, { lane: 2, driver: 1 }, { lane: 4, driver: 3 }, { lane: 5, driver: 4 }, { lane: 6, driver: 2 }]);
  const count = new Map<string, number>();
  for (const r of races) for (const x of r.seats) count.set(`${x.driver}@${x.lane}`, (count.get(`${x.driver}@${x.lane}`) ?? 0) + 1);
  assert.equal(count.size, 25);
  assert.ok([...count.values()].every(c => c === 2));
  assert.ok(races.every(r => r.seats.every(x => x.lane !== 3)));
});

test('every driver drives every used lane k times, k × n races, the empty lane never', () => {
  for (const rotation of ['block', 'everyRace'] as const)
    for (const [n, k] of [[5, 3], [6, 1], [2, 2], [4, 2]]) {
      const active = n === 5 ? [1, 2, 4, 5, 6] : defaultActiveLanes(n, 6);
      const races = raceSchedule(n, active, k, rotation);
      assert.equal(races.length, racesFor({ racesPerLane: k }, n, 6));
      const count = new Map<string, number>();
      for (const r of races) {
        assert.deepEqual(r.seats.map(s => s.lane), [...active].sort((a, b) => a - b));
        assert.equal(new Set(r.seats.map(s => s.driver)).size, n);
        for (const s of r.seats) count.set(`${s.driver}@${s.lane}`, (count.get(`${s.driver}@${s.lane}`) ?? 0) + 1);
      }
      assert.equal(count.size, n * n);
      assert.ok([...count.values()].every(c => c === k));
      assert.ok(races.every(r => r.seats.every(s => s.lane !== 3) || active.includes(3)));
    }
});

test('block keeps a lane for k races, everyRace moves on after every race', () => {
  assert.deepEqual(raceSchedule(3, [1, 2, 3], 2, 'block').map(r => r.shift), [0, 0, 1, 1, 2, 2]);
  assert.deepEqual(raceSchedule(3, [1, 2, 3], 2, 'everyRace').map(r => r.shift), [0, 1, 2, 0, 1, 2]);
  // shift 1: driver 0 moves from the first to the second used lane
  const r = raceSchedule(3, [2, 4, 5], 1, 'block');
  assert.deepEqual(r[0].seats, [{ lane: 2, driver: 0 }, { lane: 4, driver: 1 }, { lane: 5, driver: 2 }]);
  assert.deepEqual(r[1].seats, [{ lane: 2, driver: 2 }, { lane: 4, driver: 0 }, { lane: 5, driver: 1 }]);
});

test('lanes that do not fit the drivers give no schedule', () => {
  assert.deepEqual(raceSchedule(4, [1, 2, 3], 1, 'block'), []);
  assert.deepEqual(raceSchedule(3, [1, 1, 2], 1, 'block'), []);
  assert.deepEqual(raceSchedule(3, [1, 2, 3], 0, 'block'), []);
});
