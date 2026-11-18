#!/usr/bin/env python3
"""Bench tool for the nRF <-> STM link (BLE_FIRMWARE_GUIDE.md §5).

Plays the STM side of the link through a 3.3 V USB-UART adapter, so the nRF
can be tested before (or without) the control firmware. Wire the adapter's
TX to the UART1_TX net (nRF P0.16 / STM PB6), RX to the UART1_RX net (nRF
P0.18 / STM PB7) and GND, and hold the STM in reset (SW605 NRST pressed, or a
jumper on NRST to GND) so its pins are high-impedance. BENCH_TESTS.md T5.

    link_tool.py monitor  --port /dev/ttyUSB0      print every frame from the nRF
    link_tool.py echo     --port /dev/ttyUSB0      fake STM: hello, credits, echo DATA
    link_tool.py send     --port /dev/ttyUSB0 ui-state 2 0
    link_tool.py send     --port /dev/ttyUSB0 cue 1 | kick 19 | advertise 0 | stm-hello 0 1
    link_tool.py send     --port /dev/ttyUSB0 data 48656c6c6f
    link_tool.py selftest                           codec check, no port needed

The frame encoder is protocol/gen_protocol.py's reference implementation (the
one frame_codec.c is held to); this file adds only the decoder and the CTRL
layouts of ble_link_protocol.h.
"""

import argparse
import importlib.util
import struct
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
_spec = importlib.util.spec_from_file_location("gen_protocol", ROOT / "protocol" / "gen_protocol.py")
gen_protocol = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(gen_protocol)

BAUD = 460800
CH_DATA, CH_CTRL = 0, 1
DATA_MAX = 244

# type: (name, struct format, field names), ble_link_protocol.h
NRF_TO_STM = {
    0x01: ("NRF_HELLO", "<BBBBBBBBI6s",
           "linkMajor linkMinor fwMajor fwMinor fwPatch credits resetCause reserved fwGitRev btAddr"),
    0x02: ("LINK_UP", "<HBBHHHH", "attMtu txPhy rxPhy intervalUnits latency timeout10ms maxTxOctets"),
    0x03: ("LINK_DOWN", "<B", "hciReason"),
    0x04: ("BUTTON", "<B", "press"),
    0x05: ("CREDIT", "<B", "count"),
    0x06: ("STATS", "<bBHHH", "rssi reserved dataDropped linkCrcErrors uartErrors"),
}
STM_TO_NRF = {
    "stm-hello": (0x41, "<BB"),
    "ui-state": (0x42, "<BB"),
    "cue": (0x43, "<B"),
    "kick": (0x44, "<B"),
    "advertise": (0x45, "<B"),
}
RESET_CAUSES = ["UNKNOWN", "POWER_ON", "PIN", "SOFTWARE", "WATCHDOG", "LOCKUP"]
PRESSES = {1: "SHORT", 2: "LONG", 3: "VERY_LONG"}


def encode(channel: int, body: bytes) -> bytes:
    return gen_protocol.frame_encode(bytes([channel]) + body)


def cobs_decode(data: bytes):
    out = bytearray()
    i = 0
    while i < len(data):
        code = data[i]
        if code == 0:
            return None
        block = data[i + 1:i + code]
        if len(block) != code - 1:
            return None
        out += block
        i += code
        if code != 0xFF and i < len(data):
            out.append(0)
    return bytes(out)


def decode(frame: bytes):
    """One frame without its delimiter -> (channel, body) or None."""
    raw = cobs_decode(frame)
    if raw is None or len(raw) < 3:
        return None
    payload, crc = raw[:-2], raw[-2] | (raw[-1] << 8)
    if gen_protocol.crc16(payload) != crc:
        return None
    return payload[0], payload[1:]


class Deframer:
    def __init__(self):
        self.buf = bytearray()
        self.bad = 0

    def feed(self, data: bytes):
        frames = []
        for b in data:
            if b == 0:
                if self.buf:
                    f = decode(bytes(self.buf))
                    if f is None:
                        self.bad += 1
                    else:
                        frames.append(f)
                self.buf.clear()
            else:
                self.buf.append(b)
        return frames


def describe(channel: int, body: bytes) -> str:
    if channel == CH_DATA:
        return f"DATA {len(body)} B: {body[:32].hex()}{'...' if len(body) > 32 else ''}"
    if channel != CH_CTRL or not body:
        return f"channel {channel}: {body.hex()}"
    t = body[0]
    if t not in NRF_TO_STM:
        return f"CTRL type 0x{t:02X} (unknown): {body[1:].hex()}"
    name, fmt, fields = NRF_TO_STM[t]
    size = struct.calcsize(fmt)
    if len(body) - 1 < size:
        return f"CTRL {name} SHORT ({len(body) - 1} < {size} B)"
    vals = dict(zip(fields.split(), struct.unpack(fmt, body[1:1 + size])))
    if name == "NRF_HELLO":
        vals["fwGitRev"] = f"{vals['fwGitRev']:07x}"
        vals["btAddr"] = ":".join(f"{b:02X}" for b in reversed(vals["btAddr"]))
        vals["resetCause"] = RESET_CAUSES[vals["resetCause"]] if vals["resetCause"] < 6 else vals["resetCause"]
    if name == "BUTTON":
        vals["press"] = PRESSES.get(vals["press"], vals["press"])
    if name == "LINK_UP":
        vals["interval_ms"] = vals["intervalUnits"] * 1.25
    return f"{name} " + " ".join(f"{k}={v}" for k, v in vals.items())


def open_port(path: str):
    import serial  # pyserial
    return serial.Serial(path, BAUD, timeout=0.05)


def cmd_monitor(args):
    port, d = open_port(args.port), Deframer()
    while True:
        for ch, body in d.feed(port.read(512)):
            print(f"{time.monotonic():10.3f}  {describe(ch, body)}", flush=True)


def cmd_send(args):
    port = open_port(args.port)
    if args.msg == "data":
        body = bytes.fromhex("".join(args.values))
        for i in range(0, len(body), DATA_MAX):
            port.write(encode(CH_DATA, body[i:i + DATA_MAX]))
    else:
        t, fmt = STM_TO_NRF[args.msg]
        port.write(encode(CH_CTRL, bytes([t]) + struct.pack(fmt, *[int(v, 0) for v in args.values])))
    port.flush()


def cmd_echo(args):
    """Fake STM: sends STM_HELLO, tracks credits, echoes every DATA frame."""
    port, d = open_port(args.port), Deframer()
    credits, echoed, waiting = 0, 0, []
    port.write(encode(CH_CTRL, bytes([0x41, 0, 1])))
    last = time.monotonic()
    while True:
        for ch, body in d.feed(port.read(512)):
            if ch == CH_CTRL and body[:1] == b"\x01":
                credits = body[6]
                print(f"{describe(ch, body)}  -> credits = {credits}", flush=True)
            elif ch == CH_CTRL and body[:1] == b"\x05":
                credits += body[1]
            elif ch == CH_DATA:
                waiting.append(body)
            else:
                print(describe(ch, body), flush=True)
        while waiting and credits > 0:
            port.write(encode(CH_DATA, waiting.pop(0)))
            credits -= 1
            echoed += 1
        if time.monotonic() - last > 5:
            last = time.monotonic()
            print(f"echoed {echoed} frames, credits {credits}, queued {len(waiting)}, "
                  f"bad frames {d.bad}", flush=True)


def cmd_selftest(_args):
    ok = True
    cases = [bytes(), bytes([0]), bytes(range(256))[:DATA_MAX], bytes(DATA_MAX), b"\xff" * DATA_MAX]
    for body in cases:
        wire = encode(CH_DATA, body)
        got = Deframer().feed(wire)
        ok &= got == [(CH_DATA, body)]
    hello = struct.pack("<BBBBBBBBI6s", 0, 1, 2, 3, 4, 4, 4, 0, 0x07212CE4, bytes([0x11, 0x22, 0x33, 0x44, 0x55, 0xC6]))
    ok &= describe(CH_CTRL, b"\x01" + hello).startswith("NRF_HELLO linkMajor=0 linkMinor=1")
    bad = bytearray(encode(CH_DATA, b"abc"))
    bad[2] ^= 1
    d = Deframer()
    ok &= d.feed(bytes(bad) + encode(CH_DATA, b"xyz")) == [(CH_DATA, b"xyz")] and d.bad == 1
    print("link_tool selftest:", "ok" if ok else "FAIL")
    return 0 if ok else 1


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    for name in ("monitor", "echo", "send"):
        s = sub.add_parser(name)
        s.add_argument("--port", required=True)
        if name == "send":
            s.add_argument("msg", choices=sorted(STM_TO_NRF) + ["data"])
            s.add_argument("values", nargs="*")
    sub.add_parser("selftest")
    args = p.parse_args()
    return {"monitor": cmd_monitor, "echo": cmd_echo, "send": cmd_send, "selftest": cmd_selftest}[args.cmd](args) or 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        pass
