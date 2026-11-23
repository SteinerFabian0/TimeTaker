// What the connection badge shows. The shell only holds the state; wiring it
// to DeviceManager (src/device) and the health monitor comes with Status.
import { signal } from '@preact/signals';

export type Link = 'offline' | 'usb' | 'ble' | 'sim';
export type Overall = 'ok' | 'warn' | 'fail';

export const connection = signal<{ link: Link; overall: Overall; powerPack: boolean }>({
  link: 'offline', overall: 'fail', powerPack: false,
});
