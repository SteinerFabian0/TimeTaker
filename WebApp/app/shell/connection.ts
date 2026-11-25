// The connection state every page reads. app/lib/device.ts fills it from the
// DeviceManager (src/device) and the health monitor (src/health).
import { signal } from '@preact/signals';

export type Link = 'offline' | 'usb' | 'ble' | 'sim';
export type Overall = 'ok' | 'warn' | 'fail';

export const connection = signal<{ link: Link; overall: Overall; powerPack: boolean }>({
  link: 'offline', overall: 'fail', powerPack: false,
});

/**
 * The connected time taker, for pages that act on the track (Track Setup's
 * start light preview, the Grand Prix's lane lights and warm-up). Null while none is connected.
 * app/lib/device.ts fills it: `lanes` = present lanes from the node model once stable for 3 s (0 = none / unknown),
 * `seqRunning` from it too, `subscribe` = DeviceManager.subscribe({ onEvent }).
 */
export interface TrackLink {
  readonly api: Pick<import('../../src/device/deviceApi.ts').DeviceApi, 'startSequence' | 'abortSequence' | 'laneLed' | 'laneSampling'>;
  readonly lanes: number;
  readonly seqRunning: boolean;
  /** The device's events (EVT_CROSSING, EVT_LIGHTS_OUT …) from now on, as the race engine reads them; returns unsubscribe. */
  readonly subscribe: (onEvent: (e: import('../../src/race/types.ts').RaceInputEvent) => void) => () => void;
}
export const trackLink = signal<TrackLink | null>(null);

/** True while a race session runs: nothing else may drive the lights then. */
export const raceSessionActive = signal(false);
