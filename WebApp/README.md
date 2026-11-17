# TimeTaker web app

The race control app for Chrome/Edge. It reaches the control module over
**Web Serial** (USB CDC) or **Web Bluetooth** (the nRF bridge, not yet
built). Layering and plan: `../SYSTEM_ARCHITECTURE.md` §11.

## Run

```
npm test          # all tests (Node >= 22.18 runs the .ts files directly)
npm run debug     # dev server → http://localhost:5173/debug/
```

There is no install and no build step. Node strips the TypeScript types
itself: in the tests, and in `tools/serve.mjs` for the browser. That is why
the TypeScript stays within `erasableSyntaxOnly`:

- no `enum` (use `as const` objects), no `namespace`;
- no parameter properties, no `private` constructors (use `#private`
  fields);
- imports carry the `.ts` extension.

## Debug console (`debug/`)

The bring-up window onto the control module:

- **Connect USB** opens Chrome's port picker, which lists only TimeTaker
  devices (VID/PID from `[usb]` in `../protocol/host_protocol.toml`).
- **Simulator** connects to `SimDevice` instead: no hardware needed.
- **Shows** the session (bootId, protocol, live device clock, round trip,
  event position), the node list, every event decoded, and a trace of the
  raw frames.
- **Controls:** LANE_POWER on/off (SYNC starts 30 ms after the rail, which
  is the moment for the scope), NODE_LIST, REBOOT.
- **Simulator panel:** buttons, simulated cars, "drop 3 events" (to watch
  the RESUME recovery), power cycle (to see a new bootId).

**On Linux:** the user needs access to `/dev/ttyACM*`. Being in the `dialout`
group is enough (`id -nG`). Otherwise add a udev rule:
`SUBSYSTEM=="tty", ATTRS{idVendor}=="1209", ATTRS{idProduct}=="0001", TAG+="uaccess"`.

## Layout

| Path | What |
|---|---|
| `src/protocol/generated.ts` | **Generated** from `../protocol/host_protocol.toml`: IDs, enums, status codes, USB IDs, struct descriptors, types, wrappers. Never edit it. |
| `src/protocol/wire.ts` | Little-endian struct encoding driven by the descriptors |
| `src/protocol/codec.ts` | COBS + CRC-16 framing, the twin of the firmware's `frame_codec.c` |
| `src/protocol/packet.ts` | Frame bytes ↔ header + payload |
| `src/transport/` | `Transport` interface; `serialTransport.ts` (Web Serial) |
| `src/device/deviceSession.ts` | Session: HELLO, requests with timeouts, events in evSeq order exactly once (gap → RESUME), PING heartbeat + clock estimate, resume point across reconnects and transport switches, reboot detection |
| `src/device/simDevice.ts` | A simulated control module speaking the real protocol, plus lanes and cars ahead of the firmware |
| `debug/` | The debug console (plain DOM) |
| `tools/serve.mjs` | Dev server: static files + on-the-fly type stripping, bound to 127.0.0.1 |
| `test/protocol.test.ts` | Codec and wire against the golden bytes the firmware's C tests are held to |
| `test/session.test.ts` | DeviceSession against SimDevice: connect, requests, ordering, loss recovery, gaps, transport switch, reboot |
