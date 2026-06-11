# 🚀 AutoLink ESP32

**A production-grade, self-healing UART protocol layer for ESP32.**


Ever had a UART connection drop because a motor spun up nearby? Have you ever hardcoded a baud rate, only to realize the other microcontroller reset and desynced? Raw UART is notoriously fragile in real-world applications.


**AutoLink** fixes this. It sits on top of the standard ESP-IDF UART drivers and transforms your serial connection into a robust, auto-negotiating, self-healing lifeline.


If the line gets noisy, AutoLink drops the link and re-sweeps. If a wire gets bumped, it automatically sweeps the baud spectrum and locks back onto the connection. It manages all the FreeRTOS background tasks, queues, and hardware interrupts automatically. You just send and receive data.


# 🏓 Quick Start: Master / Slave Ping-Pong

The classic use case: two boards bouncing **random-sized messages** back and forth, **logging throughput**, and **self-recovering** from any disruption — with almost no application code. The link's sweep/recovery is automatic; the app just gates on `State::OK`. The examples below also stash every sent payload, compare it against the slave's echo, and log a `MISMATCH` line the moment a single byte goes missing or wrong — the easiest possible smoke test for end-to-end integrity.

## The Master

```cpp
#include "AutoLink.h"
#include "AutoLinkWeb.h"
using namespace autolink;
//pin 16=rcv 17=tx
AutoLink    comm(UART_NUM_2, 16, 17, true);
AutoLinkWeb mon(comm);                          // optional WiFi monitor
uint8_t   buf[1024];
uint8_t   sent[1024];   // stash the last payload so we can compare with the echo
int       sentLen = 0;  // length of the stashed payload; 0 = nothing in flight
uint32_t  sentSeq = 0;  // sequence number of the stashed payload
bool      wasReady = false;
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
    mon.begin("YourSSID", "password", 80);  // web monitor on :80 — remove to disable
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

    // Throughput, error count, and lifetime disconnects are shown live in
    // the AutoLinkWeb dashboard — no manual stats logging needed here.
}
```

## The Slave

The slave just echoes back whatever complete messages arrive. Pass **false** for the master flag.

```cpp
#include "AutoLink.h"
#include "AutoLinkWeb.h"
using namespace autolink;
//pin 16=rcv 17=tx
AutoLink    comm(UART_NUM_2, 16, 17, false);
AutoLinkWeb mon(comm);                          // optional WiFi monitor
uint8_t   buf[1024];
bool      wasReady = false;
Log&      LOG = Log::getLog();
void setup() {
    esp_log_level_set("*",ESP_LOG_VERBOSE);
    LOG.setLevel(Log::DEBUG);
    Serial.begin(115200);
    comm.blinkWait(1, 100, 100, 2000);
    comm.begin(); // SWP, waits for master
    mon.begin("YourSSID", "password", 80);  // web monitor on :80 — remove to disable
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
