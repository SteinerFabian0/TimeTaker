/*
 * RaceDirector (intents → device requests + host actions) and the
 * BroadcastChannel sync (a TV window as a pure view of raceState).
 */

import assert from 'node:assert/strict';
import { describe, test } from 'node:test';

import { DeviceApi } from '../src/device/deviceApi.ts';
import { adaptiveHoldoffUs, RaceDirector } from '../src/director/raceDirector.ts';
import { ClientKind, LaneState, LedMode, SamplingMode } from '../src/protocol/generated.ts';
import { fold } from '../src/race/raceEngine.ts';
import { resolveConfig } from '../src/race/rules/config.ts';
import type { RaceState } from '../src/race/types.ts';
import { EventStore } from '../src/store/eventStore.ts';
import { FakeChannelHub, RaceStatePublisher, RaceStateSubscriber } from '../src/sync/raceSync.ts';
import { named, recorder, rig } from './helpers.ts';

describe('RaceDirector', () => {
  test('prepare, start, abort and restart, corrections and undo, adaptive holdoff', async () => {
    const t = rig();
    await t.run(async () => {
      const dev = t.sim({ lanes: 2 });
      const rec = recorder();
      const store = await new EventStore().open();
      const s = t.session(dev, { ...rec, onEvent: (e) => { rec.events.push(e); store.appendEvent(e); } }, { clientKind: ClientKind.WEBAPP });
      await s.connect();
      const api = new DeviceApi(s);
      let n = 0;
      const d = new RaceDirector({ api, store, raceId: 'r1', newId: () => `a${++n}`, wallClock: () => 'now' });
      store.createRace(resolveConfig({ id: 'r1', laps: 5, entries: [{ lane: 1, driver: 'A' }, { lane: 2, driver: 'B' }] }));

      await d.powerLanes(true);
      await t.until(() => named(rec, 'EVT_LANE_STATE').filter((e) => e.value?.['state'] === LaneState.CONFIGURED).length === 2);
      await api.laneSampling(3, SamplingMode.OFF);
      await d.prepareLanes();
      assert.equal(dev.laneSamplingMode(2), SamplingMode.ON);
      await d.ledTest();
      assert.equal(dev.laneLed(1).mode, LedMode.BLINK);

      await d.start({ stepMs: 300 });
      await t.until(() => d.state().phase === 'starting');
      await assert.rejects(d.start(), /starting/);
      await d.abortStart();
      await t.until(() => d.state().aborts.length === 1);
      assert.equal(d.state().phase, 'waiting');

      dev.carsRace([1, 2], 9);
      await d.start({ stepMs: 300 });
      await t.until(() => d.state().fastestLap !== null, 60_000);
      const hold = await d.applyAdaptiveHoldoff(0.5);
      assert.equal(hold, Math.floor(d.state().fastestLap!.ms * 0.5) * 1000);
      assert.equal(dev.laneConfig(1).holdoffUs, hold);

      const pen = d.penalty(2, 3000, 'jump');
      const plus = d.adjustLaps(1, 1);
      d.undo(plus.id);
      assert.equal(pen.at?.bootId, dev.bootId);
      assert.equal(pen.wallTime, 'now');
      await t.until(() => d.state().phase === 'finished', 120_000);
      const st = d.state();
      assert.equal(st.cars.find((c) => c.lane === 2)!.penaltiesMs, 3000);
      assert.equal(st.cars.find((c) => c.lane === 1)!.lapAdjust, 0);
      assert.deepEqual(st.appliedActions, ['a1', 'a3']);
    });
  });

  test('adaptive holdoff is 0 before any valid lap', () => {
    const s = fold(resolveConfig({ id: 'x', entries: [{ lane: 1, driver: 'a' }] }), []);
    assert.equal(adaptiveHoldoffUs(s), 0);
  });
});

const state = (raceId: string, phase: RaceState['phase']): RaceState =>
  ({ ...fold(resolveConfig({ id: raceId, entries: [{ lane: 1, driver: 'a' }] }), []), phase });

describe('race state sync', () => {
  test('a late subscriber gets the latest state at once; then every update', async () => {
    const hub = new FakeChannelHub();
    const pub = new RaceStatePublisher(hub.channel(), 'control');
    pub.publish(state('r', 'waiting'));
    pub.publish(state('r', 'running'));
    const seen: string[] = [];
    const tv = new RaceStateSubscriber(hub.channel(), (s, from) => seen.push(`${from}:${s.phase}`));
    await new Promise((r) => setTimeout(r, 5));
    assert.deepEqual(seen, ['control:running']);
    pub.publish(state('r', 'finishing'));
    await new Promise((r) => setTimeout(r, 5));
    assert.deepEqual(seen, ['control:running', 'control:finishing']);
    assert.equal(tv.state?.phase, 'finishing');
    tv.close();
    pub.close();
  });

  test('a late message never rewinds the view; bigint survives the clone', () => {
    const hub = new FakeChannelHub();
    hub.manual = true;
    const pub = new RaceStatePublisher(hub.channel(), 'p');
    const seen: string[] = [];
    new RaceStateSubscriber(hub.channel(), (s) => seen.push(s.phase));
    hub.deliver();                                                     // the hello (no state yet)
    pub.publish(state('r', 'running'));
    pub.publish({ ...state('r', 'finished'), warnings: [String(2n ** 60n)] });
    hub.deliver((q) => q.reverse());
    assert.deepEqual(seen, ['finished']);
  });

  test('two publishers: the subscriber follows whoever spoke last; bye forgets a publisher', async () => {
    const hub = new FakeChannelHub();
    const a = new RaceStatePublisher(hub.channel(), 'a');
    const b = new RaceStatePublisher(hub.channel(), 'b');
    const tv = new RaceStateSubscriber(hub.channel(), () => undefined);
    a.publish(state('r', 'running'));
    await new Promise((r) => setTimeout(r, 2));
    b.publish(state('r', 'finishing'));
    await new Promise((r) => setTimeout(r, 2));
    assert.equal(tv.source, 'b');
    a.close();
    b.close();
    tv.close();
  });
});
