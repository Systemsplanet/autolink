# 🚀 AutoLink ESP32

**A production-grade, self-healing UART protocol layer for ESP32.**


Ever had a UART connection drop because a motor spun up nearby? Have you ever hardcoded a baud rate, only to realize the other microcontroller reset and desynced? Raw UART is notoriously fragile in real-world applications.


**AutoLink** fixes this. It sits on top of the standard ESP-IDF UART drivers and transforms your serial connection into a robust, auto-negotiating, self-healing lifeline.


If the line gets noisy, AutoLink drops the link and re-sweeps. If a wire gets bumped, it automatically sweeps the baud spectrum and locks back onto the connection. It manages all the FreeRTOS background tasks, queues, and hardware interrupts automatically. You just send and receive data.


# 🏓 Quick Start: Master / Slave Ping-Pong

The classic use case: two boards bouncing **random-sized messages** back and forth, **logging throughput**, and **self-recovering** from any disruption — with almost no application code. The link's sweep/recovery is automatic; the app just gates on `State::OK`. The examples below also stash every sent payload, compare it against the slave's echo, and log a `MISMATCH` line the moment a single byte goes missing or wrong — the easiest possible smoke test for end-to-end integrity.

## The Master

```cpp
#include "UtilMaster.h"
using namespace autolink;

// Wiring (crossover): Master TX(pin 17) ──► Slave RX(pin 16)
//                     Master RX(pin 16) ◄── Slave TX(pin 17)
// FireBeetle ESP32: GPIO16/17 are not on the header — use 18/19 or 21/22.
UtilMaster um(
    115200,       // Serial debug baud
    UART_NUM_2,   // UART port for the AutoLink wire
    16,           // RX pin
    17,           // TX pin
    "YourSSID",   // WiFi SSID   — omit (or pass nullptr) to disable web monitor
    "password",   // WiFi password
    80            // web server port
);

void setup() { um.setup(); }
void loop()  { um.loop();  }
```

## The Slave

The slave echoes back every complete message. Pass **false** for the master flag.

```cpp
#include "UtilSlave.h"
using namespace autolink;

// Wiring (crossover): Master TX(pin 17) ──► Slave RX(pin 16)
//                     Master RX(pin 16) ◄── Slave TX(pin 17)
// FireBeetle ESP32: GPIO16/17 are not on the header — use 18/19 or 21/22.
UtilSlave us(
    115200,       // Serial debug baud
    UART_NUM_2,   // UART port for the AutoLink wire
    16,           // RX pin
    17,           // TX pin
    "YourSSID",   // WiFi SSID   — omit (or pass nullptr) to disable web monitor
    "password",   // WiFi password
    80            // web server port
);

void setup() { us.setup(); }
void loop()  { us.loop();  }
```

That's the whole thing. No manual reconnect logic, no checksums, no framing, and no state machine to babysit in your sketch. If the cable is yanked mid-stream, both ends fall back to a baud re-sweep and the loops resume on their own — `send()` simply returns `0` until the link is back. A `recv()` of `-1` means a corrupt/desynced message was rejected and the buffer flushed; you can ignore it and keep looping.

# 📡 Web Monitor

`AutoLinkWeb` adds a self-contained WiFi dashboard to any AutoLink sketch. Include `AutoLinkWeb.h`, construct a `mon` object that wraps your `AutoLink`, and call `mon.begin()` once in `setup()` after `comm.begin()`. If WiFi connects, a mobile-friendly page is served — by default on port **8765** (pass a third argument to `begin()` to change it). If WiFi fails, `isUp()` returns `false` and the UART link is completely unaffected.

```cpp
#include "AutoLink.h"
#include "AutoLinkWeb.h"
using namespace autolink;

AutoLink    comm(UART_NUM_2, 16, 17, true);
AutoLinkWeb mon(comm);

void setup() {
    comm.begin();
    mon.begin("YourSSID", "password");       // default port 8765
    // mon.begin("YourSSID", "password", 80); // custom port
}
// Nothing changes in loop() — no handleClient() or polling needed.
```

The dashboard updates once per second and shows:

| Widget | What it shows |
|---|---|
| **State pill** (header) | Green `OK` / amber `LCK` / red `SWP` — animates on transition |
| **TX Rate** | Bytes/s + cumulative total since power-on |
| **RX Rate** | Bytes/s + cumulative total since power-on |
| **Errors** | Current per-link error counter + lifetime disconnect count |
| **WiFi RSSI** | Signal strength in dBm + free heap |
| **Live Log** | Scrolling `[E]`/`[I]`/`[D]` log panel, color-coded by severity |

Controls: **Pause / Resume** stops the 1 Hz polling without closing the page; **Clear** wipes the log DOM while keeping the sequence counter so stale entries are never re-fetched.

**Endpoints** (open, no authentication):

| Endpoint | Description |
|---|---|
| `GET /` | Mobile HTML dashboard |
| `GET /stats` | JSON snapshot: state, B/s rates, totals, RSSI, heap, uptime |
| `GET /logs?since=N` | JSON log entries with seq ≥ N (used for incremental polling) |
| `POST /reset` | Calls `resetStats()` + `resetErrors()`; returns `"ok"` |

> **Note on `resetStats()`:** `AutoLinkWeb` samples the cumulative counters on its own 1 Hz timer and never calls `resetStats()`. If your sketch calls `resetStats()`, the B/s display will read 0 for one interval and then recover automatically.


# 📦 Features Under the Hood


+ **Working Auto-Baud:** During the sweep the master steps through each allowed baud sending `PING`; the slave **retunes in lockstep**, scoring every baud it can actually decode, then both lock onto the *fastest* one that worked. (Pre-2.1 the slave never retuned and always degraded to the slowest baud — fixed in 2.1.)

+ **Boundary-Preserving Messages:** `sendMsg`/`recvMsg` frame arbitrary-length payloads with a length header and an end-to-end CRC-16, independent of the per-frame CRC-8.

+ **Built-in Throughput Metering:** TX/RX byte counters with `getStats()` / `resetStats()` — log B/s without instrumenting your app.

+ **Non-Blocking Core:** A dedicated FreeRTOS task, hardware interrupts, and `StreamBuffers` keep `loop()` responsive. The reliable writer no longer inserts artificial per-chunk delays, so throughput tracks the actual baud rate.

+ **Smart Framing:** In `SWP`/`LCK`, commands are wrapped in CRC-8-validated frames behind a `0xAA 0x55` preamble; electrical noise cannot trigger a false state change. Only the master initiates transitions.

+ **Namespace Isolation:** Everything lives in `namespace autolink`, avoiding collisions with Arduino or ESP-IDF.

+ **Test-Driven Core:** `ALink` is fully decoupled from hardware via the `ILink` interface. Run `make test` to compile and verify the protocol, negotiation, message round-tripping, and CRC handling natively on your build machine.




# 📚 Document Index

| File | Contents |
|------|----------|
| `README.md` | Quick start, current release notes, web monitor reference |
| `docAPI.md` | Message API reference, advanced usage (raw streaming, manual error control), developer notes |
| `docVersion.md` | Full version history for all releases |

# 📜 License

MIT License.

Build something awesome.
