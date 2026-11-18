#!/usr/bin/env node
/*
 * tt: the TimeTaker bench client. Talks the host protocol to the control
 * module on /dev/ttyACM* (or to the simulator with --sim), reusing the web
 * app's protocol and device-session code. See tools/README.md.
 *
 *   node tools/tt.ts [--port /dev/ttyACM0 | --sim] [--json] <command> [args]
 */

import { laneMask, ALL_LANES } from '../src/device/deviceApi.ts';
import { DeviceError } from '../src/device/deviceSession.ts';
import { SimDevice } from '../src/device/simDevice.ts';
import { HealthMonitor } from '../src/health/monitor.ts';
import type { LaneSample } from '../src/health/monitor.ts';
import { BoardType, CalibrateAction, LedMode, NodeClass, NodeState, SamplingMode, Transport as HpTransport, BindState } from '../src/protocol/generated.ts';
import type { LaneConfig } from '../src/protocol/generated.ts';
import { fromHex, toHex } from '../src/util/bytes.ts';
import { realScheduler, VirtualClock } from '../src/util/clock.ts';
import { Bench, formatOutcome, formatReport, formatSample, formatStream, hex32, nameOf } from './lib/bench.ts';
import { runSuites, SUITES } from './lib/benchSuites.ts';
import { listPorts } from './lib/ttyTransport.ts';

const USAGE = `tt — TimeTaker bench client

usage: node tools/tt.ts [--port PATH | --sim [--cars]] [--seconds N] <command>

  ports                              list /dev/ttyACM* with USB identity
  hello                              HELLO and session facts
  nodes                              NODE_LIST
  status [--raw]                     health report (self check of every part); --raw: the STATUS fields
  sensors                            live per-lane readout from the health monitor (--seconds, default 3)
  calibrate LANES|all [--forget]     learn and save each lane's reference (lanes free!), or forget it
  settings [set name=NAME ble=0|1]   SETTINGS_GET / SETTINGS_SET
  lane power on|off
  lane scan
  lane config get LANE
  lane config set MASK key=value ... [--save]
  lane sampling MASK off|on|raw
  lane led MASK off|on|blink [level]
  lane status LANE
  lane raw LANE [decimation]         stream raw samples (--seconds, default 2)
  lane calibrate MASK [forget]       raw LANE_CALIBRATE (outcomes arrive as events; prefer \`calibrate\`)
  watch                              print events as they come (--seconds, default forever)
  start [lights step holdMin holdMax pulseMs pulses]
  abort
  bind list | bind allow SECONDS | bind remove HEX32 | bind clear
  reboot                             REBOOT{APP} (no bootloader in v1: flash over SWD)
  suite NAME|all                     bench suites: ${Object.keys(SUITES).join(', ')}

MASK: "1,3,5", "all" or 0x3f. --sim runs against the simulator (virtual time, except watch/raw).`;

export interface CliResult { code: number }

function parseMask(s: string | undefined): number {
  if (!s) throw new Error('lane mask missing');
  if (s === 'all') return ALL_LANES;
  if (/^0x/i.test(s)) return parseInt(s, 16);
  return laneMask(s.split(',').map(Number));
}

function int(s: string | undefined, what: string): number {
  const n = Number(s);
  if (s === undefined || !Number.isInteger(n)) throw new Error(`${what} must be an integer`);
  return n;
}

export async function main(argv: string[], out: (l: string) => void = console.log, simIn?: SimDevice): Promise<CliResult> {
  const args = [...argv];
  const flag = (f: string): boolean => { const i = args.indexOf(f); if (i < 0) return false; args.splice(i, 1); return true; };
  const opt = (f: string): string | undefined => { const i = args.indexOf(f); if (i < 0) return undefined; const v = args[i + 1]; args.splice(i, 2); return v; };
  const useSim = flag('--sim') || simIn !== undefined;
  const cars = flag('--cars');
  const json = flag('--json');
  const rawOut = flag('--raw');
  const port = opt('--port');
  const seconds = opt('--seconds');
  const [cmd, ...rest] = args;
  if (!cmd || cmd === 'help' || cmd === '--help') { out(USAGE); return { code: cmd ? 0 : 2 }; }
  if (cmd === 'ports') {
    const ps = listPorts();
    if (!ps.length) out('no /dev/ttyACM* ports');
    for (const p of ps) out(`${p.path}  ${p.vendorId?.toString(16).padStart(4, '0') ?? '????'}:${p.productId?.toString(16).padStart(4, '0') ?? '????'}  ${p.product ?? '?'}  serial ${p.serial ?? '?'}`);
    return { code: 0 };
  }

  const realTime = cmd === 'watch' || (cmd === 'lane' && rest[0] === 'raw');
  // A simulator handed in keeps its own clock (a VirtualClock in tests, so state carries across commands).
  const clock = simIn ? simIn.scheduler : useSim && !realTime ? new VirtualClock() : realScheduler;
  const sim = simIn ?? (useSim ? new SimDevice({ scheduler: clock, seed: 7 }) : null);
  const bench = new Bench({ clock, sim, port, out, interactive: process.stdin.isTTY === true });
  const print = (v: unknown): void => out(json ? JSON.stringify(v, (_k, x: unknown) => (typeof x === 'bigint' ? x.toString() : x instanceof Uint8Array ? toHex(x) : x)) : String(v));

  const run = async (): Promise<number> => {
    await bench.connect();
    const api = bench.api;
    if (sim && cars) { await api.lanePower(true); await bench.sleep(400); sim.carsPractice(); }
    const sub = rest[0];
    const monitor = (): HealthMonitor => {
      const m = new HealthMonitor({ api, scheduler: clock });
      bench.onEvent = (e) => m.onEvent(e);
      m.setConnected(true);
      m.start();
      return m;
    };
    /** --sim: the simulator starts with the lane rail off; switch it on for the lane commands. */
    const simLanes = async (): Promise<void> => {
      if (!sim || sim.lanesPowered) return;
      await api.lanePower(true);
      await bench.sleep(400);
    };
    switch (cmd) {
      case 'hello': {
        const h = bench.session.hello!;
        if (json) { print(h); break; }
        out(`protocol ${h.protoMajor}.${h.protoMinor} · ${nameOf(HpTransport, h.transport)} · ${nameOf(BindState, h.bindState)} · boot ${hex32(h.bootId)}`);
        out(`sysUs ${h.sysUs} · log ${h.logOldestSeq}..${h.logNextSeq - 1} · other sessions ${h.otherSessions} · rtt ${bench.session.rttMs()?.toFixed(1)} ms`);
        break;
      }
      case 'nodes':
        for (const n of await api.nodeList()) {
          if (json) { print(n); continue; }
          out(`${nameOf(NodeClass, n.nodeClass)} ${n.index}  ${nameOf(BoardType, n.boardType)}  ${nameOf(NodeState, n.state)}  fw ${n.fwMajor}.${n.fwMinor}.${n.fwPatch}+${n.fwGitRev.toString(16)}  proto .${n.protoMinor}  crc ${hex32(n.imageCrc)}`);
        }
        break;
      case 'status': {
        if (rawOut) {
          const s = await api.status();
          if (json) print(s); else for (const [k, v] of Object.entries(s)) out(`${k.padEnd(16)} ${String(v)}`);
          break;
        }
        const mon = monitor();
        const r = await mon.runSelfCheck();
        mon.stop();
        if (json) print(r); else for (const l of formatReport(r)) out(l);
        return r.overall === 'fail' ? 1 : 0;
      }
      case 'sensors': {
        await simLanes();
        const mon = monitor();
        const ms = Number(seconds ?? 3) * 1000;
        let next = clock.now() + 1000;
        const latest = new Map<number, LaneSample>();
        mon.subscribe((u) => {
          if (u.type === 'sample') latest.set(u.lane, u.sample);
          if (u.type === 'marker') out(`  * lane ${u.lane}: ${u.marker.label}`);
        });
        const end = clock.now() + ms;
        while (clock.now() < end) {
          await bench.sleep(Math.max(1, next - clock.now()));
          next += 1000;
          if (json) { print(Object.fromEntries(latest)); continue; }
          if (!latest.size) out('no lane present (lane power on?)');
          for (const [lane, smp] of [...latest].sort((a, b) => a[0] - b[0])) out(formatSample(lane, smp));
        }
        mon.stop();
        break;
      }
      case 'calibrate': {
        await simLanes();
        const forget = rest.includes('--forget');
        const which = rest.filter((x) => x !== '--forget')[0];
        if (!which) throw new Error('calibrate LANES|all [--forget]');
        const lanes = which === 'all' ? 'all' as const : which.split(',').map((x) => int(x, 'lane'));
        const mon = monitor();
        await mon.runSelfCheck();
        out(forget ? 'forgetting the saved references…' : 'learning (keep the lanes free; the save can take up to 10 s)…');
        const res = forget ? await mon.forget(lanes) : await mon.calibrate(lanes);
        mon.stop();
        if (json) print(res); else for (const o of res) out(formatOutcome(o));
        return res.some((o) => o.result === 'failed') ? 1 : 0;
      }
      case 'settings':
        if (sub === 'set') {
          const cur = await api.settingsGet();
          const kv = Object.fromEntries(rest.slice(1).map((a) => a.split('=') as [string, string]));
          await api.settingsSet({ deviceName: kv['name'] ?? cur.deviceName, bleEnabled: kv['ble'] !== undefined ? kv['ble'] === '1' : cur.bleEnabled === 1 });
        }
        print(json ? await api.settingsGet() : JSON.stringify(await api.settingsGet(), (_k, x: unknown) => (x instanceof Uint8Array ? undefined : x)));
        break;
      case 'lane': {
        const a = rest[1];
        switch (sub) {
          case 'power': out(`powered: ${String(await api.lanePower(a === 'on'))} (settles in ~30 ms; lanes join after)`); break;
          case 'scan': await api.laneScan(); out('scan started (watch for LEFT / JOINED / CONFIGURED)'); break;
          case 'config': {
            if (a === 'get') { print(json ? await api.laneConfigGet(int(rest[2], 'lane')) : JSON.stringify(await api.laneConfigGet(int(rest[2], 'lane')))); break; }
            if (a !== 'set') throw new Error('lane config get|set');
            const mask = parseMask(rest[2]);
            const first = Math.log2(mask & -mask) + 1;
            const cfg: Record<string, number> = { ...(await api.laneConfigGet(first)) };
            for (const kv of rest.slice(3)) { const [k, v] = kv.split('='); if (!k || !(k in cfg)) throw new Error(`unknown config key ${k}`); cfg[k] = int(v, k); }
            await api.laneConfigSet(mask, cfg as unknown as LaneConfig, flag('--save') || rest.includes('--save'));
            out('ok');
            break;
          }
          case 'sampling': {
            const mode = { off: SamplingMode.OFF, on: SamplingMode.ON, raw: SamplingMode.RAW }[rest[2] as 'off'];
            if (mode === undefined) throw new Error('sampling off|on|raw');
            await api.laneSampling(parseMask(a), mode);
            out('ok');
            break;
          }
          case 'led': {
            const mode = { off: LedMode.OFF, on: LedMode.ON, blink: LedMode.BLINK }[rest[2] as 'off'];
            if (mode === undefined) throw new Error('led off|on|blink');
            await api.laneLed(parseMask(a), { mode, level: rest[3] ? int(rest[3], 'level') : undefined, blinkPeriodMs: 500, blinkOnPermille: 500 });
            out('ok');
            break;
          }
          case 'status': {
            const s = await api.laneStatus(int(a, 'lane'));
            if (json) print(s); else for (const [k, v] of Object.entries(s)) out(`${k.padEnd(14)} ${String(v)}`);
            break;
          }
          case 'raw': {
            const lane = int(a, 'lane');
            await api.laneRawStream(lane, rest[2] ? int(rest[2], 'decimation') : 1);
            const mark = bench.streams.length;
            await bench.sleep(Number(seconds ?? 2) * 1000);
            await api.laneRawStream(0);
            for (const s of bench.streams.slice(mark)) out(formatStream(s));
            break;
          }
          case 'calibrate': await api.laneCalibrate(parseMask(a), rest[2] === 'forget' ? CalibrateAction.FORGET : CalibrateAction.LEARN); out('ok (outcomes follow as EVT_LANE_CALIBRATED)'); break;
          default: throw new Error('lane power|scan|config|sampling|led|status|raw|calibrate');
        }
        break;
      }
      case 'watch': {
        bench.watch = true;
        for (const e of bench.events) out((await import('./lib/bench.ts')).formatEvent(e));
        const ms = seconds ? Number(seconds) * 1000 : Number.POSITIVE_INFINITY;
        await new Promise<void>((resolve) => {
          if (Number.isFinite(ms)) clock.setTimeout(resolve, ms);
          bench.session.closed().then(() => resolve(), () => resolve());
          process.once('SIGINT', () => resolve());
        });
        break;
      }
      case 'start': {
        const [l, st, hmin, hmax, pm, np] = rest.map(Number);
        const t0 = await api.startSequence({ lights: l || 6, stepMs: st || 1000, holdMinMs: hmin || 200, holdMaxMs: hmax || 3000,
          pulseRiseMs: pm || 1000, attentionPulses: rest[5] === undefined ? 2 : np });
        out(`T0 = ${t0} µs (lights out after the hold; watch for EVT_LIGHTS_OUT)`);
        break;
      }
      case 'abort': await api.abortSequence(); out('aborted'); break;
      case 'bind':
        if (sub === 'allow') await api.bindAllow(int(a, 'seconds'));
        else if (sub === 'remove') await api.bindRemove(fromHex(a ?? ''));
        else if (sub === 'clear') await api.bindClear();
        else if (sub !== 'list' && sub !== undefined) throw new Error('bind list|allow|remove|clear');
        {
          const list = await api.bindList();
          out(`pairing window: ${list.windowS ? `${list.windowS} s` : 'closed'} · ${list.entries.length} bound`);
          for (const e of list.entries) out(`  ${toHex(e.clientId)}  ${e.clientName}  lru ${e.lastUsed}`);
        }
        break;
      case 'reboot': await api.reboot(); out('rebooting'); break;
      case 'suite': {
        const names = !sub || sub === 'all' ? Object.keys(SUITES) : sub.split(',');
        const results = await runSuites(bench, names);
        if (json) print(results);
        return results.some((r) => r.error || r.checks.some((x) => !x.ok)) ? 1 : 0;
      }
      default:
        out(USAGE);
        return 2;
    }
    return 0;
  };

  try {
    const code = clock instanceof VirtualClock ? await clock.run(run) : await run();
    return { code };
  } catch (e) {
    out(`error: ${e instanceof DeviceError ? `${e.msgName} → ${e.statusName}` : e instanceof Error ? e.message : String(e)}`);
    return { code: 1 };
  } finally {
    await bench.close().catch(() => undefined);
    if (!simIn) sim?.dispose();
  }
}

if (import.meta.url === `file://${process.argv[1]}`) {
  const r = await main(process.argv.slice(2));
  process.exit(r.code);
}
