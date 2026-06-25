# 📖 AutoLink API Reference

## 📨 The Message API

| Call | Returns | Notes |
|------|---------|-------|
| `int send(const uint8_t* b, int len)` | `len` if queued, `0` if the link is down/busy | `len` must be `1..maxMsg` (default 1024). Safe to call every loop. |
| `int recv(uint8_t* b, int max)` | `>0` message length, `0` nothing ready, `-1` rejected/dropped | `max` should be `>= maxMsg`. On `-1` the bad message is drained and an error is counted. |
| `bool ready()` | `true` once negotiated | Optional — `send`/`recv` already gate themselves. |
| `void getStats(Stats& s)` | — | `tx`, `rx` (since `resetStats()`), `discCount` (OK→SWP transitions since `resetErrors()`), `frameErrs` (cumulative frame errors since `resetErrors()`). |
| `void resetStats()` | — | Zero the `tx` / `rx` throughput counters. Does **not** touch `discCount` or `frameErrs`. |
| `void resetErrors()` | — | Zero `discCount` and `frameErrs`. |
| `void getDiag(Diag& d)` | — | `txSeq`, `rxSeqSet`, `rxSeq`, `gaps`, `stale`. |

Each message goes out as a 6-byte header (`len` + `crc16`) followed by the payload, chunked into ≤250-byte COBS frames, each guarded by a per-frame CRC-8. The receiver only hands you a message once the **whole** payload arrives and its CRC-16 verifies.

> **No sizing to worry about:** set `cfg.maxMsg` to the largest message you'll send and the reassembly buffer is grown to fit automatically. Leave it alone for the 1 KB default.

## 🔌 Advanced

Most sketches never need anything below this line.

### Raw Byte Streaming

If you don't need message boundaries, `AutoLink` exposes a byte-stream API (`available` / `read` / `peek` / `write` / `flush`) shaped after Arduino's `Stream` — but it does NOT inherit from `Stream`. Set `cfg.reliableMode = false` for an unframed pass-through, or leave it on for COBS+CRC-8 framed bytes:

```cpp
const char* str = "Hello Pong!";
comm.write((const uint8_t*)str, strlen(str));

while (comm.available()) {
    uint8_t b[64];
    int len = comm.read(b, sizeof(b));
    Log::log().info("App", "Got %d bytes", len);
}
```

`write()` returns the number of bytes actually accepted (0 if the link isn't ready).

### Manual Error Control

For raw-mode applications doing their own validation, you can drive the error counter yourself. Exceeding `errThreshold` automatically issues a hardware BREAK and drops the link into a re-sweep.

```cpp
if (comm.ready() && comm.available() >= 5) {
    uint8_t p[5];
    comm.read(p, 5);
    if ((p[0]^p[1]^p[2]^p[3]) == p[4]) comm.clearErr();
    else {
        comm.err();
        Log::log().error("App", "Corrupt! count=%d", comm.getErrCount());
    }
}
```

## 🛠️ Developer Notes

- **Hardware Abstraction (Dependency Injection):** Core logic (`Link.cpp`) is decoupled from the ESP32 (`EspHal.h`) via the `IHal` interface; the LED engine via `IBlinkHal`. Keep ESP-IDF / FreeRTOS headers out of `Link.cpp` and the `Util*` classes.

- **Utility Classes:** standalone algorithms live in single-purpose, reusable classes — `UtilCrc`, `UtilCobs`, `UtilBlink`, `UtilFrameRx`. Each has a purpose comment at the top and its own `*Test.cpp` suite. `Link` consumes them and keeps only protocol logic.

- **Native PC Testing:** Because of the `IHal` abstraction, the entire stack runs on your computer. From `test/test_desktop/`, `make` builds and runs the unit suites; everything compiles `-Wall -Wextra` clean. On-hardware tests live in `test/itest/test_embedded/`.

- **State Machine:**
  - `SWP` (Sweep): Ping iterates allowed bauds sending `PING`; Pong retunes per ping and scores each baud it decodes.
  - `LCK` (Lock): Ping requests the best baud; Pong replies with the fastest scored index and both switch.
  - `OK` (Connected): raw bytes or reliable frames / messages are exchanged.

- **Reliable Mode:** User data is encapsulated in COBS frames (≤250 B payload each) with a trailing CRC-8, delimited by `0x00` sentinels. "Reliable" means *detected-and-dropped*, not retransmitted — for guaranteed delivery, layer an ack/retry on top.

- **CRC:** A precomputed 256-byte LUT gives O(1) CRC-8 for frames; messages add a CRC-16/CCITT computed over the full payload.

- **Buffer Sizing:** `MAX_CHUNK` (250) drives the static frame buffers via `static_assert`. Keep `maxMsg <= streamBufferSize`. The `AutoLink` facade auto-sizes `streamBufferSize` to `2 * (maxMsg + MSG_HDR)`.
