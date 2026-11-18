/*
 * The simulator as a Transport: one simulated cable (USB) or radio link (BLE)
 * to a SimDevice. The device session cannot tell it from the real thing
 * (SYSTEM_ARCHITECTURE §11: "the simulator is a first-class transport").
 *
 * open() fails like the real transports do when the far end is not there:
 * USB unplugged or re-enumerating after a reboot, BLE disabled in the
 * settings or another central already connected.
 */

import type { Transport, TransportHandlers } from './transport.ts';

/** What SimTransport needs from the simulator (SimDevice implements it). */
export interface SimLinkHost<L> {
  attach(kind: 'usb' | 'ble', handlers: TransportHandlers): L;
  detach(link: L, reason: string): void;
  receive(link: L, bytes: Uint8Array): void;
}

export class SimTransport<L> implements Transport {
  readonly kind: 'usb' | 'ble';
  readonly label: string;
  #host: SimLinkHost<L>;
  #link: L | null = null;

  constructor(host: SimLinkHost<L>, kind: 'usb' | 'ble') {
    this.#host = host;
    this.kind = kind;
    this.label = kind === 'usb' ? 'Simulator (USB)' : 'Simulator (BLE)';
  }

  get isOpen(): boolean { return this.#link !== null; }

  async open(handlers: TransportHandlers): Promise<void> {
    if (this.#link) throw new Error('simulator link already open');
    this.#link = this.#host.attach(this.kind, {
      onBytes: (c) => handlers.onBytes(c),
      onClose: (r) => { this.#link = null; handlers.onClose(r); },
    });
  }

  async write(bytes: Uint8Array): Promise<void> {
    if (!this.#link) throw new Error('simulator link closed');
    this.#host.receive(this.#link, bytes);
  }

  async close(): Promise<void> {
    const link = this.#link;
    this.#link = null;
    if (link) this.#host.detach(link, 'closed by host');
  }
}
