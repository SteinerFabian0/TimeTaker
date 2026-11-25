/*
 * Track timing (GUI_SPEC §8.1): start lights and minimum lap time per track.
 */

import assert from 'node:assert/strict';
import { describe, test } from 'node:test';

import { DEFAULT_RACE, DEFAULT_START_LIGHTS } from '../src/race/rules/config.ts';
import {
  DEFAULT_TRACK_TIMING, TIMING_LIMITS, isDefaultTrackTiming, normalizeTrackTiming, trackRaceConfig, validateTrackTiming,
} from '../src/track/trackTiming.ts';

describe('track timing', () => {
  test('defaults are the race engine defaults, and valid', () => {
    assert.deepEqual(DEFAULT_TRACK_TIMING.startLights, DEFAULT_START_LIGHTS);
    assert.equal(DEFAULT_TRACK_TIMING.minLapMs, DEFAULT_RACE.minLapMs);
    assert.deepEqual(validateTrackTiming(DEFAULT_TRACK_TIMING), []);
    assert.equal(isDefaultTrackTiming(DEFAULT_TRACK_TIMING), true);
  });
  test('validation', () => {
    const d = DEFAULT_TRACK_TIMING;
    assert.deepEqual(validateTrackTiming({ ...d, minLapMs: -1 }), ['minLap']);
    assert.deepEqual(validateTrackTiming({ ...d, startLights: { ...d.startLights, holdMinMs: 4000, holdMaxMs: 3000 } }), ['startLights']);
  });
  test('normalize: nothing stored → defaults; broken fields clamped; hold range kept in order', () => {
    assert.deepEqual(normalizeTrackTiming(undefined), DEFAULT_TRACK_TIMING);
    const t = normalizeTrackTiming({ minLapMs: 1e9, startLights: { attentionPulses: 'x', holdMinMs: 3000, holdMaxMs: 100 } });
    assert.equal(t.minLapMs, TIMING_LIMITS.minLapMs.max);
    assert.equal(t.startLights.attentionPulses, DEFAULT_START_LIGHTS.attentionPulses);
    assert.equal(t.startLights.holdMaxMs, 3000);
    assert.deepEqual(validateTrackTiming(t), []);
    assert.equal(isDefaultTrackTiming(t), false);
  });
  test('the race config part', () => {
    const c = trackRaceConfig({ ...DEFAULT_TRACK_TIMING, minLapMs: 15_000 });
    assert.equal(c.minLapMs, 15_000);
    assert.deepEqual(c.startLights, DEFAULT_START_LIGHTS);
  });
});
