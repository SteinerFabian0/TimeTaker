/*
 * USB transport: Web Serial (Chrome/Edge, desktop) to the control module's
 * CDC-ACM port.
 *
 * The port picker only lists devices with the TimeTaker VID/PID (USB_ID,
 * generated from [usb] in host_protocol.toml). The baud rate means nothing to
 * a CDC device but Web Serial requires one. On Linux the user needs access to
 * /dev/ttyACM* (see WebApp/README.md).
 *
 * Web Serial is not in TypeScript's DOM library yet, so the few members used
 * are declared here.
 */

import { USB_ID } from '../protocol/generated.ts';
import type { Transport, TransportHandlers } from './transport.ts';

interface SerialPortFilter { usbVendorId?: number; usbProductId?: number }
interface SerialPortLike {
  open(options: { baudRate: number }): Promise<void>;
  close(): Promise<void>;
  readonly readable: ReadableStream<Uint8Array> | null;
  readonly writable: WritableStream<Uint8Array> | null;
  getInfo(): { usbVendorId?: number; usbProductId?: number };
}
interface SerialLike extends EventTarget {
  requestPort(options?: { filters?: SerialPortFilter[] }): Promise<SerialPortLike>;
  getPorts(): Promise<SerialPortLike[]>;
}

function serialApi(): SerialLike | undefined {
  return (navigator as unknown as { serial?: SerialLike }).serial;
}

export function isWebSerialAvailable(): boolean {
  return typeof navigator !== 'undefined' && serialApi() !== undefined;
}

export class SerialTransport implements Transport {
  readonly kind = 'usb' as const;
  readonly label = 'TimeTaker (USB)';

  #port: SerialPortLike;
  #reader: ReadableStreamDefaultReader<Uint8Array> | null = null;
  #writer: WritableStreamDefaultWriter<Uint8Array> | null = null;
  #closing = false;
  #onDisconnect = (e: Event): void => {
    if ((e as Event & { target: unknown }).target === this.#port) this.#lost('USB unplugged');
  };
  #handlers: TransportHandlers | null = null;

  /** Use request() or remembered(). */
  constructor(port: SerialPortLike) {
    this.#port = port;
  }

  /** Shows the browser's port picker (needs a user gesture). */
  static async request(): Promise<SerialTransport> {
    const api = serialApi();
    if (!api) throw new Error('Web Serial is not available: use Chrome or Edge on a desktop OS');
    const port = await api.requestPort({ filters: [{ usbVendorId: USB_ID.vendorId, usbProductId: USB_ID.productId }] });
    return new SerialTransport(port);
  }

  /** A TimeTaker port this origin was already granted, without the picker. */
  static async remembered(): Promise<SerialTransport | null> {
    const api = serialApi();
    if (!api) return null;
    const ports = await api.getPorts();
    const port = ports.find((p) => {
      const i = p.getInfo();
      return i.usbVendorId === USB_ID.vendorId && i.usbProductId === USB_ID.productId;
    });
    return port ? new SerialTransport(port) : null;
  }

  async open(handlers: TransportHandlers): Promise<void> {
    this.#handlers = handlers;
    this.#closing = false;
    await this.#port.open({ baudRate: 115200 });
    if (!this.#port.readable || !this.#port.writable) throw new Error('serial port has no streams');
    this.#writer = this.#port.writable.getWriter();
    this.#reader = this.#port.readable.getReader();
    serialApi()?.addEventListener('disconnect', this.#onDisconnect);
    void this.#readLoop(this.#reader);
  }

  async write(bytes: Uint8Array): Promise<void> {
    if (!this.#writer) throw new Error('USB port not open');
    await this.#writer.write(bytes);
  }

  async close(): Promise<void> {
    this.#closing = true;
    serialApi()?.removeEventListener('disconnect', this.#onDisconnect);
    try { await this.#reader?.cancel(); } catch { /* already gone */ }
    try { this.#reader?.releaseLock(); } catch { /* already released */ }
    try { this.#writer?.releaseLock(); } catch { /* already released */ }
    this.#reader = null;
    this.#writer = null;
    try { await this.#port.close(); } catch { /* unplugged */ }
  }

  async #readLoop(reader: ReadableStreamDefaultReader<Uint8Array>): Promise<void> {
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        if (value && value.length > 0) this.#handlers?.onBytes(value);
      }
      this.#lost('USB port closed');
    } catch (err) {
      this.#lost(`USB read failed: ${String(err)}`);
    }
  }

  #lost(reason: string): void {
    if (this.#closing) return;
    this.#closing = true;
    serialApi()?.removeEventListener('disconnect', this.#onDisconnect);
    this.#handlers?.onClose(reason);
    void this.close();
  }
}
