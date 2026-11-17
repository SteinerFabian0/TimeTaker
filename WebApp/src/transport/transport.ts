/*
 * A byte pipe to the control module. USB (Web Serial), BLE (Web Bluetooth,
 * later) and the simulator all implement this, and the device session never
 * knows which one it got (SYSTEM_ARCHITECTURE §11).
 *
 * Bytes, not frames: chunks arrive however the transport delivers them, and
 * the session's FrameDecoder reassembles frames.
 */

export type TransportKind = 'usb' | 'ble' | 'sim';

export interface TransportHandlers {
  /** A chunk of received bytes. */
  onBytes(chunk: Uint8Array): void;
  /** The link is gone (unplug, port closed, error). Not called after close(). */
  onClose(reason: string): void;
}

export interface Transport {
  readonly kind: TransportKind;
  /** Human-readable name for the UI, for example "TimeTaker (USB)". */
  readonly label: string;
  open(handlers: TransportHandlers): Promise<void>;
  write(bytes: Uint8Array): Promise<void>;
  close(): Promise<void>;
}
