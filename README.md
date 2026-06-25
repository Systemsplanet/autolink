# AutoLink ESP32

A production-grade, self-healing UART protocol layer for ESP32.

Ever had a UART connection drop because a motor spun up nearby? Have you ever hardcoded a baud rate, only to realize the other microcontroller reset and desynced? Raw UART is notoriously fragile in real-world applications.

AutoLink fixes this. It sits on top of the standard ESP-IDF UART drivers and transforms your serial connection into a robust, auto-negotiating, self-healing lifeline.

If the line gets noisy, AutoLink drops the link and re-sweeps. If a wire gets bumped, it automatically sweeps the baud spectrum and locks back onto the connection. It manages all the FreeRTOS background tasks, queues, and hardware interrupts automatically. You just send and receive data.

See `docs/Version.md` for the version history and per-release notes.

## Quick Start: Ping / Pong

The classic use case: two boards bouncing **random-sized messages** back and forth, **logging throughput**, and **self-recovering** from any disruption — with almost no application code. The link's sweep/recovery is automatic; the app just gates on `State::OK`. The `Ping` example pipelines sends and verifies every echo (length + CRC-16) against what it sent, logging a `MISMATCH` the moment a byte goes wrong — the easiest possible end-to-end smoke test.

The two sketches live in `examples/PingPong/` as `Ping.ino` (ping) and `Pong.ino` (pong). Cross-wire the two boards (`Ping TX(GPIO17) → Pong RX(GPIO16)` and `Ping RX(GPIO16) ← Pong TX(GPIO17)`, shared GND).

### Ping

```cpp
#include "PingPong.h"
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

If you want your own send/recv loop, the underlying layer is the `AutoLink` class (a thin facade over `Link`). See `docs/API.md` for the full reference. The bare minimum:

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

## Installation

The library ships in three installable forms. Pick the one that matches your toolchain.

### Arduino Library Manager

Drop this zip into your Arduino `libraries/` folder (or use "Add .ZIP Library" in the IDE). The library exposes itself as `AutoLink`. The `library.properties` carries the version.

User sketches `#include "AutoLink.h"` and `#include "PingPong.h"` as usual — the Arduino toolchain picks them up from the `src/` directory of the library.

### ESP Component Registry (idf.py)

Add the dependency to your project's `idf_component.yml`:

```yaml
dependencies:
  autolink:
    version: "*"   # see docs/Version.md for the current release
```

When you depend on the published component, the `CMakeLists.txt` + `idf_component.yml` in this repo are bypassed and the registry version is used.

**Heads-up:** the C++ facade (`AutoLink.h`, `PingPong.h`) is wired against the Arduino-ESP32 core (it includes `<Arduino.h>` through `EspHal.h`). To consume AutoLink from a stock ESP-IDF project, include the Arduino-ESP32 component as a wrapper (Arduino-as-ESP-IDF-component) and depend on `autolink` as a managed component. The CMakeLists.txt auto-detects this with the `ARDUINO` / `ARDUINO_AS_ESP_IDF_COMPONENT` / `CONFIG_ARDUINO` defines and compiles the full source list including `AutoLinkWeb.cpp`. Without an Arduino core present, the WiFi dashboard glue is excluded; the protocol + message API still work.

If you only need raw UART (no AutoLink protocol), see `examples/espidf_basic/` — a standalone stock-ESP-IDF project that uses `driver/uart.h` directly and doesn't pull in the C++ facade.

### Manual drop-in (no package manager)

Unzip at the project root and point your build at it:

- **arduino-cli:** `arduino-cli compile --library . ...`
- **ESP-IDF:** copy or symlink the root into your project's `components/` directory (using the library name as the subdirectory name), or set `EXTRA_COMPONENT_DIRS` to the unzipped root.

## Features Under the Hood

- **Working Auto-Baud:** During the sweep Ping steps through each allowed baud sending `PING`; Pong **retunes in lockstep**, scoring every baud it can actually decode, then both lock onto the *fastest* one that worked.
- **Baud Preference:** Once the link locks, the baud that worked is remembered. Any subsequent drop (idle watchdog, error burst, peer BREAK) re-sweeps starting at that baud — not from the top of `allowedBauds`. The link only falls back to a full sweep after `cfg.baudRetryLimit` consecutive failed retries.
- **Error-Rate Window:** The absolute `errThreshold` catches transient bursts (default 100); the rate window (`cfg.errRateWindow`, default 30 errors in any 1 s) catches sustained-but-not-bursty noise. A connection doing 30 errors/second is healthy statistics-wise but useless — the link re-sweeps to a more robust baud.
- **Boundary-Preserving Messages:** `sendMsg` / `recvMsg` frame arbitrary-length payloads with a length header and an end-to-end CRC-16, independent of the per-frame CRC-8.
- **Header+Data Coalescing:** Short messages merge the 6-byte MSG_HDR with the first payload chunk into a single wire frame. Per-message wire efficiency goes from ~20% to ~38% for 100-byte messages; ~50% to ~80% for 500-byte messages.
- **cobsSeq Gap/Stale Detection:** Every data frame carries a 1-byte sequence counter (the `cobsSeq` space is 0..253; 0xFE and 0xFF are reserved wire discriminators). On a gap, the out-of-order frame is held in a bounded reorder buffer and a NAK fires for the first missing seq; the retransmit lands in-order when it arrives. A staleness cap (`cfg.reorderHoldMs`, default 1500 ms) drops the hold and counts `lostMsgs++` if the retransmit never succeeds.
- **ARQ — Per-Message ACK:** Every accepted `send()` is retransmitted on demand (NAK-driven fast retransmit + ACK-timeout retransmit) until the peer acknowledges it. If the ARQ exceeds its retry budget or the link drops, the `disconnect` counter is incremented. The payload cache is pool-backed (no `malloc` on the TX hot path) — under sustained back-pressure, `send()` returns 0 and the message is dropped (with a loud log line), never silently sent-without-cache. Delivery is best-effort under sustained noise — if your sketch needs hard delivery guarantees, layer them on top.
- **Built-in Throughput Metering:** TX/RX byte counters with `getStats()` / `resetStats()`.
- **Optional WiFi Web Monitor:** the `AutoLinkWeb` class serves a self-contained, mobile-friendly dashboard over WiFi — live TX/RX throughput, link state, lifetime error and disconnect counts, RSSI, heap, current baud, fill mode, log level, a scrolling log panel with a **Save button** that downloads the log as `ping.txt` or `pong.txt`, and Reset and Reboot controls. Runs in its own task on the ESP's built-in HTTP server. If WiFi fails the UART link is completely unaffected. See `docs/WebMonitor.md`.
- **Non-Blocking Core:** A dedicated FreeRTOS task, hardware interrupts, and `StreamBuffers` keep `loop()` responsive.
- **Smart Framing:** In `SWP` / `LCK`, commands are wrapped in CRC-8-validated frames behind a `0xAA 0x55` preamble; electrical noise cannot trigger a false state change.
- **Namespace Isolation:** Everything lives in `namespace autolink`.
- **Test-Driven Core:** `Link` is fully decoupled from hardware via the `IHal` interface. The host unit suite (`test/test_desktop/`) verifies the protocol natively on your build machine — no ESP32 required.

## Document Index

| File | Contents |
|------|----------|
| `README.md` | Overview, quick start, feature summary |
| `docs/WebMonitor.md` | Web Monitor setup, dashboard, controls, endpoints |
| `docs/API.md` | Message API reference, advanced usage |
| `docs/Protocol.md` | Wire-level protocol spec (phases, ARQ, framing) |
| `docs/Tests.md` | How to build and run the host test suite; ASan + coverage modes |
| `docs/Version.md` | Version history (last 8 releases; older in git) |
| `AGENTS.md` | Working-with-this-project rules for AI agents / contributors |

Source layout: public headers in `include/`; implementation in `src/` and the internal subdirs `src/al/{link,util,hal,web,pingpong}/`. Flat shims `AutoLink.h` and `PingPong.h` at `src/` keep Arduino-1.x happy (which only adds `src/` to the include path). Runnable sketches live in `examples/arduino_basic/`, `examples/PingPong/`, `examples/espidf_basic/`; tests live in `test/test_desktop/` (unit), `test/itest/test_desktop/` (host integration), `test/itest/test_embedded/` (on-hardware); build scripts (`build_env.sh`, `verify_build.sh`, `arduino-cli-cmd.sh`) live in `build/`.

## License

MIT.
