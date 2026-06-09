# 🚀 AutoLink ESP32

**A production-grade, self-healing UART protocol layer for ESP32.**


Ever had a UART connection drop because a motor spun up nearby? Have you ever hardcoded a baud rate, only to realize the other microcontroller reset and desynced? Raw UART is notoriously fragile in real-world applications.


Ever had a UART connection drop because a motor spun up nearby? Have you ever hardcoded a baud rate, only to realize the other microcontroller reset and desynced? Raw UART is notoriously fragile in real-world applications.


**AutoLink** fixes this. It sits on top of the standard ESP-IDF UART drivers and transforms your serial connection into a robust, auto-negotiating, self-healing lifeline.


If the line gets noisy, AutoLink drops the link and re-sweeps. If a wire gets bumped, it automatically sweeps the baud spectrum and locks back onto the connection. It manages all the FreeRTOS background tasks, queues, and hardware interrupts automatically. You just send and receive data.


# ⚡ What's New in 2.5

The previous versions recovered gracefully from a **yanked cable** (symmetric disruption: both sides see noise), but a **silent peer death** — slave power-cycle, MCU hard-fault, UART stuck — could deadlock the master in `LCK` sending `REQ` forever, and the slave would never re-arm itself even after its own error counter tripped. **2.5 fixes the asymmetric recovery path end-to-end:**

+ **Idle-channel watchdog** (`cfg.idleTimeoutMs`, default 3000 ms): while in `OK`, if no RX bytes arrive for the configured window, the link is dropped locally and a `BREAK` is sent to the peer. The master is the originator of traffic in most sketches, so its RX pin goes silent when the slave dies — without this watchdog it would sit in `OK` indefinitely.
+ **Self-resetting err path**: `err_unlocked()` now performs the same local state transition (`SWP` + retune to `allowedBauds[0]`) that `onBreak()` does. Previously, a side that tripped its own error counter would send a `BREAK` to the peer but leave its own UART tuned to the locked baud — the peer's re-sweep was then inaudible. Now both sides re-arm symmetrically.
+ **LCK timeout**: if the master sends `REQ` with no slave reply for more than `2 * allowedBauds.size()` ticks, the sweep restarts from `allowedBauds[0]`. Closes the "dead peer, master stuck in `LCK`" hole that v2.4 left open.
+ **BREAK debounce + UART FIFO flush on retune**: line glitches that fire `UART_BREAK` for a few hundred microseconds are now collapsed (anything tighter than 50 ms is dropped but the FIFO is still drained). `setSpd()` flushes the RX FIFO before changing baud so stale samples from the previous baud never feed the parser as garbage on the first byte of a new sweep.

# ⚡ What's New in 2.2

The public API is now **one object and two verbs**. Construct an `AutoLink` as a global, call `begin()`, then `send()` / `recv()` — reliable framing, CRC protection, buffer sizing, and link recovery are all on by default and handled under the covers. No config struct, no `new`, no state machine to gate on in your sketch. The old `sendMsg`/`recvMsg`, the raw `Stream` byte API, and manual error control are still there under **Advanced** for anyone who wants them.




# 🏓 Quick Start: Master / Slave Ping-Pong

The classic use case: two boards bouncing **random-sized messages** back and forth, **logging throughput**, and **self-recovering** from any disruption — with almost no application code. The link's sweep/recovery is automatic; the app just gates on `State::OK`. The examples below also stash every sent payload, compare it against the slave's echo, and log a `MISMATCH` line the moment a single byte goes missing or wrong — the easiest possible smoke test for end-to-end integrity.

## The Master

```cpp
#include "AutoLink.h"
using namespace autolink;
//pin 16=rcv 17=tx
AutoLink comm(UART_NUM_2, 16, 17, true);
uint8_t   buf[1024];
uint8_t   sent[1024];   // stash the last payload so we can compare with the echo
int       sentLen = 0;  // length of the stashed payload; 0 = nothing in flight
bool      wasReady = false;
uint32_t  tStat = 0;
uint32_t  msgSeq = 0;   // monotonically increasing, included in the payload so
                        // mismatches are easy to spot in the log
Log&      LOG = Log::getLog();
void fill(uint8_t* b, int n) { for (int i = 0; i < n; i++) b[i] = random(256); }

void setup() {
    esp_log_level_set("*",ESP_LOG_VERBOSE);
    LOG.setLevel(Log::DEBUG);
    Serial.begin(115200);
    randomSeed(esp_random());
    comm.blinkWait(1, 100, 100, 2000);
    comm.begin();  // baud sweep
    comm.blinkWait(2, 100, 100, 2000);
}

void loop() {
    // search for link
    if (!comm.ready()) {
       LOG.debug("Main", "not ready");
       comm.blinkWait(3, 100, 100, 2000);
       wasReady = false;
       sentLen = 0;          // a link drop invalidates any in-flight compare
       return;
    }
    // connected
    if (!wasReady) {
       LOG.debug("Main", "ready");
       comm.blinkWait(4);
       wasReady = true;
    }

    // linked: send a fresh random payload. Stash it for the echo check below.
    int n = random(1, 1024);
    fill(buf, n);
    if (comm.send(buf, n)) {
        sentLen   = n;
        memcpy(sent, buf, n);
        LOG.debug("Main", "sent %d bytes  seq=%lu", n, (unsigned long)msgSeq);
        comm.blinkWait(1);
    } else {
        LOG.error("Main", "send failed (link dropped mid-send)");
        sentLen = 0;
    }

    // Drain one echo, compare against the stashed send, and log the byte count.
    int got = comm.recv(buf, sizeof buf);
    if (got > 0) {
        LOG.debug("Main", "recv %d bytes  seq=%lu", got, (unsigned long)msgSeq);
        if (got != sentLen) {
            LOG.error("Main",
                "MISMATCH seq=%lu  sent=%d bytes  echoed=%d bytes",
                (unsigned long)msgSeq, sentLen, got);
        } else if (memcmp(buf, sent, got) != 0) {
            int firstBad = -1;
            for (int i = 0; i < got; i++) {
                if (buf[i] != sent[i]) { firstBad = i; break; }
            }
            LOG.error("Main",
                "MISMATCH seq=%lu  %d bytes differ, first bad offset=%d  "
                "expected 0x%02X got 0x%02X",
                (unsigned long)msgSeq, got, firstBad,
                sent[firstBad >= 0 ? firstBad : 0],
                buf[firstBad >= 0 ? firstBad : 0]);
        }
        msgSeq++;
        sentLen = 0;
    } else if (got < 0) {
        // -1 = CRC reject / desync; the bad message was drained by recvMsg.
        // Don't bump msgSeq -- the round trip is incomplete.
        LOG.error("Main", "recv rejected (CRC/desync) seq=%lu",
                  (unsigned long)msgSeq);
        sentLen = 0;
    }

    // log throughput once a second.
    if (millis() - tStat > 1000) {
        uint64_t tx, rx; comm.getStats(tx, rx); comm.resetStats();
        LOG.debug("Main", "TX %lu B/s   RX %lu B/s",
                  (unsigned long)tx, (unsigned long)rx);
        tStat = millis();
    }
}
```

## The Slave

The slave just echoes back whatever complete messages arrive. Pass **false** for the master flag.

```cpp
#include "AutoLink.h"
using namespace autolink;
//pin 16=rcv 17=tx
AutoLink comm(UART_NUM_2, 16, 17, false);
uint8_t   buf[1024];
bool      wasReady = false;
Log&      LOG = Log::getLog();
void setup() {
    esp_log_level_set("*",ESP_LOG_VERBOSE);
    LOG.setLevel(Log::DEBUG);
    Serial.begin(115200);
    comm.blinkWait(1, 100, 100, 2000);
    comm.begin(); // SWP, waits for master
    comm.blinkWait(2, 100, 100, 2000);
}

void loop() {
    if (!comm.ready()) {
       LOG.debug("Main", "comm not ready");
       comm.blinkWait(3, 100, 100, 2000);
       wasReady = false;
       return;
   }
    if (!wasReady) {
       LOG.debug("Main", "comm ready");
       comm.blinkWait(4, 100, 100, 2000);
       wasReady = true;
   }

    int n;
    while ((n = comm.recv(buf, sizeof buf)) > 0) {
        LOG.debug("Main", "recv %d bytes", n);
        if (comm.send(buf, n)) {
            LOG.debug("Main", "echoed %d bytes", n);
        } else {
            LOG.error("Main", "echo send failed (link dropped)  %d bytes dropped", n);
        }
        comm.blinkWait(1);
    }
}
```

That's the whole thing. No manual reconnect logic, no checksums, no framing, and no state machine to babysit in your sketch. If the cable is yanked mid-stream, both ends fall back to a baud re-sweep and the loops resume on their own — `send()` simply returns `0` until the link is back. A `recv()` of `-1` means a corrupt/desynced message was rejected and the buffer flushed; you can ignore it and keep looping.

## 🔵 Status LED

`blinkWait(n)` flashes the onboard blue LED `n` times so you can read the link's state at a glance, no serial monitor needed. The examples above use a simple convention:

| What you see | Meaning |
|---|---|
| One slow blink, repeating | Searching / negotiating baud (`ready()` is false) |
| A quick burst of **3** | Just connected |
| One blink per flash | One packet sent (master) / echoed (slave) |

The LED defaults to **GPIO 2** (the blue LED on most ESP32 dev boards). Point it elsewhere with `cfg.ledPin`, tune the flash length with `blinkWait(n, onMs, offMs)`, and pass an optional `delayMs` to pause after the flash — `blinkWait(1, 60, 60, 200)` flashes once then waits 200 ms, handy for pacing a send loop. `blinkWait()` is blocking, so it holds `loop()` for the full flash + delay duration; the UART keeps receiving into the app buffer, but your own code won't drain it until `blinkWait()` returns. Use it for one-shot bring-up markings and short pacing delays, not for a per-packet heartbeat at speed.


# 📨 The Message API

This is the whole public surface for normal use:

| Call | Returns | Notes |
|------|---------|-------|
| `int send(const uint8_t* b, int len)` | `len` if queued, `0` if the link is down/busy | `len` must be `1..maxMsg` (default 1024). Safe to call every loop. |
| `int recv(uint8_t* b, int max)` | `>0` message length, `0` nothing ready, `-1` rejected/dropped | `max` should be `>= maxMsg`. On `-1` the bad message is drained and an error is counted. |
| `bool ready()` | `true` once negotiated | Optional — `send`/`recv` already gate themselves, so you rarely need this. |
| `void getStats(uint64_t& tx, uint64_t& rx)` | — | App-stream bytes since the last reset. |
| `void resetStats()` | — | Zero the counters (call after each sample to get B/s). |

Each message goes out as a 6-byte header (`len` + `crc16`) followed by the payload, chunked into ≤250-byte COBS frames, each guarded by a per-frame CRC-8. The receiver only hands you a message once the **whole** payload arrives and its CRC-16 verifies — so you never see a half-message or a corrupted one.

> **No sizing to worry about:** set `cfg.maxMsg` to the largest message you'll send and the reassembly buffer is grown to fit automatically. Leave it alone for the 1 KB default.


# 🔌 Advanced

Most sketches never need anything below this line. The simple `send`/`recv`/`ready` API above covers the common case.

### Raw Byte Streaming

If you don't need message boundaries, `AutoLink` is still a drop-in `Stream`. Set `cfg.reliableMode = false` for an unframed pass-through, or leave it on for COBS+CRC-8 framed bytes:

```cpp
const char* str = "Hello Slave!";
comm.write((const uint8_t*)str, strlen(str));

while (comm.available()) {
    uint8_t b[64];
    int len = comm.read(b, sizeof(b));
    Log::getLog().info("App", "Got %d bytes", len);
}
```

`write()` returns the number of bytes actually accepted (0 if the link isn't ready), so you always know whether your data made it onto the wire.

### Manual Error Control

For raw-mode applications doing their own validation, you can drive the error counter yourself. Exceeding `errThreshold` automatically issues a hardware BREAK and drops the link into a re-sweep.

```cpp
if (comm.ready() && comm.available() >= 5) {
    uint8_t p[5];
    comm.read(p, 5);
    if ((p[0]^p[1]^p[2]^p[3]) == p[4]) comm.clearErr();      // good data decays the counter
    else {
        comm.err();                                          // bad data; after errThreshold -> re-sweep
        Log::getLog().error("App", "Corrupt! count=%d", comm.getErrCount());
    }
}
```


# 📦 Features Under the Hood


+ **Working Auto-Baud:** During the sweep the master steps through each allowed baud sending `PING`; the slave **retunes in lockstep**, scoring every baud it can actually decode, then both lock onto the *fastest* one that worked. (Pre-2.1 the slave never retuned and always degraded to the slowest baud — fixed in 2.1.)

+ **Boundary-Preserving Messages:** `sendMsg`/`recvMsg` frame arbitrary-length payloads with a length header and an end-to-end CRC-16, independent of the per-frame CRC-8.

+ **Built-in Throughput Metering:** TX/RX byte counters with `getStats()` / `resetStats()` — log B/s without instrumenting your app.

+ **Non-Blocking Core:** A dedicated FreeRTOS task, hardware interrupts, and `StreamBuffers` keep `loop()` responsive. The reliable writer no longer inserts artificial per-chunk delays, so throughput tracks the actual baud rate.

+ **Smart Framing:** In `SWP`/`LCK`, commands are wrapped in CRC-8-validated frames behind a `0xAA 0x55` preamble; electrical noise cannot trigger a false state change. Only the master initiates transitions.

+ **Namespace Isolation:** Everything lives in `namespace autolink`, avoiding collisions with Arduino or ESP-IDF.

+ **Test-Driven Core:** `ALink` is fully decoupled from hardware via the `ILink` interface. Run `make test` to compile and verify the protocol, negotiation, message round-tripping, and CRC handling natively on your build machine.


# 🛠️ Developer Notes

+ **Hardware Abstraction (Dependency Injection):** Core logic (`ALink.cpp`) is decoupled from the ESP32 (`EspHal.h`) via the `ILink` interface. Keep ESP-IDF / FreeRTOS headers out of `ALink.cpp`.

+ **Native PC Testing:** Because of that abstraction, the entire state machine and message layer run on your computer. `make test` builds and runs the mock-hardware tests in `test.cpp`.

+ **State Machine:**
  + `SWP` (Sweep): master iterates allowed bauds sending `PING`; the slave retunes per ping and scores each baud it decodes.
  + `LCK` (Lock): master requests the best baud; the slave replies with the fastest scored index and both switch.
  + `OK` (Connected): raw bytes or reliable frames / messages are exchanged.

+ **Reliable Mode:** User data is encapsulated in COBS frames (≤250 B payload each) with a trailing CRC-8, delimited by `0x00` sentinels, so the receiver can discard a corrupt frame without losing stream sync. Note that "reliable" means *detected-and-dropped*, not retransmitted — for guaranteed delivery, layer an ack/retry on top (the ping-pong echo pattern above is one).

+ **CRC:** A precomputed 256-byte LUT gives O(1) CRC-8 for frames; messages add a CRC-16/CCITT computed over the full payload.

+ **Buffer Sizing:** `MAX_CHUNK` (250) drives the static frame buffers via `static_assert`. Keep `maxMsg <= streamBufferSize`.


# 📅 Revision History

**v2.5.0**

+ **Asymmetric peer-death recovery:** slave's `err_unlocked()` now performs the same local state reset (`SWP` + retune to `allowedBauds[0]` + score/buffer clear) that `onBreak()` does, so a side that trips its own error counter re-arms itself instead of waiting for the peer to drive the sweep. Closes the deadlock where the master was the originator of traffic and never saw RX errors, leaving both sides stuck.
+ **Idle-channel watchdog:** new `cfg.idleTimeoutMs` (default 3000 ms, set 0 to disable). In `OK`, if no RX activity for that long, the link is dropped locally and a `BREAK` is sent to the peer. The master's RX pin goes silent when the slave powers off; this is the only way the master notices in the originator-of-traffic case.
+ **LCK timeout:** master tracks `lckRetries` and restarts the sweep if more than `2 * allowedBauds.size()` `REQ` ticks pass with no slave reply. Prevents the master from getting stuck in `LCK` forever when the peer is gone.
+ **BREAK debounce:** spurious `UART_BREAK` events closer than 50 ms apart are collapsed; the FIFO is still drained so a noise burst can't leave stale bytes in the parser. Legitimate breaks from a peer `sendBreak()` or a hard reset always pass through.
+ **FIFO flush on retune:** `EspHal::setSpd()` now calls `uart_flush_input()` before `uart_set_baudrate()`. The first bytes after a baud change are no longer samples from the old baud, so a clean re-negotiation doesn't accumulate false-positive errors that would re-trip the threshold and bounce the link.

**v2.4.0**

+ **Thread safety:** `recvMsg()` and the reliable-mode `onRx` path now hold the protocol mutex for the full reassembly / frame-parse sequence, eliminating a data race between the UART task and the app loop that the host tests could not exercise. `err_unlocked()` was added so the parser can count errors without re-entering the lock.
+ **Reliable RX parser hardening:** bad CRC, malformed COBS, and buffer overflow no longer `break` out of the event (which could leave the parser mid-frame on the next call). They now `err_unlocked()` and keep scanning, so back-to-back frames in one event are processed.
+ **COBS decode speed:** inner loop replaced with `memcpy`, ~5–10× faster on ESP32 at high baud. CRC-16/CCITT inner loop replaced with a 256-entry LUT (constexpr, lives in flash) for O(1) per byte.
+ **Write/sendMsg locking:** a single mutex acquisition per frame in `write()`, and `sendMsg()` now holds the lock across header + payload so the link can't drop between them. New private `writeLocked()` / `sendFrame_unlocked()` helpers back the new flow.
+ **UART event task:** `std::vector<uint8_t> rx_buf` replaced with an `alloca`-backed scratch buffer to avoid per-iteration alloc/ctor churn on a 4 KB stack task.
+ **Status LED rename:** `blink()` → `blinkWait()` to make the blocking behavior obvious at the call site. No behavior change.

**v2.3.0**

+ **Status LED:** added `blink(n, onMs, offMs, delayMs)` to flash the onboard blue LED for at-a-glance link state, with an optional trailing `delayMs` pause to pace a loop. Configurable pin via `cfg.ledPin` (default GPIO 2). The master/slave examples now show a searching heartbeat, a 3-blink connect burst, and one blink per packet. (Renamed to `blinkWait()` in 2.4.0; behavior unchanged.)

**v2.2.0**

+ **One-object API:** construct `AutoLink` on the stack (no `new`/pointer), call `begin()`, then `send()` / `recv()`. Added `ready()` so `State` never has to appear in user code.
+ **Reliable by default:** `reliableMode` now defaults on, so the message API is protected out of the box.
+ **Auto-sized buffers:** the reassembly buffer grows from `maxMsg` automatically; the old `maxMsg <= streamBufferSize` rule (and its construction-time error, which the *default* config used to trip) is gone.
+ **Saner defaults:** `maxMsg` defaults to 1 KB instead of 8 KB, so a default-constructed link no longer over-allocates.
+ Raw `Stream` byte API, explicit `sendMsg`/`recvMsg`, and manual error control retained under **Advanced** — no behavior change.

**v2.1.1**

+ **Auto-Baud Fix (critical):** the slave now retunes its baud in lockstep with the master during the sweep, so scoring works on real hardware and the link selects the fastest working baud instead of always collapsing to `allowedBauds[0]`.
+ **Message API:** added `sendMsg()` / `recvMsg()` — boundary-preserving, length-framed, CRC-16-protected transfer of arbitrary random-sized payloads.
+ **Throughput Counters:** `getStats()` / `resetStats()` expose TX/RX byte totals for one-call B/s logging.
+ **Write Feedback:** `write()` now returns the bytes actually accepted (0 when the link is down) instead of silently claiming success.
+ **Throughput Ceiling Removed:** dropped the artificial 1 ms per-chunk delay in the reliable writer; throughput now follows the real baud rate.
+ **Memory & Safety:** replaced per-chunk heap allocations in the writer with static scratch buffers; added `static_assert` coupling between `MAX_CHUNK` and the frame buffers; construction warns if `maxMsg > streamBufferSize`.

**v2.0.0 (Production-Ready)**

+ Inherited `AutoLink` from the Arduino `Stream` class for native `.println()` / `.parseInt()` compatibility.
+ Added opt-in `reliableMode` (COBS + CRC-8 framing).
+ Replaced bitwise CRC loops with an O(1) 256-byte LUT.
+ Concurrency fixes: bounded `uart_event_task` waits for clean teardown; held the TX mutex across full transmissions.
+ Memory safety: `std::make_unique` allocations and patched init-failure leaks.
+ Consolidated the constructor into an `AutoLinkConfig` struct.

**v1.0.0 (Initial Prototype)**

+ Initial master/slave auto-baud negotiation.
+ Basic FreeRTOS stream buffer and hardware interrupt integration.


# 📜 License

MIT License.

Build something awesome.
