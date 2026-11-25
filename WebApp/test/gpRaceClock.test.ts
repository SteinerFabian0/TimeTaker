import { test } from 'node:test';
import assert from 'node:assert/strict';
import { finalLapLanes, formatLap, lapClock, lapOn, LAST_LAP_HOLD_MS } from '../src/grandPrix/raceClock.ts';
import type { CarResult, Lap } from '../src/race/types.ts';

const lap = (n: number, ms: number, endUs: number): Lap => ({ n, ms, endUs, bootId: 1, evSeq: n, raceMs: endUs / 1000, flags: [] });
const car = (laps: Lap[], over: Partial<CarResult> = {}): CarResult => ({
  lane: 1, driver: 'Anna', status: 'running', laps: laps.length, lapAdjust: 0, lapTimes: laps, tlfLap: null,
  lastLapMs: laps.at(-1)?.ms ?? null, bestLapMs: null, bestLapN: null, reactionMs: 300, falseStart: false,
  penaltiesMs: 0, elapsedMs: null, totalMs: null, rejected: [], ...over,
});

test('idle until the lights are out', () => {
  assert.deepEqual(lapClock(car([]), null, 5e6), { kind: 'idle' });
});

test('lap 1 runs from lights out; the start crossing does not restart it', () => {
  assert.deepEqual(lapClock(car([]), 1e6, 3e6), { kind: 'live', ms: 2000 });
  assert.deepEqual(lapClock(car([], { reactionMs: null, status: 'grid' }), 1e6, 3e6), { kind: 'live', ms: 2000 });
});

test('lap counter, F1 style: 0 on the grid, 1 from the start crossing, capped at the laps', () => {
  assert.equal(lapOn(car([], { status: 'grid', reactionMs: null }), 5), 0);
  assert.equal(lapOn(car([]), 5), 1);
  assert.equal(lapOn(car([lap(1, 9000, 10e6), lap(2, 9000, 19e6)]), 5), 3);
  assert.equal(lapOn(car([lap(1, 9000, 10e6)], { laps: 4 }), 5), 5);
  assert.equal(lapOn(car([], { laps: 5, status: 'finished' }), 5), 5);
  assert.equal(lapOn(car([lap(1, 9000, 10e6)]), null), 2, 'time race: no cap');
  assert.equal(lapOn(undefined, 5), 0);
});

test('a finished lap shows for 5 s, then the clock runs live from that crossing', () => {
  const c = car([lap(1, 9000, 10e6)]);
  assert.deepEqual(lapClock(c, 1e6, 12e6), { kind: 'last', ms: 9000, n: 1 });
  assert.deepEqual(lapClock(c, 1e6, 10e6 + LAST_LAP_HOLD_MS * 1000), { kind: 'live', ms: LAST_LAP_HOLD_MS });
});

test('after the flag the last lap stays', () => {
  const c = car([lap(1, 9000, 10e6)], { status: 'finished' });
  assert.deepEqual(lapClock(c, 1e6, 60e6), { kind: 'finished', ms: 9000, n: 1 });
});

test('formatLap: always minutes, seconds and hundredths', () => {
  assert.equal(formatLap(23456), '0:23.45');
  assert.equal(formatLap(62345), '1:02.34');
  assert.equal(formatLap(9999.9), '0:09.99');
  assert.equal(formatLap(0), '0:00.00');
  assert.equal(formatLap(600_000), '9:59.99');
});

test('the light goes red when a car starts its last lap', () => {
  const cars = [
    car([], { lane: 1, status: 'grid', reactionMs: null }),
    car([lap(1, 9000, 10e6), lap(2, 9000, 19e6), lap(3, 9000, 28e6), lap(4, 9000, 37e6)], { lane: 2 }),
    car([lap(1, 9000, 10e6)], { lane: 3 }),
    car([], { lane: 4, status: 'finished' }),
  ];
  assert.deepEqual(finalLapLanes(cars, 5), [2, 4]);
  assert.deepEqual(finalLapLanes([car([], { lane: 5 })], 1), [5], 'one-lap race: red from the start crossing');
});
