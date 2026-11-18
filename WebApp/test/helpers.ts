/*
 * Shared test rig: a VirtualClock, a SimDevice on it, sessions on it, and
 * recorders. Tests run their body inside clock.run(), so simulated time
 * moves as fast as the code allows (a 60 s bind timeout takes milliseconds).
 */

import { DeviceSession } from '../src/device/deviceSession.ts';
import type { DeviceEvent, DeviceStream, ResumePoint, SessionListener, SessionOptions } from '../src/device/deviceSession.ts';
import { SimDevice } from '../src/device/simDevice.ts';
import type { SimOptions } from '../src/device/simDevice.ts';
import { ClientKind } from '../src/protocol/generated.ts';
import { sleep, VirtualClock, waitFor } from '../src/util/clock.ts';

export interface Recorder extends SessionListener {
  events: DeviceEvent[];
  streams: DeviceStream[];
  boots: number[];
  gaps: [number, number][];
  states: string[];
}

export function recorder(): Recorder {
  const r: Recorder = {
    events: [], streams: [], boots: [], gaps: [], states: [],
    onEvent: (e) => { r.events.push(e); },
    onStream: (s) => { r.streams.push(s); },
    onBoot: (b) => { r.boots.push(b); },
    onGap: (_b, from, to) => { r.gaps.push([from, to]); },
    onState: (s) => { r.states.push(s); },
  };
  return r;
}

export const named = (r: Recorder, name: string): DeviceEvent[] => r.events.filter((e) => e.name === name);

export class Rig {
  readonly clock = new VirtualClock();
  readonly sims: SimDevice[] = [];
  readonly sessions: DeviceSession[] = [];

  sim(o: SimOptions = {}): SimDevice {
    const d = new SimDevice({ seed: 42, ...o, scheduler: this.clock });
    this.sims.push(d);
    return d;
  }

  session(dev: SimDevice, rec: SessionListener = {}, o: Partial<SessionOptions> & { kind?: 'usb' | 'ble'; resume?: ResumePoint | null } = {}): DeviceSession {
    const s = new DeviceSession(dev.transport(o.kind ?? 'usb'), {
      clientId: new Uint8Array(16).fill(7), clientName: 'test', clientKind: ClientKind.TOOL,
      requestTimeoutMs: 500, pingIntervalMs: 1000, ...o, scheduler: this.clock,
    }, rec, o.resume ?? null);
    this.sessions.push(s);
    return s;
  }

  sleep(ms: number): Promise<void> { return sleep(this.clock, ms); }
  until(cond: () => boolean, ms = 5000, what = 'condition'): Promise<void> { return waitFor(this.clock, cond, ms, what, 1); }

  /** Runs body on auto-advancing virtual time, then cleans up. */
  async run(body: () => Promise<void>): Promise<void> {
    await this.clock.run(async () => {
      try {
        await body();
      } finally {
        for (const s of this.sessions.splice(0)) await s.close();
        for (const d of this.sims.splice(0)) d.dispose();
      }
    });
  }
}

export function rig(): Rig { return new Rig(); }
