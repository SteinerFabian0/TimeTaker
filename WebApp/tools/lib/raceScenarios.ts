/*
 * Recorded races, made by the simulator through the whole stack:
 *
 *   SimDevice (cars) → transport → DeviceSession/Manager → EventStore
 *                    ← RaceDirector (power, lanes, start, corrections)
 *
 * on virtual time, with fixed seeds, so the same scenario always produces the
 * same bytes. tools/genFixtures.ts writes them to test/fixtures/races/; the
 * fixture test folds them and checks the result against `expect`.
 *
 * `expect` is NOT computed by the race engine. It comes from the simulator's
 * ground truth (every real car crossing, which ones were doubles, false
 * starts, blocks) through `oracle()` below, a deliberately simple and
 * separate implementation of RACE_RULES.md: "count the real crossings, apply
 * the flag". Host actions in a scenario adjust the expectation by hand
 * (a disqualification or crash-out gives the device time it happened at).
 *
 * The cars are race drivers (raceCarProfiles: 30–40 s laps).
 */

import { DeviceManager } from '../../src/device/deviceManager.ts';
import type { DeviceEvent } from '../../src/device/deviceSession.ts';
import { SimDevice } from '../../src/device/simDevice.ts';
import type { CarProfile, CarScript, TruthCrossing } from '../../src/device/simDevice.ts';
import { raceCarProfiles } from '../../src/device/sim/simCars.ts';
import { RaceDirector } from '../../src/director/raceDirector.ts';
import { ClientKind, LaneState, MsgId } from '../../src/protocol/generated.ts';
import { resolveConfig } from '../../src/race/rules/config.ts';
import type { RaceConfigInput } from '../../src/race/rules/config.ts';
import type { RaceConfig } from '../../src/race/types.ts';
import { EventStore } from '../../src/store/eventStore.ts';
import { exportRace } from '../../src/store/raceFile.ts';
import type { RaceFile } from '../../src/store/raceFile.ts';
import { sleep, VirtualClock, waitFor } from '../../src/util/clock.ts';

export interface CarExpect {
  laps: number;
  status: 'finished' | 'running' | 'disqualified' | 'retired' | 'crashed';
  falseStart: boolean;
  reactionMs: number | null;
  /** null entries: the lap spans a reboot. */
  lapMs: (number | null)[];
  /** Before penalties; omitted across reboots. */
  elapsedMs?: number;
  penaltiesMs: number;
  /** Disqualified mid-lap: the aborted lap's number (shown TLF). */
  tlfLap: number | null;
}

export interface RaceExpect {
  phase: string;
  order: number[];
  cars: Record<string, CarExpect>;
  timingGaps: number;
  aborts: number;
}

export interface ScenarioCtx {
  clock: VirtualClock;
  dev: SimDevice;
  manager: DeviceManager;
  store: EventStore;
  director: RaceDirector;
  config: RaceConfig;
  sleep(ms: number): Promise<void>;
  until(cond: () => boolean, ms: number, what: string): Promise<void>;
  /** Number of truth entries before the (final) start: the oracle looks after it. */
  truthFrom: number;
  /** Manual changes to the expectation (host actions). outFromUs: device time (start boot) of a DQ / crash-out. */
  adjust: Partial<Record<number, Partial<Pick<CarExpect, 'penaltiesMs' | 'status'>> & { outFromUs?: number }>>;
}

export interface Scenario {
  name: string;
  description: string;
  seed: number;
  lanes: number;
  config: RaceConfigInput;
  cars?: Record<number, Partial<CarProfile>>;
  scripts?: Record<number, CarScript>;
  /** Start parameter overrides (default: the race config's startLights). */
  start?: { stepMs?: number; holdMinMs?: number; holdMaxMs?: number };
  /** Runs while the race runs, after the start (the default just waits). */
  during?(ctx: ScenarioCtx): Promise<void>;
  /** Replaces the default "start once" (aborted starts). */
  starting?(ctx: ScenarioCtx): Promise<void>;
}

const FIXED_TIME = '2026-11-17T20:00:00.000Z';

export async function runScenario(sc: Scenario): Promise<RaceFile> {
  const clock = new VirtualClock();
  const race = raceCarProfiles(sc.seed);
  const cars: Record<number, Partial<CarProfile>> = {};
  for (let l = 1; l <= 6; l++) cars[l] = { ...race[l], ...(sc.cars?.[l] ?? {}) };
  const dev = new SimDevice({ seed: sc.seed, scheduler: clock, lanes: sc.lanes, cars });
  for (const [lane, s] of Object.entries(sc.scripts ?? {})) dev.setCarScript(Number(lane), s);
  const store = new EventStore();
  await store.open();
  const manager = new DeviceManager({
    clientId: new Uint8Array(16).fill(0x42), clientName: 'fixture', clientKind: ClientKind.WEBAPP, scheduler: clock,
    requestTimeoutMs: 800, reconnectMs: 300, preferCheckMs: 1000,
    sources: [
      { kind: 'usb', find: async () => (dev.usbPlugged ? dev.transport('usb') : null) },
      { kind: 'ble', find: async () => dev.transport('ble') },
    ],
  }, { onEvent: (e: DeviceEvent) => { store.appendEvent(e); } });
  const config = resolveConfig(sc.config);
  let ids = 0;
  const director = new RaceDirector({
    api: manager.api, store, raceId: config.id, newId: () => `act${++ids}`, wallClock: () => FIXED_TIME,
  });
  const ctx: ScenarioCtx = {
    clock, dev, manager, store, director, config, truthFrom: 0, adjust: {},
    sleep: (ms) => sleep(clock, ms),
    until: (cond, ms, what) => waitFor(clock, cond, ms, what, 100),
  };

  try {
    await clock.run(async () => {
      manager.start();
      await ctx.until(() => manager.state === 'connected', 5000, 'connection');
      await director.powerLanes(true);
      const configured = (): number => store.events.filter((e) => e.msgId === MsgId.EVT_LANE_STATE && e.value?.['state'] === LaneState.CONFIGURED).length;
      await ctx.until(() => configured() >= sc.lanes, 5000, 'lanes');
      await ctx.sleep(2000);
      store.createRace(config, null, FIXED_TIME);
      await director.prepareLanes();
      const lanes = config.entries.map((e) => e.lane);
      dev.carsRace(lanes, config.mode === 'laps' ? config.laps + 5 : Number.POSITIVE_INFINITY);
      ctx.truthFrom = dev.truth().length;
      if (sc.starting) await sc.starting(ctx);
      else await director.start({ ...sc.start });
      if (sc.during) await sc.during(ctx);
      await ctx.until(() => director.state().phase === 'finished', 30 * 60_000, 'the race to finish');
      await ctx.sleep(3000);
      dev.carsStop();
      await manager.stop();
    });
  } finally {
    dev.dispose();
  }

  const input = store.raceInput(config.id);
  return exportRace({
    race: { id: config.id, config, from: store.race(config.id).from, createdAt: FIXED_TIME },
    events: input.events, actions: input.actions, exportedAt: FIXED_TIME,
    expect: oracle(config, dev.truth().slice(ctx.truthFrom), store, ctx.adjust),
  });
}

/** The expected result from the simulator's ground truth (independent of race/). */
export function oracle(config: RaceConfig, truth: readonly TruthCrossing[], store: EventStore,
                       adjust: ScenarioCtx['adjust']): RaceExpect {
  const evs = store.events;
  const boots: number[] = [];
  for (const e of evs) if (!boots.includes(e.bootId)) boots.push(e.bootId);
  const out = [...evs].reverse().find((e) => e.msgId === MsgId.EVT_LIGHTS_OUT);
  if (!out) throw new Error('scenario never started');
  const toutUs = Number(out.value!['toutUs']);
  const startBoot = out.bootId;
  const aborts = evs.filter((e) => e.msgId === MsgId.EVT_SEQ_ABORTED).length;
  const ord = (c: { bootId: number }): number => boots.indexOf(c.bootId);
  const later = (a: TruthCrossing, b: TruthCrossing): boolean => ord(a) > ord(b) || (ord(a) === ord(b) && a.tEntryUs > b.tEntryUs);
  const raceMs = (c: TruthCrossing): number | null => (c.bootId === startBoot ? (c.tEntryUs - toutUs) / 1000 : null);

  // Every real, detected crossing (a stop on the line counts; a re-slotted
  // car's second pass is not a lap), from T0 of the start sequence on.
  const t0Us = Number([...evs].reverse().find((e) => e.msgId === MsgId.EVT_SEQ_STARTED && e.bootId === startBoot)!.value!['t0Us']);
  const fsPenalty = config.falseStart.action === 'timePenalty' ? config.falseStart.penaltyMs : 0;
  const perLane = new Map<number, TruthCrossing[]>();
  for (const e of config.entries) {
    let cs = truth.filter((c) => c.lane === e.lane && c.detected && c.kind !== 'double' && !(c.bootId === startBoot && c.tEntryUs < t0Us));
    const fs = cs[0]?.kind === 'falseStart';
    const out = adjust[e.lane]?.outFromUs;
    if (out !== undefined) {
      cs = cs.filter((c) => c.bootId !== startBoot || c.tEntryUs <= out);
    }
    perLane.set(e.lane, cs);
  }

  // The flag.
  let flag: TruthCrossing | null = null;
  for (const [, cs] of perLane) {
    const counted = cs.slice(1);
    const c = config.mode === 'laps' ? counted[config.laps - 1] : counted.find((x) => (raceMs(x) ?? 0) >= config.durationMs);
    if (c && (!flag || later(flag, c))) flag = c;
  }

  const cars: Record<string, CarExpect> = {};
  const best = new Map<number, number>();
  for (const e of config.entries) {
    const cs = perLane.get(e.lane)!;
    const first = cs[0] ?? null;
    let counted = cs.slice(1);
    let finished = false;
    if (flag) {
      const fin = config.mode === 'laps'
        ? counted.findIndex((c) => c === flag || later(c, flag!))
        : counted.findIndex((c) => (raceMs(c) ?? 0) >= config.durationMs);
      if (fin >= 0) { counted = counted.slice(0, fin + 1); finished = true; }
    }
    const fs = first?.kind === 'falseStart';
    const lapMs = counted.map((c, i) => {
      const prev = i === 0 ? first! : counted[i - 1]!;
      if (prev.bootId !== c.bootId) return null;
      const ms = (c.tEntryUs - prev.tEntryUs) / 1000;
      return i === 0 && fs && config.falseStart.apply === 'addToFirstLap' ? ms + fsPenalty : ms;
    });
    const timed = lapMs.filter((x): x is number => x !== null);
    if (timed.length) best.set(e.lane, Math.min(...timed));
    const last = counted.at(-1);
    const a = adjust[e.lane] ?? {};
    const dq = a.status === 'disqualified';
    const car: CarExpect = {
      laps: counted.length, status: dq ? 'disqualified' : a.status ?? (finished ? 'finished' : 'running'), falseStart: fs,
      reactionMs: first && first.bootId === startBoot ? (first.tEntryUs - toutUs) / 1000 : null, lapMs,
      penaltiesMs: (a.penaltiesMs ?? 0) + (fs ? fsPenalty : 0),
      tlfLap: dq && first && !finished ? counted.length + 1 : null,
    };
    if (last && boots.length === 1) car.elapsedMs = raceMs(last)!;
    cars[String(e.lane)] = car;
  }

  // More laps, then lower total (with penalties), then faster best lap, then lane; disqualified last.
  const order = config.entries.map((e) => e.lane).sort((x, y) => {
    const a = cars[String(x)]!;
    const b = cars[String(y)]!;
    const dq = (c: CarExpect): number => (c.status === 'disqualified' ? 1 : 0);
    if (dq(a) !== dq(b)) return dq(a) - dq(b);
    if (a.laps !== b.laps) return b.laps - a.laps;
    const ta = perLane.get(x)!.slice(1)[a.laps - 1];
    const tb = perLane.get(y)!.slice(1)[b.laps - 1];
    const tie = (best.get(x) ?? Infinity) - (best.get(y) ?? Infinity) || x - y;
    if (ta && tb && ta.bootId === tb.bootId && boots.length === 1) {
      return (ta.tEntryUs / 1000 + a.penaltiesMs) - (tb.tEntryUs / 1000 + b.penaltiesMs) || tie;
    }
    if (ta && tb) return later(ta, tb) ? 1 : later(tb, ta) ? -1 : tie;
    return tie;
  });

  return { phase: 'finished', order, cars, timingGaps: boots.indexOf(startBoot) >= 0 ? boots.length - 1 - boots.indexOf(startBoot) : 0, aborts };
}

// ---- the scenarios ------------------------------------------------------------------

const six = [1, 2, 3, 4, 5, 6].map((lane) => ({ lane, driver: ['Anna', 'Ben', 'Cleo', 'Dan', 'Eva', 'Finn'][lane - 1]! }));

/** Device time (µs, current boot) a few seconds after a lane's n-th crossing of this race: mid-lap. */
async function midLap(ctx: ScenarioCtx, lane: number, n: number, afterMs: number): Promise<number> {
  const mine = (): TruthCrossing[] => ctx.dev.truth().slice(ctx.truthFrom).filter((c) => c.lane === lane && c.kind !== 'double');
  await ctx.until(() => mine().length >= n, 10 * 60_000, `lane ${lane} crossing ${n}`);
  await ctx.sleep(afterMs);
  return Number(ctx.dev.sysUs());
}

export const SCENARIOS: Scenario[] = [
  {
    name: 'laps-clean',
    description: '6 cars, 6 laps of 30–40 s, default settings, nothing special (random deslots only)',
    seed: 1001, lanes: 6,
    config: { id: 'laps-clean', name: 'Clean 6 laps', mode: 'laps', laps: 6, entries: six },
  },
  {
    name: 'laps-incidents',
    description: '6 cars, 8 laps: lane 2 false start (5 s added to its first lap), re-slotted cars behind the line (double crossings), a deslot and replace, a car stopped on the line, adaptive holdoff, a penalty and an undone +1 lap',
    seed: 2002, lanes: 6,
    config: { id: 'laps-incidents', name: 'Incidents', mode: 'laps', laps: 8, entries: six },
    scripts: {
      2: { falseStart: true },
      3: { incidents: [{ afterCrossing: 3, kind: 'double' }] },
      4: { incidents: [{ afterCrossing: 5, kind: 'block' }] },
      5: { incidents: [{ afterCrossing: 2, kind: 'double' }, { afterCrossing: 6, kind: 'deslot' }] },
    },
    async during(ctx) {
      await ctx.until(() => ctx.director.state().fastestLap !== null, 120_000, 'a first lap');
      await ctx.director.applyAdaptiveHoldoff(0.55);
      await ctx.sleep(40_000);
      ctx.director.penalty(1, 2000, 'cut the chicane');
      const plus = ctx.director.adjustLaps(6, 1, 'missed crossing?');
      await ctx.sleep(5000);
      ctx.director.undo(plus.id, 'no, it was fine');
      ctx.adjust[1] = { penaltiesMs: 2000 };
    },
  },
  {
    name: 'time-race',
    description: '4 cars, 150 s time race: each car finishes on its first crossing after the time',
    seed: 3003, lanes: 4,
    config: { id: 'time-race', name: '150 s', mode: 'time', durationMs: 150_000, entries: six.slice(0, 4) },
  },
  {
    name: 'abort-restart',
    description: '3 cars; the first start is aborted after lane 1 jumps it, the second start runs 4 laps',
    seed: 4004, lanes: 3,
    config: { id: 'abort-restart', name: 'Restart', mode: 'laps', laps: 4, entries: six.slice(0, 3) },
    scripts: { 1: { falseStart: true } },
    async starting(ctx) {
      await ctx.director.start();
      await ctx.until(() => ctx.dev.truth().slice(ctx.truthFrom).some((c) => c.kind === 'falseStart'), 20_000, 'the jump');
      await ctx.director.abortStart();
      await ctx.sleep(3000);
      ctx.dev.setCarScript(1, { falseStart: false });
      ctx.dev.carsRace([1, 2, 3], 9);
      ctx.truthFrom = ctx.dev.truth().length;
      await ctx.director.start();
    },
  },
  {
    name: 'reboot-mid-race',
    description: '3 cars, 5 laps; the control module reboots after ~2 laps; lanes are powered again and the race goes on (flagged timing gap)',
    seed: 5005, lanes: 3,
    config: { id: 'reboot-mid-race', name: 'Reboot', mode: 'laps', laps: 5, entries: six.slice(0, 3) },
    async during(ctx) {
      await ctx.sleep(90_000);
      const before = ctx.dev.bootId;
      await ctx.manager.api.reboot();
      await ctx.until(() => ctx.dev.bootId !== before, 2000, 'the reboot');
      await ctx.until(() => ctx.manager.state === 'connected' && ctx.manager.session?.hello?.bootId === ctx.dev.bootId, 10_000, 'reconnect');
      await ctx.director.powerLanes(true);
      await ctx.sleep(500);
    },
  },
  {
    name: 'usb-unplug-ble',
    description: '6 cars, 6 laps; USB pulled mid-race, the app continues over BLE and goes back to USB when it is plugged in (same seed as laps-clean: identical standings)',
    seed: 1001, lanes: 6,
    config: { id: 'laps-clean', name: 'Clean 6 laps', mode: 'laps', laps: 6, entries: six },
    async during(ctx) {
      await ctx.sleep(60_000);
      ctx.dev.unplugUsb();
      await ctx.sleep(60_000);
      ctx.dev.plugUsb();
    },
  },
  {
    name: 'crash-dnf',
    description: '4 cars, 5 laps, crash policy crash = DNF: lane 3 crashes out after its 3rd crossing, the host marks it crashed/out, the race ends when the other three have finished',
    seed: 6006, lanes: 4,
    config: { id: 'crash-dnf', name: 'Crash = DNF', mode: 'laps', laps: 5, crashPolicy: 'crashIsDnf', entries: six.slice(0, 4) },
    scripts: { 3: { incidents: [{ afterCrossing: 3, kind: 'crash' }] } },
    async during(ctx) {
      const at = await midLap(ctx, 3, 3, 15_000);
      ctx.director.crashOut(3, 'off the track at the hairpin');
      ctx.adjust[3] = { status: 'crashed', outFromUs: at };
    },
  },
  {
    name: 'dq-mid-lap',
    description: '3 cars, 5 laps: lane 2 is disqualified 12 s into its 3rd lap; it keeps driving (no power board yet), nothing after counts, lap 3 is TLF',
    seed: 7007, lanes: 3,
    config: { id: 'dq-mid-lap', name: 'Disqualified', mode: 'laps', laps: 5, entries: six.slice(0, 3) },
    async during(ctx) {
      const at = await midLap(ctx, 2, 3, 12_000);
      ctx.director.disqualify(2, 'dangerous driving');
      ctx.adjust[2] = { status: 'disqualified', outFromUs: at };
    },
  },
];
