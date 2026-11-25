import { test } from 'node:test';
import assert from 'node:assert/strict';
import { addCrossing, carDrive, laneWarmup, startWarmup, stopLanes, warmupDriveMs } from '../src/grandPrix/warmup.ts';
import { LaneEventType } from '../src/protocol/generated.ts';

const cross = (lane: number, sec: number, type: number = LaneEventType.CROSSING) =>
  ({ lane, type, flags: 0, tEntryUs: BigInt(Math.round(sec * 1e6)) });

test('every counted crossing starts the next warm-up lap', () => {
  let s = startWarmup();
  s = addCrossing(s, cross(1, 0), [1, 2], 3000);
  s = addCrossing(s, cross(1, 5), [1, 2], 3000);
  assert.deepEqual(s.counts, { 1: 2 });
});

test('a crossing sooner than the minimum lap time, another event type or another lane does not count', () => {
  const s0 = addCrossing(startWarmup(), cross(1, 10), [1, 2], 3000);
  assert.equal(addCrossing(s0, cross(1, 12.9), [1, 2], 3000), s0);
  assert.equal(addCrossing(s0, cross(2, 11, LaneEventType.BLOCKED), [1, 2], 3000), s0);
  assert.equal(addCrossing(s0, cross(3, 11), [1, 2], 3000), s0);
  assert.equal(addCrossing(s0, cross(1, 13), [1, 2], 3000).counts[1], 2);
});

test('two warm-up laps: the light goes red as the last lap starts, a third crossing is one too many', () => {
  assert.deepEqual(laneWarmup(0, 2), { lap: 0, stop: false, over: false, showCount: true });
  assert.deepEqual(laneWarmup(1, 2), { lap: 1, stop: false, over: false, showCount: true });
  assert.deepEqual(laneWarmup(2, 2), { lap: 2, stop: true, over: false, showCount: true });
  assert.deepEqual(laneWarmup(3, 2), { lap: 3, stop: true, over: true, showCount: true });
});

test('one warm-up lap: no counter (the lap never ends) unless the car crosses again', () => {
  assert.deepEqual(laneWarmup(0, 1), { lap: 0, stop: false, over: false, showCount: false });
  assert.deepEqual(laneWarmup(1, 1), { lap: 1, stop: true, over: false, showCount: false });
  assert.equal(laneWarmup(2, 1).showCount, true);
  assert.equal(laneWarmup(2, 1).over, true);
});

test('stopLanes: the lanes whose light is red', () => {
  let s = startWarmup();
  for (const [l, t] of [[1, 0], [1, 5], [3, 1]] as const) s = addCrossing(s, cross(l, t), [1, 3, 4], 3000);
  assert.deepEqual(stopLanes(s, [1, 3, 4], 2), [1]);
  assert.deepEqual(stopLanes(s, [1, 3, 4], 1), [1, 3]);
});

test('the drive on screen takes 70 % of the average warm-up', () => {
  assert.equal(warmupDriveMs(27_000, 2), 37_800);
  assert.equal(warmupDriveMs(27_000, 0), 0);
});

test('each car drives at its own pace, rising from 0 to 1, within the spread', () => {
  const a = carDrive(42, 1, 10_000), b = carDrive(42, 2, 10_000);
  assert.deepEqual(carDrive(42, 1, 10_000), a, 'same seed and lane: same drive');
  assert.notDeepEqual(a, b);
  for (const d of [a, b]) {
    assert.ok(d.durationMs >= 8_800 && d.durationMs <= 11_200);
    assert.deepEqual(d.points[0], { t: 0, p: 0 });
    assert.deepEqual(d.points.at(-1), { t: 1, p: 1 });
    for (let i = 1; i < d.points.length; i++) {
      assert.ok(d.points[i].t > d.points[i - 1].t);
      assert.ok(d.points[i].p > d.points[i - 1].p);
    }
  }
});
