/*
 * A fake navigator.bluetooth whose one device is a SimDevice's BLE link:
 * NUS RX writes go to the simulator, its bytes come back as TX notifications
 * (split at `mtuPayload`, like the nRF does), and a lost link fires
 * 'gattserverdisconnected'. Records every write size.
 */

import { NUS_RX, NUS_SERVICE, NUS_TX } from '../src/transport/bleTransport.ts';
import type {
  BluetoothDeviceLike, BluetoothLike, CharacteristicLike, GattServerLike, RequestDeviceOptions, ServiceLike,
} from '../src/transport/bleTransport.ts';
import type { SimDevice } from '../src/device/simDevice.ts';
import type { Transport } from '../src/transport/transport.ts';

class FakeChar extends EventTarget implements CharacteristicLike {
  value: DataView | null = null;
  notifying = false;
  readonly onWrite?: (b: Uint8Array) => Promise<void>;
  constructor(onWrite?: (b: Uint8Array) => Promise<void>) { super(); this.onWrite = onWrite; }
  async startNotifications(): Promise<this> { this.notifying = true; return this; }
  async stopNotifications(): Promise<this> { this.notifying = false; return this; }
  async writeValueWithoutResponse(data: BufferSource): Promise<void> {
    const b = data instanceof Uint8Array ? data : new Uint8Array(data as ArrayBuffer);
    await this.onWrite?.(b);
  }
  notify(bytes: Uint8Array): void {
    if (!this.notifying) return;
    this.value = new DataView(bytes.slice().buffer);
    this.dispatchEvent(new Event('characteristicvaluechanged'));
  }
}

export class FakeBleDevice extends EventTarget implements BluetoothDeviceLike {
  readonly id = 'fake-1';
  readonly name: string;
  readonly writes: number[] = [];
  gatt: GattServerLike;
  #sim: SimDevice;
  #link: Transport | null = null;
  #rx: FakeChar;
  #tx = new FakeChar();
  #connected = false;

  readonly mtuPayload: number;
  constructor(sim: SimDevice, name = 'TimeTaker', mtuPayload = 20) {
    super();
    this.mtuPayload = mtuPayload;
    this.#sim = sim;
    this.name = name;
    this.#rx = new FakeChar(async (b) => {
      this.writes.push(b.length);
      if (!this.#link) throw new Error('GATT disconnected');
      await this.#link.write(b);
    });
    const self = this;
    const service: ServiceLike = {
      async getCharacteristic(uuid: string): Promise<CharacteristicLike> {
        if (uuid === NUS_RX) return self.#rx;
        if (uuid === NUS_TX) return self.#tx;
        throw new Error(`no characteristic ${uuid}`);
      },
    };
    this.gatt = {
      get connected() { return self.#connected; },
      async connect() {
        const link = self.#sim.transport('ble');
        await link.open({
          onBytes: (c) => { for (let i = 0; i < c.length; i += self.mtuPayload) self.#tx.notify(c.slice(i, i + self.mtuPayload)); },
          onClose: () => { self.#link = null; self.#connected = false; self.dispatchEvent(new Event('gattserverdisconnected')); },
        });
        self.#link = link;
        self.#connected = true;
        return self.gatt;
      },
      disconnect() {
        const l = self.#link;
        self.#link = null;
        if (!self.#connected) return;
        self.#connected = false;
        void l?.close();
        self.dispatchEvent(new Event('gattserverdisconnected'));
      },
      async getPrimaryService(uuid: string) {
        if (uuid !== NUS_SERVICE) throw new Error(`no service ${uuid}`);
        return service;
      },
    };
  }
}

export class FakeBluetooth implements BluetoothLike {
  lastRequest: RequestDeviceOptions | null = null;
  readonly devices: FakeBleDevice[];
  readonly granted: FakeBleDevice[];
  constructor(devices: FakeBleDevice[], granted: FakeBleDevice[] = []) { this.devices = devices; this.granted = granted; }
  async requestDevice(o: RequestDeviceOptions): Promise<FakeBleDevice> {
    this.lastRequest = o;
    const f = o.filters[0];
    const d = this.devices.find((x) => !f?.namePrefix || x.name.startsWith(f.namePrefix));
    if (!d) throw new Error('NotFoundError: User cancelled the requestDevice() chooser.');
    if (!this.granted.includes(d)) this.granted.push(d);
    return d;
  }
  async getDevices(): Promise<FakeBleDevice[]> { return this.granted.slice(); }
}
