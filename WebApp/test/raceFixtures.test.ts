/*
 * Recorded races are regression tests (SYSTEM_ARCHITECTURE §11).
 *
 * Each file in test/fixtures/races/ is a race file (src/store/raceFile.ts)
 * recorded by tools/genFixtures.ts from the simulator through the whole
 * stack. Its `expect` comes from the simulator's ground truth (not from the
 * engine). Here:
 *   1. every fixture folds to its `expect`;
 *   2. the simulator still records the same events, byte for byte
 *      (a behaviour change in the simulator shows up here first);
 *   3. pulling USB mid-race and continuing over BLE gives standings identical
 *      to the same race without the unplug (the required test, §7.4);
 *   4. the store and the file format round-trip.
 */

import assert from 'node:assert/strict';
import { readdirSync, readFileSync } from 'node:fs';
import { describe, test } from 'node:test';

import { fold } from '../src/race/raceEngine.ts';
import { EventStore, MemoryStorage } from '../src/store/eventStore.ts';
import { exportRace, parseRace, stringifyRace } from '../src/store/raceFile.ts';
import type { RaceExpect } from '../tools/lib/raceScenarios.ts';
import { runScenario, SCENARIOS } from '../tools/lib/raceScenarios.ts';

const dir = new URL('./fixtures/races/', import.meta.url);
const files = readdirSync(dir).filter((f) => f.endsWith('.json')).sort();
const load = (f: string): string => readFileSync(new URL(f, dir), 'utf8');

describe('fixtures fold to their expected results', () => {
  test('there are fixtures', () => assert.ok(files.length >= 8));

  for (const f of files) {
    test(f, () => {
      const race = parseRace(load(f));
      const exp = race.expect as RaceExpect;
      const s = fold(race.race.config, race.events, race.actions);
      assert.equal(s.phase, exp.phase);
      assert.deepEqual(s.standings.map((x) => x.lane), exp.order, 'order');
      assert.equal(s.timingGaps.length, exp.timingGaps, 'timing gaps');
      assert.equal(s.aborts.length, exp.aborts, 'aborts');
      for (const c of s.cars) {
        const e = exp.cars[String(c.lane)]!;
        const at = `lane ${c.lane}`;
        assert.equal(c.laps, e.laps, `${at} laps`);
        assert.equal(c.status, e.status, `${at} status`);
        assert.equal(c.falseStart, e.falseStart, `${at} false start`);
        assert.equal(c.reactionMs, e.reactionMs, `${at} reaction`);
        assert.deepEqual(c.lapTimes.map((l) => l.ms), e.lapMs, `${at} lap times`);
        assert.equal(c.penaltiesMs, e.penaltiesMs, `${at} penalties`);
        assert.equal(c.tlfLap, e.tlfLap, `${at} TLF lap`);
        if (e.elapsedMs !== undefined) assert.equal(c.elapsedMs, e.elapsedMs, `${at} elapsed`);
      }
    });
  }
});

describe('fixture contents', () => {
  test('incidents: doubles rejected, a block counted, holdoff seen, a false start', () => {
    const race = parseRace(load('laps-incidents.json'));
    const s = fold(race.race.config, race.events, race.actions);
    const rejected = s.cars.flatMap((c) => c.rejected.map((r) => `${c.lane}:${r.reason}`));
    assert.ok(rejected.includes('3:minLap') || rejected.includes('3:holdoff'));
    assert.ok(rejected.some((r) => r.startsWith('5:')));
    assert.ok(s.cars.find((c) => c.lane === 4)!.lapTimes.some((l) => l.flags.includes('blocked')));
    assert.equal(s.cars.find((c) => c.lane === 2)!.falseStart, true);
    assert.equal(race.actions.length, 3);
    assert.deepEqual(s.appliedActions, ['act1', 'act3']);             // the penalty and the undo; the +1 (act2) is undone
  });

  test('incidents: the false start costs 5 s on the first lap; every counted lap is 30–40 s or an incident lap', () => {
    const race = parseRace(load('laps-incidents.json'));
    const s = fold(race.race.config, race.events, race.actions);
    const ben = s.cars.find((c) => c.lane === 2)!;
    assert.equal(ben.penaltiesMs, 5000);
    assert.deepEqual(ben.lapTimes[0]!.flags, ['falseStartPenalty']);
    const raw = s.cars.flatMap((c) => c.lapTimes.map((l) => (l.flags.includes('falseStartPenalty') ? l.ms! - 5000 : l.ms!)));
    assert.ok(raw.every((ms) => ms >= 30_000 && ms <= 50_000), raw.join(' '));
    assert.ok(raw.filter((ms) => ms <= 40_000).length >= raw.length - 2);
  });

  test('crash = DNF: the crashed car is out, the race ended without it', () => {
    const race = parseRace(load('crash-dnf.json'));
    const s = fold(race.race.config, race.events, race.actions);
    assert.equal(s.phase, 'finished');
    assert.equal(s.cars.find((c) => c.lane === 3)!.status, 'crashed');
    assert.equal(s.cars.filter((c) => c.status === 'finished').length, 3);
    const undone = fold(race.race.config, race.events, []);
    assert.equal(undone.phase, 'finishing', 'without the crash-out the race waits for the car');
  });

  test('DQ mid-lap: later crossings rejected, the lap in progress is TLF', () => {
    const race = parseRace(load('dq-mid-lap.json'));
    const s = fold(race.race.config, race.events, race.actions);
    const c = s.cars.find((x) => x.lane === 2)!;
    assert.equal(c.status, 'disqualified');
    assert.equal(c.tlfLap, 3);
    assert.ok(c.rejected.length >= 2 && c.rejected.every((r) => r.reason === 'disqualified'));
    assert.equal(s.standings.at(-1)?.lane, 2);
  });

  test('reboot: one lap per car spans the reboot with no time', () => {
    const race = parseRace(load('reboot-mid-race.json'));
    const s = fold(race.race.config, race.events, race.actions);
    for (const c of s.cars) assert.equal(c.lapTimes.filter((l) => l.flags.includes('rebootGap')).length, 1, `lane ${c.lane}`);
  });
});

describe('determinism', () => {
  for (const sc of SCENARIOS) {
    test(`${sc.name} records the same bytes again`, async () => {
      const again = stringifyRace(await runScenario(sc));
      assert.equal(again, load(`${sc.name}.json`));
    });
  }
});

describe('USB pulled mid-race, continued over BLE', () => {
  test('identical standings and lap times to the same race without the unplug', () => {
    const plain = parseRace(load('laps-clean.json'));
    const unplug = parseRace(load('usb-unplug-ble.json'));
    const a = fold(plain.race.config, plain.events, plain.actions);
    const b = fold(unplug.race.config, unplug.events, unplug.actions);
    assert.deepEqual(b.standings, a.standings);
    assert.deepEqual(b.cars.map((c) => c.lapTimes.map((l) => l.ms)), a.cars.map((c) => c.lapTimes.map((l) => l.ms)));
    assert.ok(unplug.events.some((e) => e.name === 'EVT_BLE_LINK'), 'the race really went over BLE');
  });
});

describe('store and file format', () => {
  test('import into a store, fold from the store, export the same file again', async () => {
    const text = load('laps-incidents.json');
    const race = parseRace(text);
    const storage = new MemoryStorage();
    const store = await new EventStore(storage).open();
    for (const e of [...race.events].reverse()) store.appendEvent(e);          // out of order on purpose
    assert.equal(store.appendEvent(race.events[3]!), false);                    // duplicate
    store.createRace(race.race.config, race.race.from, race.race.createdAt);
    for (const a of race.actions) store.appendAction(race.race.id, a);
    await store.flush();

    const reopened = await new EventStore(storage).open();
    const input = reopened.raceInput(race.race.id);
    assert.deepEqual(fold(input.config, input.events, input.actions), fold(race.race.config, race.events, race.actions));
    const again = exportRace({ race: race.race, events: input.events, actions: input.actions, expect: race.expect, exportedAt: '2026-11-17T20:00:00.000Z' });
    assert.equal(stringifyRace(again), text);
    assert.deepEqual(reopened.resumePoint(), { bootId: race.events.at(-1)!.bootId, nextEvSeq: race.events.at(-1)!.evSeq + 1 });
  });

  test('refuses other formats and newer versions', () => {
    assert.throws(() => parseRace('{"format":"x"}'), /not a TimeTaker race file/);
    assert.throws(() => parseRace('{"format":"timetaker-race","version":99,"race":{},"events":[],"actions":[]}'), /newer/);
  });
});
