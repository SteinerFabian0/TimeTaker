/*
 * BLE transport: Web Bluetooth (Chrome/Edge; Android too) to the nRF52805
 * bridge's Nordic UART Service (BLE_FIRMWARE_GUIDE §6, SYSTEM_ARCHITECTURE
 * §9.6). The bridge is dumb: NUS bytes in and out are the host protocol's
 * byte stream, exactly as over USB.
 *
 *   - Chooser filter: NUS service UUID + namePrefix "TimeTaker" (the name is
 *     in the scan response, the UUID in the advertising data; Chrome merges
 *     them, so one filter with both works).
 *   - Host → device: the RX characteristic, written in chunks of 20 bytes
 *     (BLE guide §13 Q6: Chrome does not expose the negotiated MTU; 20 B
 *     always fits ATT MTU 23). Writes are serialised (GATT allows one
 *     operation at a time). Write without response when the browser has it,
 *     else with response.
 *   - Device → host: notifications on TX, delivered as byte chunks; frames
 *     may span notifications (the session's decoder reassembles).
 *   - 'gattserverdisconnected' → onClose (not after close()).
 *   - remembered(): navigator.bluetooth.getDevices() (where available) gives
 *     a device this origin was granted before: reconnect without the chooser.
 *
 * Web Bluetooth is not in TypeScript's DOM library, so the members used are
 * declared here. The API object is injectable (tests use a fake).
 */

import type { Transport, TransportHandlers } from './transport.ts';

export const NUS_SERVICE = '6e400001-b5a3-f393-e0a9-e50e24dcca9e';
/** Central → peripheral (write). */
export const NUS_RX = '6e400002-b5a3-f393-e0a9-e50e24dcca9e';
/** Peripheral → central (notify). */
export const NUS_TX = '6e400003-b5a3-f393-e0a9-e50e24dcca9e';
export const BLE_NAME_PREFIX = 'TimeTaker';
export const BLE_WRITE_CHUNK = 20;

export interface CharacteristicLike extends EventTarget {
  readonly value?: DataView | null;
  startNotifications(): Promise<unknown>;
  stopNotifications?(): Promise<unknown>;
  writeValueWithoutResponse?(data: BufferSource): Promise<void>;
  writeValueWithResponse?(data: BufferSource): Promise<void>;
  writeValue?(data: BufferSource): Promise<void>;
}
export interface ServiceLike { getCharacteristic(uuid: string): Promise<CharacteristicLike> }
export interface GattServerLike {
  readonly connected: boolean;
  connect(): Promise<GattServerLike>;
  disconnect(): void;
  getPrimaryService(uuid: string): Promise<ServiceLike>;
}
export interface BluetoothDeviceLike extends EventTarget {
  readonly id: string;
  readonly name?: string | null;
  readonly gatt?: GattServerLike | null;
}
export interface RequestDeviceOptions {
  filters: { services?: string[]; namePrefix?: string; name?: string }[];
  optionalServices?: string[];
}
export interface BluetoothLike {
  requestDevice(options: RequestDeviceOptions): Promise<BluetoothDeviceLike>;
  getDevices?(): Promise<BluetoothDeviceLike[]>;
  getAvailability?(): Promise<boolean>;
}

export function bluetoothApi(): BluetoothLike | undefined {
  return typeof navigator === 'undefined' ? undefined : (navigator as unknown as { bluetooth?: BluetoothLike }).bluetooth;
}

export function isWebBluetoothAvailable(): boolean {
  return bluetoothApi() !== undefined;
}

export interface BleOptions {
  /** Bytes per GATT write (default 20). */
  readonly chunkBytes?: number;
  /** 'auto' prefers write without response. */
  readonly writeMode?: 'auto' | 'withResponse' | 'withoutResponse';
}

export const BLE_FILTER: RequestDeviceOptions = {
  filters: [{ services: [NUS_SERVICE], namePrefix: BLE_NAME_PREFIX }],
  optionalServices: [NUS_SERVICE],
};

export class BleTransport implements Transport {
  readonly kind = 'ble' as const;
  readonly label: string;
  readonly device: BluetoothDeviceLike;
  #opts: Required<BleOptions>;
  #handlers: TransportHandlers | null = null;
  #rx: CharacteristicLike | null = null;
  #tx: CharacteristicLike | null = null;
  #queue: Promise<void> = Promise.resolve();
  #closing = false;
  #onNotify = (ev: Event): void => {
    const v = (ev.target as CharacteristicLike).value;
    if (v && v.byteLength > 0) this.#handlers?.onBytes(new Uint8Array(v.buffer.slice(v.byteOffset, v.byteOffset + v.byteLength)));
  };
  #onDisconnect = (): void => { this.#lost('BLE link lost'); };

  constructor(device: BluetoothDeviceLike, opts: BleOptions = {}) {
    this.device = device;
    this.label = `${device.name ?? BLE_NAME_PREFIX} (BLE)`;
    this.#opts = { chunkBytes: BLE_WRITE_CHUNK, writeMode: 'auto', ...opts };
  }

  /** Shows the browser's BLE chooser (needs a user gesture). */
  static async request(api: BluetoothLike | undefined = bluetoothApi(), opts?: BleOptions): Promise<BleTransport> {
    if (!api) throw new Error('Web Bluetooth is not available: use Chrome or Edge (Linux may need chrome://flags/#enable-experimental-web-platform-features)');
    return new BleTransport(await api.requestDevice(BLE_FILTER), opts);
  }

  /** A TimeTaker this origin was granted before, without the chooser (null if none or unsupported). */
  static async remembered(api: BluetoothLike | undefined = bluetoothApi(), opts?: BleOptions): Promise<BleTransport | null> {
    if (!api?.getDevices) return null;
    const devices = await api.getDevices();
    const d = devices.find((x) => (x.name ?? '').startsWith(BLE_NAME_PREFIX));
    return d ? new BleTransport(d, opts) : null;
  }

  async open(handlers: TransportHandlers): Promise<void> {
    if (!this.device.gatt) throw new Error('this Bluetooth device has no GATT server');
    this.#handlers = handlers;
    this.#closing = false;
    this.device.addEventListener('gattserverdisconnected', this.#onDisconnect);
    try {
      const server = await this.device.gatt.connect();
      const service = await server.getPrimaryService(NUS_SERVICE);
      this.#rx = await service.getCharacteristic(NUS_RX);
      this.#tx = await service.getCharacteristic(NUS_TX);
      this.#tx.addEventListener('characteristicvaluechanged', this.#onNotify);
      await this.#tx.startNotifications();
    } catch (err) {
      this.#closing = true;
      this.#detach();
      try { this.device.gatt.disconnect(); } catch { /* not connected */ }
      throw err;
    }
  }

  write(bytes: Uint8Array): Promise<void> {
    const rx = this.#rx;
    if (!rx) return Promise.reject(new Error('BLE link not open'));
    const data = bytes.slice();
    const job = this.#queue.then(async () => {
      for (let i = 0; i < data.length; i += this.#opts.chunkBytes) {
        if (this.#closing) throw new Error('BLE link closed');
        await this.#writeChunk(rx, data.slice(i, i + this.#opts.chunkBytes));
      }
    });
    this.#queue = job.catch(() => undefined);
    return job;
  }

  async close(): Promise<void> {
    this.#closing = true;
    const tx = this.#tx;
    this.#detach();
    try { await tx?.stopNotifications?.(); } catch { /* already gone */ }
    try { if (this.device.gatt?.connected) this.device.gatt.disconnect(); } catch { /* already gone */ }
  }

  async #writeChunk(rx: CharacteristicLike, chunk: Uint8Array): Promise<void> {
    const mode = this.#opts.writeMode;
    if (mode !== 'withResponse' && rx.writeValueWithoutResponse) return rx.writeValueWithoutResponse(chunk);
    if (mode !== 'withoutResponse' && rx.writeValueWithResponse) return rx.writeValueWithResponse(chunk);
    if (rx.writeValue) return rx.writeValue(chunk);
    throw new Error(`the NUS RX characteristic cannot be written (${mode})`);
  }

  #detach(): void {
    this.device.removeEventListener('gattserverdisconnected', this.#onDisconnect);
    this.#tx?.removeEventListener('characteristicvaluechanged', this.#onNotify);
    this.#rx = null;
    this.#tx = null;
  }

  #lost(reason: string): void {
    if (this.#closing) return;
    this.#closing = true;
    this.#detach();
    this.#handlers?.onClose(reason);
  }
}
