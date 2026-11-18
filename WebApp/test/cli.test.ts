/*
 * The bench CLI: every suite against the simulator, a few commands, and the
 * real tty code path through a socat pty pair (skipped without socat).
 */

import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { existsSync, mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { describe, test } from 'node:test';
import { execFileSync } from 'node:child_process';

import { SimDevice } from '../src/device/simDevice.ts';
import { VirtualClock } from '../src/util/clock.ts';
import { TtyTransport, writeAll } from '../tools/lib/ttyTransport.ts';
import { main } from '../tools/tt.ts';

async function cli(args: string[], sim?: SimDevice): Promise<{ code: number; out: string[] }> {
  const out: string[] = [];
  const r = await main(args, (l) => out.push(l), sim);
  return { code: r.code, out };
}

describe('tt --sim', () => {
  test('every bench suite passes against the simulator', async () => {
    const r = await cli(['--sim', 'suite', 'all']);
    assert.equal(r.code, 0, r.out.filter((l) => /FAIL/.test(l)).join('\n'));
    assert.match(r.out.at(-1)!, /checks, 0 failed/);
  });

  test('commands: hello, nodes, bind list, lane errors, usage', async () => {
    assert.match((await cli(['--sim', 'hello'])).out[0]!, /protocol 0\.6 · USB · TRUSTED/);
    assert.ok((await cli(['--sim', 'nodes'])).out.some((l) => l.startsWith('CTRL 0  CONTROL')));
    assert.match((await cli(['--sim', 'bind', 'list'])).out[0]!, /pairing window: closed · 0 bound/);
    const bad = await cli(['--sim', 'lane', 'status', '1']);
    assert.equal(bad.code, 1);
    assert.match(bad.out[0]!, /LANE_STATUS → ERR_LANE_ABSENT/);
    assert.equal((await cli([])).code, 2);
  });

  test('status (health report), sensors, calibrate / --forget', async () => {
    const st = await cli(['--sim', 'status']);
    assert.equal(st.code, 0, st.out.join('\n'));
    assert.equal(st.out[0], 'overall: WARN');                                   // the simulator boots with lane power off
    assert.ok(st.out.includes('control module: online'));
    assert.ok(st.out.includes('BLE chip: online, no phone connected'));
    assert.ok(st.out.some((l) => /WARN ctrl\.lanePower\s+lane power off/.test(l)));
    assert.ok((await cli(['--sim', 'status', '--raw'])).out.some((l) => /^bleBridge\s+1$/.test(l)));
    const json = JSON.parse((await cli(['--sim', '--json', 'status'])).out[0]!) as { lanes: unknown[] };
    assert.equal(json.lanes.length, 6);

    const sim = new SimDevice({ seed: 5, lanes: 2, scheduler: new VirtualClock() });
    try {
      const sens = await cli(['sensors', '--seconds', '2'], sim);
      assert.equal(sens.code, 0, sens.out.join('\n'));
      assert.equal(sens.out.filter((l) => /^lane 1 +raw +\d+ \[ *\d+\.\. *\d+\] +base +\d+ +thr \d+\/\d+ +ref +\d+ +noise/.test(l)).length, 2);
      const cal = await cli(['calibrate', 'all'], sim);
      assert.equal(cal.code, 0, cal.out.join('\n'));
      assert.match(cal.out.at(-2)!, /^lane 1: saved reference \d+, noise \d+$/);
      const fg = await cli(['calibrate', '2', '--forget'], sim);
      assert.match(fg.out.at(-1)!, /^lane 2: saved reference forgotten/);
      const bad = await cli(['calibrate', '5'], sim);
      assert.equal(bad.code, 1);
      assert.match(bad.out.at(-1)!, /^lane 5: FAILED/);
    } finally {
      sim.dispose();
    }
  });
});

const hasSocat = ((): boolean => { try { execFileSync('socat', ['-V'], { stdio: 'ignore' }); return true; } catch { return false; } })();

describe('tt over a real tty', { skip: !hasSocat && 'socat not installed' }, () => {
  test('HELLO and NODE_LIST through a pty pair to the simulator', async () => {
    const dir = mkdtempSync(join(tmpdir(), 'tt-pty-'));
    const devPath = join(dir, 'device');
    const hostPath = join(dir, 'host');
    const socat = spawn('socat', [`pty,raw,echo=0,link=${devPath}`, `pty,raw,echo=0,link=${hostPath}`], { stdio: 'ignore' });
    const sim = new SimDevice({ seed: 3 });
    const devTty = new TtyTransport(devPath);
    try {
      for (let i = 0; i < 100 && !(existsSync(devPath) && existsSync(hostPath)); i++) await new Promise((r) => setTimeout(r, 20));
      const link = sim.transport('usb');
      await link.open({ onBytes: (b) => { void devTty.write(b); }, onClose: () => undefined });
      await devTty.open({ onBytes: (b) => { void link.write(b); }, onClose: () => undefined });
      const hello = await cli(['--port', hostPath, 'hello']);
      assert.equal(hello.code, 0, hello.out.join('\n'));
      assert.match(hello.out[0]!, /protocol 0\.6 · USB · TRUSTED/);
      const nodes = await cli(['--port', hostPath, 'nodes']);
      assert.ok(nodes.out.some((l) => l.startsWith('CTRL 0')));
    } finally {
      await devTty.close();
      sim.dispose();
      socat.kill();
      rmSync(dir, { recursive: true, force: true });
    }
  });
});

describe('tty writes', () => {
  test('writeAll finishes partial writes, in order', async () => {
    const out: number[] = [];
    await writeAll(async (buf, off) => { const n = Math.min(3, buf.length - off); out.push(...buf.subarray(off, off + n)); return n; },
      Uint8Array.from({ length: 10 }, (_, i) => i));
    assert.deepEqual(out, [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]);
  });
  test('writeAll stops when the port is gone and when it accepts nothing', async () => {
    await assert.rejects(writeAll(async () => 1, new Uint8Array(5), () => false), /closed/);
    await assert.rejects(writeAll(async () => 0, new Uint8Array(5)), /no bytes/);
  });
});
