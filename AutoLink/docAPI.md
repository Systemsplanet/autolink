# 📖 AutoLink API Reference

---

# 📨 The Message API

This is the complete public surface for normal use:

| Call | Returns | Notes |
|------|---------|-------|
| `int send(const uint8_t* b, int len)` | `len` if queued, `0` if the link is down/busy | `len` must be `1..maxMsg` (default 1024). Safe to call every loop. |
| `int recv(uint8_t* b, int max)` | `>0` message length, `0` nothing ready, `-1` rejected/dropped | `max` should be `>= maxMsg`. On `-1` the bad message is drained and an error is counted. |
| `bool ready()` | `true` once negotiated | Optional — `send`/`recv` already gate themselves, so you rarely need this. |
| `void getStats(uint64_t& tx, uint64_t& rx)` | — | App-stream bytes since the last `resetStats()`. |
| `void getStats(uint64_t& tx, uint64_t& rx, uint64_t& errs)` | — | Adds the lifetime disconnect count (one per OK→SWP transition). Monotonic across samples and link drops; only zeroed by `resetErrors()`. |
| `void resetStats()` | — | Zero the tx/rx throughput counters. Does **not** touch the disconnect counter. |
| `void resetErrors()` | — | Zero the lifetime disconnect counter (e.g. on operator ack). |

Each message goes out as a 6-byte header (`len` + `crc16`) followed by the payload, chunked into ≤250-byte COBS frames, each guarded by a per-frame CRC-8. The receiver only hands you a message once the **whole** payload arrives and its CRC-16 verifies — so you never see a half-message or a corrupted one.

> **No sizing to worry about:** set `cfg.maxMsg` to the largest message you'll send and the reassembly buffer is grown to fit automatically. Leave it alone for the 1 KB default.

---

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
        comm.err();                                            // bad data; after errThreshold -> re-sweep
        Log::getLog().error("App", "Corrupt! count=%d", comm.getErrCount());
    }
}
```

---

# 🛠️ Developer Notes

+ **Hardware Abstraction (Dependency Injection):** Core logic (`ALink.cpp`) is decoupled from the ESP32 (`EspHal.h`) via the `ILink` interface; the LED engine is likewise decoupled via `IBlinkHal`. Keep ESP-IDF / FreeRTOS headers out of `ALink.cpp` and the `Util*` classes.

+ **Utility Classes:** standalone algorithms live in single-purpose, reusable classes — `UtilCrc` (checksums + LUTs), `UtilCobs` (codec), `UtilBlink` (LED patterns), `UtilFrameRx` (reliable-frame accumulator). Each has a purpose comment at the top and its own `*Test.cpp` suite. `ALink` consumes them and keeps only protocol logic.

+ **Native PC Testing:** Because of that abstraction, the entire stack runs on your computer. From `test/test_desktop/`, `make` builds and runs five suites: `UtilCrcTest`, `UtilCobsTest`, `UtilBlinkTest`, `UtilFrameRxTest`, and the protocol/integration tests in `test.cpp`. Everything compiles `-Wall -Wextra` clean. On-hardware tests live in `test/test_embedded/`.

+ **State Machine:**
  + `SWP` (Sweep): master iterates allowed bauds sending `PING`; the slave retunes per ping and scores each baud it decodes.
  + `LCK` (Lock): master requests the best baud; the slave replies with the fastest scored index and both switch.
  + `OK` (Connected): raw bytes or reliable frames / messages are exchanged.

+ **Reliable Mode:** User data is encapsulated in COBS frames (≤250 B payload each) with a trailing CRC-8, delimited by `0x00` sentinels, so the receiver can discard a corrupt frame without losing stream sync. Note that "reliable" means *detected-and-dropped*, not retransmitted — for guaranteed delivery, layer an ack/retry on top (the ping-pong echo pattern in the README is one approach).

+ **CRC:** A precomputed 256-byte LUT gives O(1) CRC-8 for frames; messages add a CRC-16/CCITT computed over the full payload.

+ **Buffer Sizing:** `MAX_CHUNK` (250) drives the static frame buffers via `static_assert`. Keep `maxMsg <= streamBufferSize`. The `AutoLink` facade auto-sizes `streamBufferSize` to `2 * (maxMsg + MSG_HDR)` so you normally only need to set `maxMsg`.
