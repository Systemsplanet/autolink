# AutoLink API Reference

Wire-level behaviour: `docs/Protocol.md`. Dashboard: `docs/WebMonitor.md`.

## Message API

| Call | Returns | Notes |
|------|---------|-------|
| `int send(const uint8_t *b, int len)` | `len` if queued, `0` if the link is down or the window is full | `len` must be `1..cfg.maxMsg` (default 5120). Safe to call every loop. |
| `int recv(uint8_t *b, int max)` | `>0` message length, `0` nothing ready, `-1` rejected | `max` should be `>= cfg.maxMsg`. On `-1` the bad message is drained and a frame error is counted. |
| `bool ready()` | `true` once the link is in `State::OK` | Optional — `send` / `recv` gate themselves. |
| `void getStats(Stats &s)` | — | `tx`, `rx` (since `resetStats()`), `discCount` (OK→SWP transitions), `frameErrs` (cumulative). |
| `void getDiag(Diag &d)` | — | `txSeq`, `rxSeq`, `rxSeqSet`, `gaps`, `stale`, `lostMsgs`, `baudRetries`, `preferredBaud`. |
| `void resetStats()` / `resetErrors()` / `resetDiag()` | — | Throughput / disconnect+frame-error / diagnostic counters, independently. |

A message goes out as a 6-byte header (`len` u32 LE + `crc16` LE) followed by the
payload, chunked into ≤250-byte COBS frames each guarded by a CRC-8. `recv()`
returns a message only once the whole payload has arrived in order and its CRC-16
verifies. Set `cfg.maxMsg` to the largest message you will send.

`send()` returning `0` is normal backpressure: the GBN window is full, or the link
is re-sweeping. Retry on the next loop.

## Modes

`cfg.mode` selects the delivery discipline:

- **`ASYNC`** (default on ESP32) — Go-Back-N pipeline. Up to
  `AUTOLINK_ARQ_PIPELINE_WINDOW` chunks in flight; `send()` does not block.
- **`SYNC`** — one frame in flight, `send()` blocks on the ACK and runs its own
  retransmit ladder.

## Byte-stream API

`available` / `read` / `peek` / `write` / `flush`, shaped after Arduino's `Stream`
but not inheriting from it. Bytes are still COBS+CRC-8 framed and still ride the
ARQ; the only thing you give up is message boundaries.

```cpp
const char *s = "Hello Pong!";
comm.write((const uint8_t *)s, strlen(s));

uint8_t b[64];
while (comm.available()) {
    int n = comm.read(b, sizeof(b));
    Log::log().info("App", "Got %d bytes", n);
}
```

`write()` returns the bytes actually accepted (`0` if the link isn't ready).

## Manual error control

Applications doing their own validation can drive the error counter. Crossing
`cfg.errThreshold` (default 100), or `cfg.errRateWindow` errors inside one second
(default 30), issues a hardware BREAK and drops the link into a re-sweep.

```cpp
comm.err();         // count one
comm.clearErr();    // reset the rolling counter
comm.getErrCount(); // read it
```

## Developer notes

- **Hardware abstraction.** `Link` is decoupled from the ESP32 (`EspHal.h`) by
  `IHal`, and the LED engine by `IBlinkHal`. Keep ESP-IDF / FreeRTOS headers out
  of the link and the `Util*` classes — that is what lets the whole protocol run
  on the host.
- **Utilities.** `UtilCrc`, `UtilCobs`, `UtilBlink`, `UtilFrameRx` are
  single-purpose and independently tested. `Link` holds only protocol logic.
- **State machine.** Two states: `SWP` (sweeping for a baud) and `OK` (locked).
  The three-phase sweep inside `SWP` is described in `docs/Protocol.md`.
- **Buffer sizing.** `MAX_CHUNK` (250) drives the static frame buffers via
  `static_assert`. `EspHal::begin` floors `cfg.streamBufferSize` at
  `2 * (maxMsg + 6)` and the UART RX/TX buffers against the GBN window
  (`streamBufferFloor` / `uartRxBufferFloor` / `uartTxBufferFloor` in
  `AutoLinkConfig.h`); a caller-set value larger than the floor always wins. The
  facade never rewrites the config — the HAL is the one owner. Floors are then
  capped against free heap (`capFloorByHeap`).
- **CRC.** A 256-byte LUT gives O(1) CRC-8 per frame; messages add a CRC-16/CCITT
  over the whole payload.
