# 🚀 AutoLink ESP32

**A production-grade, self-healing UART protocol layer for ESP32.**


Ever had a UART connection drop because a motor spun up nearby? Have you ever hardcoded a baud rate, only to realize the other microcontroller reset and desynced? Raw UART is notoriously fragile in real-world applications.


**AutoLink** fixes this. It sits on top of the standard ESP-IDF UART drivers and transforms your serial connection into a robust, auto-negotiating, self-healing lifeline.


If the line gets noisy, AutoLink drops the link and re-sweeps. If a wire gets bumped, it automatically sweeps the baud spectrum and locks back onto the connection. It manages all the FreeRTOS background tasks, queues, and hardware interrupts automatically. You just send and receive data.


# ⚡ What's New in 2.8

+ **Lifetime disconnect counter on the API:** `getStats()` now has a 3-arg form `getStats(tx, rx, errs)` that returns the lifetime disconnect count — **one count per link drop**, regardless of cause (bad frame flood tripping the threshold, idle watchdog, peer BREAK, LCK timeout). Per-byte error noise is intentionally not counted. The counter is monotonic across link drops and `resetStats()` — it is only zeroed by the new `resetErrors()`. Designed for longevity testing: "how many bounces has this link survived?"
+ **`resetStats()` / `resetErrors()` are now distinct.** The throughput reset is unchanged (zeros tx/rx), but it no longer touches the disconnect counter, so per-second B/s sampling doesn't wipe the very history that would tell you "errors went up since last sample". `resetErrors()` zeros the disconnect counter explicitly (e.g. on operator ack).
+ The 2-arg `getStats()` form is unchanged. Existing sketches that ignore the new counter see no difference; the README Master example was updated to log the counter alongside throughput.

# ⚡ What's New in 2.7

2.7 is an internal-quality release: the standalone algorithms moved out of the protocol god-file into small, single-purpose utility classes, each with its own exhaustive unit suite. Public API unchanged.

+ **New utility classes** (all reusable, all flat in the repo): `UtilCrc` (CRC-8 + CRC-16/CCITT-FALSE with their LUTs), `UtilCobs` (COBS codec), `UtilBlink` (the async/blocking LED pattern engine behind `blinkWait`, host-testable via an injected `IBlinkHal`), and `UtilFrameRx` (the reliable-mode frame accumulator extracted from the 100-line `onRx` parser). `ALink.cpp` shrank by ~200 lines and now reads as pure protocol.
+ **Five test suites:** `make test` builds and runs `UtilCrcTest`, `UtilCobsTest`, `UtilBlinkTest`, `UtilFrameRxTest`, and the protocol/integration suite (`test.cpp`) — 34 tests total, compiled `-Wall -Wextra` clean. Every class now has direct tests, not just loopback coverage.
+ **🐛 Latent CRC-16 table corruption found and fixed.** The new known-answer test (`"123456789"` → `0x29B1`) exposed four corrupt entries (indices 76–79) in the CRC-16 LUT that had shipped in every prior version. Loopback tests could never catch it because both ends shared the same wrong table. **Wire-compat note:** a 2.7 node exchanging *messages* with a ≤2.6 node will see CRC-16 rejects (`recv()` → `-1`) on payloads whose checksum touches those entries — upgrade both ends together.

# ⚡ What's New in 2.6

+ **Async status LED:** `blinkWait(n)` is now **non-blocking by default** — the flash pattern runs on an `esp_timer` and the call returns immediately, so the per-packet `blinkWait(1)` in the examples no longer stalls the loop (~120 ms saved per echo; round-trip throughput roughly doubles). Pass a trailing `delayMs > 0` and it behaves exactly as before: blocking flash + pause, for pacing a loop.
+ **Watchdog actually armed:** in 2.5 the idle watchdog only ever got one stray timer tick after entering `OK` and then went silent — a dead slave was effectively never detected. Entering `OK` now arms a periodic tick (`idleTimeoutMs / 3`), so the watchdog genuinely fires.
+ **Keepalive:** while in `OK`, each side that has been TX-quiet for a third of the window sends a lone `0x00` the peer's parser skips as an inter-frame zero but counts as RX. A healthy-but-quiet link no longer bounces every `idleTimeoutMs` (reliable mode only — see note below).
+ **App-buffer overflow detected:** a full stream buffer used to drop bytes silently and desync the message stream. The HAL now reports bytes accepted, and a shortfall counts toward the error threshold so the link drops and resyncs instead of corrupting quietly. The auto-sized buffer also grew to two full messages of headroom.
+ **Parser yields after a drop:** when the error threshold trips mid-event, the remainder of that UART event is handed to the SWP command parser instead of being eaten as stale OK-mode frame bytes — the first re-sweep `PING` in the same burst is no longer lost.
+ **Locking hardened:** `dropLink` no longer releases and re-takes the mutex mid-reset, and the `onRx` retune paths run fully locked — closing small interleave windows with `recvMsg`.
+ **UART task stack safety:** the RX scratch buffer moved from `alloca` to a one-time heap allocation, so a large `rxBufferSize` can't overflow the 4 KB event-task stack.
+ **Flat host testing:** `extract_readme.py` now embeds its Arduino/ESP-IDF stubs and writes them to a temp dir, so `make readme` works from the flat repo with no `host_stubs/` directory. The block finder also checks **every** example now (the slave sketch was previously skipped).

> **Note (raw mode):** the keepalive is suppressed when `reliableMode = false` because a `0x00` would reach your app as data. If your raw-mode app can be TX-quiet longer than `idleTimeoutMs`, raise it or set it to `0`.

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
uint32_t  sentSeq = 0;  // sequence number of the stashed payload
bool      wasReady = false;
uint32_t  tStat = 0;
uint32_t  msgSeq = 0;   // monotonically increasing send sequence
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

// Drain every complete echo the slave has sent back, comparing each against
// the most recent stashed send. Echoes are FIFO: the first one out is the
// echo of the first message we put in. If only one message is in flight at
// a time (this example) the first echo is the matching one. If we ever sent
// more than one before the first echo came back, we compare in order and
// the seq number is just for log readability -- the byte comparison is the
// ground truth.
void drainAndCompare() {
    int got;
    while ((got = comm.recv(buf, sizeof buf)) > 0) {
        if (sentLen == 0) {
            // Echo arrived with nothing in flight. Could be a stale echo
            // from before a link drop -- the dropLink_unlocked path clears
            // the app buffer, so this should be rare. Log so it's visible
            // in the trace; not fatal.
            LOG.error("Main", "recv %d bytes with no in-flight send", got);
            continue;
        }
        LOG.debug("Main", "recv %d bytes  sentSeq=%lu", got, (unsigned long)sentSeq);
        // Flash the LED so the link is visually symmetric: master blinks
        // on each echo it receives, matching the slave's per-echo blink.
        comm.blinkWait(1);
        if (got != sentLen) {
            LOG.error("Main",
                "MISMATCH sentSeq=%lu  sent=%d bytes  echoed=%d bytes",
                (unsigned long)sentSeq, sentLen, got);
        } else if (memcmp(buf, sent, got) != 0) {
            int firstBad = -1;
            for (int i = 0; i < got; i++) {
                if (buf[i] != sent[i]) { firstBad = i; break; }
            }
            LOG.error("Main",
                "MISMATCH seq=%lu %d bytes differ, first bad offset=%d "
                "expected 0x%02X got 0x%02X",
                (unsigned long)sentSeq, got, firstBad,
                sent[firstBad >= 0 ? firstBad : 0],
                buf[firstBad >= 0 ? firstBad : 0]);
        }
        sentLen = 0;
    }
    // got == -1 is a CRC reject -- the bad message was drained and counted.
    // Leave sentLen alone in that case so a clean retry is possible.
    if (got < 0) {
        LOG.error("Main", "recv rejected (CRC/desync) sentSeq=%lu",
                  (unsigned long)sentSeq);
        sentLen = 0;
    }
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
       // After a (re)connect, drain anything the slave queued during the
       // gap -- the peer usually has at least one message for us.
       drainAndCompare();
       comm.blinkWait(4);
       wasReady = true;
    }

    // linked: send a fresh random payload, then wait for its echo before
    // sending the next one. This keeps the round-trip "in flight" count at
    // 1, which makes the byte compare unambiguous. (Faster apps that want
    // pipelining should track a queue of pending sends, not just one.)
    if (sentLen == 0) {
        int n = random(1, 1024);
        fill(buf, n);
        if (comm.send(buf, n)) {
            sentLen  = n;
            sentSeq  = msgSeq++;
            memcpy(sent, buf, n);
            LOG.debug("Main", "sent %d bytes seq=%lu", n, (unsigned long)sentSeq);
        } else {
            LOG.error("Main", "send failed (link dropped mid-send)");
        }
    }

    // Drain any echo that's already arrived. If the echo isn't back yet
    // we'll just loop on the next iteration -- comm.ready() will keep us
    // from sending a new one until the previous round trip completes.
    drainAndCompare();

    // log throughput + lifetime disconnect count once a second.
    // The disconnect counter is monotonic across samples and link drops --
    // call resetErrors() explicitly if you want to zero it (e.g. after
    // an operator ack), never via resetStats(), which is for B/s deltas.
    if (millis() - tStat > 1000) {
        uint64_t tx, rx, errs; comm.getStats(tx, rx, errs); comm.resetStats();
        LOG.debug("Main", "TX %lu B/s RX %lu B/s err=%lu",
                  (unsigned long)tx, (unsigned long)rx, (unsigned long)errs);
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
| Bursts of **3**, with a pause, repeating | Searching / negotiating baud (`ready()` is false) |
| A burst of **4** | Just connected |
| One quick flash | One packet sent (master) / echoed (slave) |

The LED defaults to **GPIO 2** (the blue LED on most ESP32 dev boards); point it elsewhere with `cfg.ledPin` and tune the flash with `blinkWait(n, onMs, offMs)`.

`blinkWait` has two modes, picked by the last parameter:

+ **Async (default, `delayMs` omitted or 0):** returns immediately; the pattern runs on an `esp_timer` in the background. This makes a per-packet heartbeat free — `blinkWait(1)` on every send/echo costs your loop nothing. A new call replaces any pattern still running.
+ **Blocking (`delayMs > 0`):** flashes, then pauses `delayMs` — holds `loop()` for `n * (onMs + offMs) + delayMs` ms. The UART keeps receiving into the app buffer while you wait, but your code won't drain it until the call returns. `blinkWait(3, 100, 100, 2000)` is what paces the searching loop in the examples.


# 📨 The Message API

This is the whole public surface for normal use:

| Call | Returns | Notes |
|------|---------|-------|
| `int send(const uint8_t* b, int len)` | `len` if queued, `0` if the link is down/busy | `len` must be `1..maxMsg` (default 1024). Safe to call every loop. |
| `int recv(uint8_t* b, int max)` | `>0` message length, `0` nothing ready, `-1` rejected/dropped | `max` should be `>= maxMsg`. On `-1` the bad message is drained and an error is counted. |
| `bool ready()` | `true` once negotiated | Optional — `send`/`recv` already gate themselves, so you rarely need this. |
| `void getStats(uint64_t& tx, uint64_t& rx)` | — | App-stream bytes since the last `resetStats()`. |
| `void getStats(uint64_t& tx, uint64_t& rx, uint64_t& errs)` | — | Adds the lifetime disconnect count (one per link drop). Monotonic across samples and link drops; only zeroed by `resetErrors()`. |
| `void resetStats()` | — | Zero the tx/rx counters. **Does not** touch the disconnect counter. |
| `void resetErrors()` | — | Zero the lifetime disconnect counter. |
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

+ **Hardware Abstraction (Dependency Injection):** Core logic (`ALink.cpp`) is decoupled from the ESP32 (`EspHal.h`) via the `ILink` interface; the LED engine is likewise decoupled via `IBlinkHal`. Keep ESP-IDF / FreeRTOS headers out of `ALink.cpp` and the `Util*` classes.

+ **Utility Classes:** standalone algorithms live in single-purpose, reusable classes — `UtilCrc` (checksums + LUTs), `UtilCobs` (codec), `UtilBlink` (LED patterns), `UtilFrameRx` (reliable-frame accumulator). Each has a purpose comment at the top and its own `*Test.cpp` suite. `ALink` consumes them and keeps only protocol logic.

+ **Native PC Testing:** Because of that abstraction, the entire stack runs on your computer. `make test` builds and runs five suites: `UtilCrcTest`, `UtilCobsTest`, `UtilBlinkTest`, `UtilFrameRxTest`, and the protocol/integration tests in `test.cpp`. Everything compiles `-Wall -Wextra` clean.

+ **State Machine:**
  + `SWP` (Sweep): master iterates allowed bauds sending `PING`; the slave retunes per ping and scores each baud it decodes.
  + `LCK` (Lock): master requests the best baud; the slave replies with the fastest scored index and both switch.
  + `OK` (Connected): raw bytes or reliable frames / messages are exchanged.

+ **Reliable Mode:** User data is encapsulated in COBS frames (≤250 B payload each) with a trailing CRC-8, delimited by `0x00` sentinels, so the receiver can discard a corrupt frame without losing stream sync. Note that "reliable" means *detected-and-dropped*, not retransmitted — for guaranteed delivery, layer an ack/retry on top (the ping-pong echo pattern above is one).

+ **CRC:** A precomputed 256-byte LUT gives O(1) CRC-8 for frames; messages add a CRC-16/CCITT computed over the full payload.

+ **Buffer Sizing:** `MAX_CHUNK` (250) drives the static frame buffers via `static_assert`. Keep `maxMsg <= streamBufferSize`.


# 📅 Revision History

**v2.8.0**

+ **Lifetime disconnect counter:** `getStats()` now has a 3-arg form `getStats(tx, rx, errs)` that returns a lifetime count of link drops (one per drop, regardless of cause). `resetStats()` zeros only tx/rx; the new `resetErrors()` zeros the disconnect counter. For longevity testing.

**v2.7.0**

+ **Refactor to utility classes:** `UtilCrc`, `UtilCobs`, `UtilBlink` (+ `IBlinkHal`/`EspBlinkHal`), and `UtilFrameRx` extracted from `ALink.cpp` / `AutoLink.h`. `ALink` implements `UtilFrameRx::Listener`; `AutoLink::blinkWait` is a two-line forward into `UtilBlink`. Public API unchanged.
+ **Per-class unit suites:** `UtilCrcTest`, `UtilCobsTest`, `UtilBlinkTest`, `UtilFrameRxTest` (24 new tests: known-answer CRC vectors, single-bit error detection, COBS 0xFF group boundaries and malformed-input rejection, exact LED on/off/timer sequences, frame splitting/desync/drop semantics). `make test` runs all five binaries; build is `-Wall -Wextra` clean.
+ **CRC-16 LUT fix:** four corrupt entries (76–79) in the CCITT-FALSE table, present since the LUT was introduced, found by the new known-answer test and regenerated. See the wire-compat note in What's New.
+ **Hygiene:** every class carries a purpose summary comment; `AutoLink` copy/move deleted (the blink timer captures `this`).

**v2.6.0**

+ **Async `blinkWait`:** with the default `delayMs == 0` the call is non-blocking — the flash pattern runs on an `esp_timer` and a new call replaces a running one. `delayMs > 0` keeps the original blocking flash + pause. Per-packet LED feedback no longer throttles throughput.
+ **OK-state tick:** entering `OK` arms a periodic timer (`idleTimeoutMs / 3`). Fixes the 2.5 idle watchdog, which was only ever checked on one leftover LCK tick and then never again — a silently dead peer went undetected.
+ **Keepalive:** a TX-quiet side in `OK` emits a lone `0x00` each tick (reliable mode only); the peer skips it as an inter-frame zero but its watchdog counts it. Healthy-but-quiet links no longer re-sweep spuriously.
+ **Overflow accounting:** `ILink::pushAppBuf(buf, n)` now returns bytes accepted; a shortfall (full stream buffer) counts as a link error so persistent overflow drops and resyncs the link instead of silently corrupting the message stream. `AutoLink` auto-sizes the buffer to `2 * (maxMsg + MSG_HDR)`.
+ **Parser yield on drop:** after the error threshold trips mid-event, the rest of the UART event goes to the command parser, so a re-sweep `PING` arriving in the same burst is handled immediately.
+ **Locking:** `dropLink` holds the mutex throughout; `onRx` retune paths no longer unlock mid-parse. `ILink` gained `nowMs()` so the watchdog/keepalive clock is injectable — host tests can now drive time, and the suite grew tests for the watchdog, keepalive, LCK timeout, buffer overflow, and parser yield.
+ **HAL:** UART event-task scratch buffer moved from `alloca` to a one-time heap allocation (stack-overflow risk with large `rxBufferSize`).
+ **Tooling:** `extract_readme.py` embeds its Arduino/ESP-IDF stubs (temp dir at run time; no `host_stubs/`), and its block finder now checks every README example instead of only the first.

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
