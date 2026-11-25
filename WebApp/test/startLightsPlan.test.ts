/*
 * The start light timeline for the Track Setup preview (SYSTEM_ARCHITECTURE §6.4).
 */

import assert from 'node:assert/strict';
import { describe, test } from 'node:test';

import { planStartLights, randomHoldMs, startLightLevels } from '../src/track/startLightsPlan.ts';

const sl = { attentionPulses: 2, pulseRiseMs: 1000, stepMs: 1000, holdMinMs: 200, holdMaxMs: 3000 };
const round = (a: number[]) => a.map(x => Math.round(x * 100) / 100);

describe('start lights plan', () => {
  test('timeline: 6 lanes, defaults', () => {
    const p = planStartLights(sl, 6, 500);
    assert.equal(p.litMs, 4000 + 5000);
    assert.equal(p.outMs, 9500);
  });
  test('breathing in unison, lane 1 stays on from the last peak', () => {
    const p = planStartLights(sl, 6, 500);
    assert.deepEqual(round(startLightLevels(sl, p, 0)), [0, 0, 0, 0, 0, 0]);
    assert.deepEqual(round(startLightLevels(sl, p, 1000)), [1, 1, 1, 1, 1, 1]);
    assert.deepEqual(round(startLightLevels(sl, p, 2000)), [0, 0, 0, 0, 0, 0]);
    assert.deepEqual(round(startLightLevels(sl, p, 3500)), [1, 0.5, 0.5, 0.5, 0.5, 0.5]);
  });
  test('countdown from lane 1 toward the chain end, then all out', () => {
    const p = planStartLights(sl, 6, 500);
    assert.deepEqual(startLightLevels(sl, p, 4000), [1, 0, 0, 0, 0, 0]);
    assert.deepEqual(startLightLevels(sl, p, 5000), [1, 1, 0, 0, 0, 0]);
    assert.deepEqual(startLightLevels(sl, p, 9000), [1, 1, 1, 1, 1, 1]);
    assert.deepEqual(startLightLevels(sl, p, 9500), [0, 0, 0, 0, 0, 0]);
  });
  test('no attention pulses: lane 1 lights at T0', () => {
    const s = { ...sl, attentionPulses: 0 };
    const p = planStartLights(s, 4, 0);
    assert.deepEqual(startLightLevels(s, p, 0), [1, 0, 0, 0]);
    assert.deepEqual(startLightLevels(s, p, 2000), [1, 1, 1, 0]);
    assert.equal(p.outMs, 3000);
  });
  test('lane count clamped to 1..6; hold drawn from the range', () => {
    assert.equal(planStartLights(sl, 9, 0).lanes, 6);
    assert.equal(planStartLights(sl, 0, 0).lanes, 1);
    assert.equal(randomHoldMs(sl, () => 0), 200);
    assert.equal(randomHoldMs(sl, () => 1), 3000);
  });
});
