# 🚀 AutoLink ESP32

**A production-grade, self-healing UART protocol layer for ESP32.**


Ever had a UART connection drop because a motor spun up nearby? Have you ever hardcoded a baud rate, only to realize the other microcontroller reset and desynced? Raw UART is notoriously fragile in real-world applications.


**AutoLink** fixes this. It sits on top of the standard ESP-IDF UART drivers and transforms your serial connection into a robust, auto-negotiating, self-healing lifeline.


If the line gets noisy, AutoLink drops the link and re-sweeps. If a wire gets bumped, it automatically sweeps the baud spectrum and locks back onto the connection. It manages all the FreeRTOS background tasks, queues, and hardware interrupts automatically. You just send and receive data.


# ⚡ What's New in 2.1

AutoLink now has a **message API** on top of the byte stream. `sendMsg()` / `recvMsg()` preserve message boundaries for **arbitrary, random-sized payloads** and protect each message end-to-end with a CRC-16 — so a `write` of 4096 random bytes comes out the other side as exactly one 4096-byte message, intact or not at all. Built-in **throughput counters** let you log B/s with a single call, and the auto-baud sweep now actually retunes the slave so it selects the *fastest working* baud instead of always falling back to the slowest.


# 🏓 Quick Start: Master / Slave Ping-Pong

The classic use case: two boards bouncing **random-sized messages** back and forth, **logging throughput**, and **self-recovering** from any disruption — with almost no application code. The link's sweep/recovery is automatic; the app just gates on `State::OK`.

## The Master

```cpp
#include "AutoLink.h"
using namespace autolink;

AutoLink* link = nullptr;
uint8_t   buf[4096];
uint32_t  tRound = 0, tStat = 0;

void fill(uint8_t* b, int n) { for (int i = 0; i < n; i++) b[i] = random(256); }

void setup() {
    Serial.begin(115200);
    randomSeed(esp_random());

    AutoLinkConfig cfg;
    cfg.reliableMode    = true;   // required for the message API
    cfg.streamBufferSize = 8192;  // must exceed maxMsg
    cfg.maxMsg          = 4096;

    link = new AutoLink(UART_NUM_1, 16, 17, true, cfg); // true = master
    link->begin();                                      // kicks off the baud sweep
}

void loop() {
    if (link->getState() != State::OK) { delay(50); return; } // sweep self-heals

    // Fire one random-sized message per round.
    if (millis() - tRound > 20) {
        int n = random(1, 4096);
        fill(buf, n);
        if (link->sendMsg(buf, n)) tRound = millis();       // false => link busy/down, retry next loop
    }

    // Drain any echoes the slave bounced back.
    int n;
    while ((n = link->recvMsg(buf, sizeof(buf))) > 0) { /* got a full message back */ }

    // Log throughput once a second.
    if (millis() - tStat > 1000) {
        uint64_t tx, rx; link->getStats(tx, rx); link->resetStats();
        Log::getLog().info("App", "TX %lu B/s   RX %lu B/s",
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

AutoLink* link = nullptr;
uint8_t   buf[4096];
uint32_t  tStat = 0;

void setup() {
    Serial.begin(115200);

    AutoLinkConfig cfg;
    cfg.reliableMode    = true;
    cfg.streamBufferSize = 8192;
    cfg.maxMsg          = 4096;

    link = new AutoLink(UART_NUM_1, 16, 17, false, cfg); // false = slave
    link->begin();                                       // arms in SWP, waits for master
}

void loop() {
    if (link->getState() != State::OK) { delay(50); return; }

    int n;
    while ((n = link->recvMsg(buf, sizeof(buf))) > 0) {
        link->sendMsg(buf, n); // echo it straight back
    }

    if (millis() - tStat > 1000) {
        uint64_t tx, rx; link->getStats(tx, rx); link->resetStats();
        Log::getLog().info("App", "TX %lu B/s   RX %lu B/s",
                           (unsigned long)tx, (unsigned long)rx);
        tStat = millis();
    }
}
```

That's the whole thing. No manual reconnect logic, no checksums, no framing in your sketch. If the cable is yanked mid-stream, both ends fall back to `SWP`, re-negotiate, and the loops resume on their own the moment `getState()` returns `OK` again. A `recvMsg()` of `-1` means a corrupt/desynced message was rejected and the buffer flushed — you can ignore it and keep looping.


# 📨 The Message API

| Call | Returns | Notes |
|------|---------|-------|
| `bool sendMsg(const uint8_t* b, int len)` | `true` if the whole message was queued | `len` must be `1..maxMsg`. Returns `false` if the link isn't `OK`. |
| `int recvMsg(uint8_t* b, int max)` | `>0` message length, `0` nothing ready, `-1` rejected/dropped | `max` should be `>= maxMsg`. On `-1` the bad message is drained and an error is counted. |
| `void getStats(uint64_t& tx, uint64_t& rx)` | — | App-stream bytes since the last reset. |
| `void resetStats()` | — | Zero the counters (call after each sample to get B/s). |

Each message goes out as a 6-byte header (`len` + `crc16`) followed by the payload, chunked into ≤250-byte COBS frames, each guarded by a per-frame CRC-8. The receiver only hands you a message once the **whole** payload arrives and its CRC-16 verifies — so you never see a half-message or a corrupted one.

> **Sizing rule:** `maxMsg <= streamBufferSize`. A whole message is buffered for reassembly, so the stream buffer must be able to hold the largest message you send. AutoLink logs an error at construction if you get this backwards.


# 🔌 Raw Byte Streaming

If you don't need message boundaries, AutoLink is still a drop-in `Stream`. Leave `reliableMode` off for an unframed pass-through, or on for COBS+CRC-8 framed bytes:

```cpp
const char* str = "Hello Slave!";
link->write((const uint8_t*)str, strlen(str));

while (link->available()) {
    uint8_t b[64];
    int len = link->read(b, sizeof(b));
    Log::getLog().info("App", "Got %d bytes", len);
}
```

`write()` returns the number of bytes actually accepted (0 if the link isn't `OK`), so you always know whether your data made it onto the wire.


# 🧠 Advanced: Manual Error Control

For raw-mode applications doing their own validation, you can drive the error counter yourself. Exceeding `errThreshold` automatically issues a hardware BREAK and drops the link back to `SWP`.

```cpp
if (link->getState() == State::OK && link->available() >= 5) {
    uint8_t p[5];
    link->read(p, 5);
    if ((p[0]^p[1]^p[2]^p[3]) == p[4]) link->clearErr();      // good data decays the counter
    else {
        link->err();                                          // bad data; after errThreshold -> SWP
        Log::getLog().error("App", "Corrupt! count=%d", link->getErrCount());
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
