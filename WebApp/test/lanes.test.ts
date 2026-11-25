/*
 * The track's lane count (GUI_SPEC §8.0): 2, 4 or 6, capped by a connected
 * time taker.
 */

import assert from 'node:assert/strict';
import { describe, test } from 'node:test';

import { allowedLanes, fitLanes, normalizeLanes, DEFAULT_LANES } from '../src/track/lanes.ts';

describe('track lanes', () => {
  test('without a time taker every option is allowed', () => {
    assert.deepEqual(allowedLanes(null), [2, 4, 6]);
    assert.deepEqual(allowedLanes(0), [2, 4, 6]);
  });
  test('a time taker with 4 lanes locks 6', () => {
    assert.deepEqual(allowedLanes(4), [2, 4]);
    assert.deepEqual(allowedLanes(5), [2, 4]);
    assert.deepEqual(allowedLanes(1), [2]);
  });
  test('a setting above the time taker drops to the largest that fits; others stay', () => {
    assert.equal(fitLanes(6, 4), 4);
    assert.equal(fitLanes(6, 2), 2);
    assert.equal(fitLanes(2, 6), 2);
    assert.equal(fitLanes(4, null), 4);
  });
  test('stored values are read back to a valid option', () => {
    assert.equal(normalizeLanes(4), 4);
    assert.equal(normalizeLanes(5), DEFAULT_LANES);
    assert.equal(normalizeLanes('6'), DEFAULT_LANES);
  });
});
