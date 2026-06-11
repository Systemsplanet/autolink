# 🚀 AutoLink ESP32

**A production-grade, self-healing UART protocol layer for ESP32.**


Ever had a UART connection drop because a motor spun up nearby? Have you ever hardcoded a baud rate, only to realize the other microcontroller reset and desynced? Raw UART is notoriously fragile in real-world applications.


**AutoLink** fixes this. It sits on top of the standard ESP-IDF UART drivers and transforms your serial connection into a robust, auto-negotiating, self-healing lifeline.


If the line gets noisy, AutoLink drops the link and re-sweeps. If a wire gets bumped, it automatically sweeps the baud spectrum and locks back onto the connection. It manages all the FreeRTOS background tasks, queues, and hardware interrupts automatically. You just send and receive data.


# 🏓 Quick Start: Ping / Pong

The classic use case: two boards bouncing **random-sized messages** back and forth, **logging throughput**, and **self-recovering** from any disruption — with almost no application code. The link's sweep/recovery is automatic; the app just gates on `State::OK`. The `Ping` example pipelines sends and verifies every echo (length + CRC-16) against what it sent, logging a `MISMATCH` the moment a byte goes wrong — the easiest possible end-to-end smoke test.

The two sketches live in `examples/PingPong/` as `Ping.ino` (ping) and `Pong.ino` (pong).

## Ping

```cpp
#include "util/UtilPing.h"
using namespace autolink;

// Wiring (crossover): Ping TX(GPIO17) ──► Pong RX(GPIO16)
//                     Ping RX(GPIO16) ◄── Pong TX(GPIO17)
//                     shared GND
UtilPing ping(
    115200,       // Serial debug baud
    UART_NUM_2,   // UART port for the AutoLink wire
    16,           // RX pin
    17,           // TX pin
    "YourSSID",   // WiFi SSID   — omit (or pass nullptr) to disable web monitor
    "password",   // WiFi password
    80            // web server port
);

void setup() { ping.setup(); }
void loop()  { ping.loop();  }
```

## Pong

```cpp
#include "util/UtilPong.h"
using namespace autolink;

// Wiring (crossover): Ping TX(GPIO17) ──► Pong RX(GPIO16)
//                     Ping RX(GPIO16) ◄── Pong TX(GPIO17)
//                     shared GND
UtilPong pong(
    115200,       // Serial debug baud
    UART_NUM_2,   // UART port for the AutoLink wire
    16,           // RX pin
    17,           // TX pin
    "YourSSID",   // WiFi SSID   — omit (or pass nullptr) to disable web monitor
    "password",   // WiFi password
    80            // web server port
);

void setup() { pong.setup(); }
void loop()  { pong.loop();  }
```

That's the whole thing. No manual reconnect logic, no checksums, no framing, and no state machine to babysit in your sketch. If the cable is yanked mid-stream, both ends fall back to a baud re-sweep and the loops resume on their own — `send()` simply returns `0` until the link is back. A `recv()` of `-1` means a corrupt/desynced message was rejected and the buffer flushed; you can ignore it and keep looping.

Both sketches log throughput, baud, disconnects, and lifetime error counts to serial every 5 seconds. Pass WiFi credentials (as above) and the same data appears on a live web dashboard — see **Web Monitor** below.


# 📦 Features Under the Hood


+ **Working Auto-Baud:** During the sweep Ping steps through each allowed baud sending `PING`; Pong **retunes in lockstep**, scoring every baud it can actually decode, then both lock onto the *fastest* one that worked.

+ **Boundary-Preserving Messages:** `sendMsg`/`recvMsg` frame arbitrary-length payloads with a length header and an end-to-end CRC-16, independent of the per-frame CRC-8.

+ **Built-in Throughput Metering:** TX/RX byte counters with `getStats()` / `resetStats()` — log B/s without instrumenting your app.

+ **Optional WiFi Web Monitor:** the `AutoLinkWeb` class serves a self-contained, mobile-friendly dashboard over WiFi — live TX/RX throughput, link state, lifetime error and disconnect counts, RSSI, heap, current baud, and a scrolling log panel, with Reset and Reboot controls. It runs in its own task on the ESP's built-in HTTP server (no extra libraries, no `handleClient()` in your loop), and if WiFi fails the UART link is completely unaffected. Just pass WiFi credentials to `UtilPing`/`UtilPong` or construct an `AutoLinkWeb` directly. See **`docWebMonitor.md`**.

+ **Non-Blocking Core:** A dedicated FreeRTOS task, hardware interrupts, and `StreamBuffers` keep `loop()` responsive. The reliable writer inserts no artificial per-chunk delays, so throughput tracks the actual baud rate.

+ **Smart Framing:** In `SWP`/`LCK`, commands are wrapped in CRC-8-validated frames behind a `0xAA 0x55` preamble; electrical noise cannot trigger a false state change. Only Ping initiates transitions.

+ **Namespace Isolation:** Everything lives in `namespace autolink`, avoiding collisions with Arduino or ESP-IDF.

+ **Test-Driven Core:** `ALink` is fully decoupled from hardware via the `ILink` interface. The desktop test suite (`test/test_desktop/`) compiles and verifies the protocol, negotiation, message round-tripping, and CRC handling natively on your build machine — no ESP32 required.


# 📚 Document Index

| File | Contents |
|------|----------|
| [README.md](README.md) | Overview, quick start (Ping/Pong), feature summary |
| [AutoLink/docWebMonitor.md](AutoLink/docWebMonitor.md) | Web Monitor setup, dashboard, controls, endpoints, errors vs. disconnects |
| [AutoLink/docAPI.md](AutoLink/docAPI.md) | Message API reference, advanced usage (raw streaming, manual error control), developer notes |
| [AutoLink/docVersion.md](AutoLink/docVersion.md) | Full version history for all releases |

Source layout: library code is in `AutoLink/src/` (core) and `AutoLink/src/util/` (utilities); runnable sketches are in `AutoLink/examples/PingPong/`; tests are in `AutoLink/test/test_desktop/` (host) and `AutoLink/test/test_embedded/` (on-hardware).

# 📜 License

MIT License.

Build something awesome.
