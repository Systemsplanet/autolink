# AutoLink ESP32

A production-grade, self-healing UART protocol layer for ESP32.

Ever had a UART connection drop because a motor spun up nearby? Have you ever hardcoded a baud rate, only to realize the other microcontroller reset and desynced? Raw UART is notoriously fragile in real-world applications.

AutoLink fixes this. It sits on top of the standard ESP-IDF UART drivers and transforms your serial connection into a robust, auto-negotiating, self-healing lifeline.

If the line gets noisy, AutoLink drops the link and re-sweeps. If a wire gets bumped, it automatically sweeps the baud spectrum and locks back onto the connection. It manages all the FreeRTOS background tasks, queues, and hardware interrupts automatically. You just send and receive data.

## Quick Start: Ping / Pong

The classic use case: two boards bouncing **random-sized messages** back and forth, **logging throughput**, and **self-recovering** from any disruption — with almost no application code. The link's sweep/recovery is automatic; the app just gates on `State::OK`. The `Ping` example pipelines sends and verifies every echo (length + CRC-16) against what it sent, logging a `MISMATCH` the moment a byte goes wrong — the easiest possible end-to-end smoke test.

The two sketches live in `examples/PingPong/` as `Ping.ino` (ping) and `Pong.ino` (pong). Cross-wire the two boards (`Ping TX(GPIO17) → Pong RX(GPIO16)` and `Ping RX(GPIO16) ← Pong TX(GPIO17)`, shared GND).

### Ping

```cpp
#include <pingpong/PingPong.h>
using namespace autolink;

PingPong upp(
    PingPong::PING,
    115200,        // Serial debug baud
    UART_NUM_2,    // UART port for the AutoLink wire
    16,            // RX pin
    17,            // TX pin
    "YourSSID",    // WiFi SSID  (nullptr to disable web monitor)
    "password",    // WiFi password
    80             // web server port
);

void setup() { upp.setup(); }
void loop()  { upp.loop();  }
```

### Pong

Identical except `PingPong::PONG` instead of `PingPong::PING`. To swap a board from one role to the other, change the enum and re-flash — no other source change needed.

That's the whole thing. No manual reconnect logic, no checksums, no framing, and no state machine to babysit in your sketch. If the cable is yanked mid-stream, both ends fall back to a baud re-sweep and the loops resume on their own — `send()` simply returns `0` until the link is back. A `recv()` of `-1` means a corrupt/desynced message was rejected and the buffer flushed; you can ignore it and keep looping.

Both sketches log throughput, baud, disconnects, and lifetime error counts to serial every 5 seconds. Pass WiFi credentials (as above) and the same data appears on a live web dashboard — see **Web Monitor** below.

## Using AutoLink Directly (not the PingPong wrapper)

If you want your own send/recv loop, the underlying layer is the `AutoLink` class (a thin facade over `ALink`). See `docAPI.md` for the full reference. The bare minimum:

```cpp
#include <AutoLink.h>
using namespace autolink;

AutoLink comm(UART_NUM_2, 16, 17, /*isMaster=*/true);

void setup() { comm.begin(); }
void loop()  {
    if (comm.ready()) {
        // comm.send(buf, n); comm.recv(buf, sizeof buf);
    }
}
```

## Features Under the Hood

- **Working Auto-Baud:** During the sweep Ping steps through each allowed baud sending `PING`; Pong **retunes in lockstep**, scoring every baud it can actually decode, then both lock onto the *fastest* one that worked.
- **Boundary-Preserving Messages:** `sendMsg` / `recvMsg` frame arbitrary-length payloads with a length header and an end-to-end CRC-16, independent of the per-frame CRC-8.
- **cobsSeq Gap/Stale Detection:** Every reliable-mode data frame carries a 1-byte sequence counter. The receiver drops stale or out-of-order frames at the wire layer so a wire-byte shift can never reach the message layer — the v3.x bug that caused permanent link desync is gone.
- **ARQ — Per-Message ACK (v5):** Every accepted `send()` is retransmitted on demand until the peer acknowledges it. If `send()` returned `len`, the message is guaranteed to reach `recv()` on the peer (or the link drops with `disconnect` count incremented). The v4 best-effort mode is gone — every sent byte is now either delivered or triggers a link reset.
- **Built-in Throughput Metering:** TX/RX byte counters with `getStats()` / `resetStats()` — log B/s without instrumenting your app.
- **Optional WiFi Web Monitor:** the `AutoLinkWeb` class serves a self-contained, mobile-friendly dashboard over WiFi — live TX/RX throughput, link state, lifetime error and disconnect counts, RSSI, heap, current baud, fill mode, log level (None / Error / Warn / Info / Debug / Verbose), and a scrolling log panel, with Reset and Reboot controls. It runs in its own task on the ESP's built-in HTTP server (no extra libraries, no `handleClient()` in your loop), and if WiFi fails the UART link is completely unaffected. See `docWebMonitor.md`.
- **Non-Blocking Core:** A dedicated FreeRTOS task, hardware interrupts, and `StreamBuffers` keep `loop()` responsive. The reliable writer inserts no artificial per-chunk delays, so throughput tracks the actual baud rate.
- **Smart Framing:** In `SWP` / `LCK`, commands are wrapped in CRC-8-validated frames behind a `0xAA 0x55` preamble; electrical noise cannot trigger a false state change. Only Ping initiates transitions.
- **Namespace Isolation:** Everything lives in `namespace autolink`, avoiding collisions with Arduino or ESP-IDF.
- **Test-Driven Core:** `ALink` is fully decoupled from hardware via the `ILink` interface. The desktop test suite (`test/test_desktop/`) compiles and verifies the protocol, negotiation, message round-tripping, and CRC handling natively on your build machine — no ESP32 required.

## Document Index

| File | Contents |
|------|----------|
| `README.md` | Overview, quick start (Ping/Pong), feature summary |
| `docWebMonitor.md` | Web Monitor setup, dashboard, controls, endpoints, errors vs. disconnects |
| `docAPI.md` | Message API reference, advanced usage (raw streaming, manual error control), developer notes |
| `docTests.md` | How to build, run, and extend the host test suite; ASan + coverage modes; CI recipes |
| `docVersion.md` | Full version history for all releases |
| `AGENTS.md` | Working-with-this-project rules for AI agents (or new contributors): how to bump versions, build, verify, and ship a clean zip |

Source layout: library code is in `src/` (core + ping-pong) and `src/util/` (utilities); runnable sketches are in `examples/PingPong/` and `examples/Diagnostic/`; tests are in `test/test_desktop/` (host) and `test/test_embedded/` (on-hardware); build scripts (`build_env.sh`, `verify_build.sh`, `flatten_for_arduino_cli.sh`) live in `build/`.

## License

MIT License.

Build something awesome.
