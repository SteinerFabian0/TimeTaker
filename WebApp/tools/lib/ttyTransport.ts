/*
 * The control module's USB CDC port from Node (bench CLI): /dev/ttyACM*,
 * raw mode via `stty -F <dev> raw -echo` (and the tty handle's raw mode),
 * read through a libuv tty handle, written through a second blocking fd.
 * Linux only (sysfs for the port list).
 */

import { execFileSync } from 'node:child_process';
import { closeSync, constants, existsSync, openSync, readdirSync, readFileSync, realpathSync, write } from 'node:fs';
import { dirname, join } from 'node:path';
import { ReadStream } from 'node:tty';

import { USB_ID } from '../../src/protocol/generated.ts';
import type { Transport, TransportHandlers } from '../../src/transport/transport.ts';

export interface PortInfo {
  path: string;
  vendorId: number | null;
  productId: number | null;
  product: string | null;
  serial: string | null;
}

const readHex = (f: string): number | null => (existsSync(f) ? parseInt(readFileSync(f, 'utf8').trim(), 16) : null);
const readStr = (f: string): string | null => (existsSync(f) ? readFileSync(f, 'utf8').trim() : null);

/** Every /dev/ttyACM* with its USB identity from sysfs. */
export function listPorts(): PortInfo[] {
  const base = '/sys/class/tty';
  if (!existsSync(base)) return [];
  return readdirSync(base).filter((n) => n.startsWith('ttyACM')).sort().map((n) => {
    let usb: string | null = null;
    try { usb = dirname(realpathSync(join(base, n, 'device'))); } catch { usb = null; }
    return {
      path: `/dev/${n}`,
      vendorId: usb ? readHex(join(usb, 'idVendor')) : null,
      productId: usb ? readHex(join(usb, 'idProduct')) : null,
      product: usb ? readStr(join(usb, 'product')) : null,
      serial: usb ? readStr(join(usb, 'serial')) : null,
    };
  });
}

/** TimeTaker ports: our VID and PID. */
export function findTimeTaker(): PortInfo[] {
  return listPorts().filter((p) => p.vendorId === USB_ID.vendorId && p.productId === USB_ID.productId);
}

/** Writes all of bytes through a write(2)-like function that may take only part of them. */
export async function writeAll(
  writeSome: (buf: Uint8Array, offset: number) => Promise<number>, bytes: Uint8Array, isOpen: () => boolean = () => true,
): Promise<void> {
  let off = 0;
  while (off < bytes.length) {
    if (!isOpen()) throw new Error('port closed');
    const n = await writeSome(bytes, off);
    if (n <= 0) throw new Error('port accepted no bytes');
    off += n;
  }
}

export class TtyTransport implements Transport {
  readonly kind = 'usb' as const;
  readonly label: string;
  readonly path: string;
  #rfd = -1;
  #wfd = -1;
  #stream: ReadStream | null = null;
  #closing = false;
  #handlers: TransportHandlers | null = null;
  #writes: Promise<void> = Promise.resolve();

  constructor(path: string) {
    this.path = path;
    this.label = `TimeTaker (${path})`;
  }

  async open(handlers: TransportHandlers): Promise<void> {
    this.#handlers = handlers;
    this.#closing = false;
    try { execFileSync('stty', ['-F', this.path, 'raw', '-echo', '115200'], { stdio: 'ignore' }); } catch { /* not fatal: raw mode below */ }
    this.#rfd = openSync(this.path, constants.O_RDWR | constants.O_NOCTTY);
    this.#wfd = openSync(this.path, constants.O_WRONLY | constants.O_NOCTTY);
    const s = new ReadStream(this.#rfd);
    try { s.setRawMode(true); } catch { /* stty did it */ }
    s.on('data', (b: Buffer) => this.#handlers?.onBytes(new Uint8Array(b.buffer, b.byteOffset, b.length)));
    s.on('error', (e) => this.#lost(`read failed: ${e.message}`));
    s.on('end', () => this.#lost('port closed'));
    this.#stream = s;
  }

  write(bytes: Uint8Array): Promise<void> {
    if (this.#wfd < 0) return Promise.reject(new Error('port not open'));
    const data = bytes.slice();
    // One frame at a time, whole: partial writes must not interleave, and a vanished port ends the link.
    const job = this.#writes.then(() => writeAll((buf, off) => new Promise<number>((resolve, reject) => {
      const fd = this.#wfd;
      write(fd, buf, off, buf.length - off, null, (err, n) => (err ? reject(err) : resolve(n)));
    }), data, () => this.#wfd >= 0)).catch((err: unknown) => {
      const e = err instanceof Error ? err : new Error(String(err));
      if (!this.#closing && /EIO|ENODEV|ENXIO|EPIPE|EBADF/.test(`${(e as NodeJS.ErrnoException).code ?? ''} ${e.message}`)) {
        this.#lost(`write failed: ${e.message}`);
      }
      throw e;
    });
    this.#writes = job.catch(() => undefined);
    return job;
  }

  async close(): Promise<void> {
    this.#closing = true;
    this.#release();
  }

  #release(): void {
    this.#stream?.removeAllListeners();
    this.#stream?.on('error', () => undefined);   // destroy() on a vanished port may still report
    this.#stream?.destroy();
    this.#stream = null;
    if (this.#wfd >= 0) { try { closeSync(this.#wfd); } catch { /* gone */ } }
    this.#rfd = -1;
    this.#wfd = -1;
  }

  #lost(reason: string): void {
    if (this.#closing) return;
    this.#closing = true;
    this.#release();
    this.#handlers?.onClose(reason);
  }
}
