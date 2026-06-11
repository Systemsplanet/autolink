# 📅 AutoLink Version History

All releases, most recent first.

---

## v2.11.1

+ **Dashboard Reset button.** A `↺ Reset` button in the web monitor calls `resetStats()` and `resetErrors()` on the AutoLink instance via a new `POST /reset` endpoint. The button shows `✓ Done` on success or `✗ Err` on failure, then reverts after 1.2 s. The sampler's internal B/s baseline is also zeroed so the first reading after a reset is 0 rather than a momentary spike.

---

## v2.11.0

+ **`AutoLinkWeb` — optional WiFi web monitor.** Include `AutoLinkWeb.h`, construct `AutoLinkWeb mon(comm)`, call `mon.begin("SSID", "password")` (or `mon.begin("SSID", "password", 80)` for a custom port) in `setup()`. A mobile-friendly dashboard is served — by default on port **8765** — via the ESP's built-in `esp_http_server`. Shows TX/RX B/s, cumulative totals, error count toward threshold, lifetime disconnects, link-state pill, RSSI, free heap, uptime, and a live scrolling log panel. Updates at 1 Hz via `fetch()` polling; **Pause/Resume** and **Clear** controls in the UI. WiFi failure leaves the UART core completely unaffected and `isUp()` returns `false`.
+ **`Log::setSink(fn, ctx)` / `clearSink()`.** Registers a callback fired after every `emit()` — the mechanism `AutoLinkWeb` uses to populate the log panel. Callback runs in the emitting task; must be fast and non-blocking.

---

## v2.10.0

+ **Top-down sweep + fast-ack.** The master now sweeps from the top baud down (instead of bottom-up), so the first PINGs the slave sees are at the preferred speed, not the slowest fallback. When the slave has scored enough PINGs to trust the current baud it sends a one-byte fast-ack; the master jumps straight to OK with no REQ round-trip. On a healthy top-baud link, lock time drops from ~25 timer ticks (full sweep + REQ round-trip) to 4 ticks. This is the "if the top baud passes, use it immediately" path. Old 2.x nodes on the other end still work via the legacy REQ path. New `cfg.fastBaudLock` knob (default `true`) to disable if needed.

---

## v2.9.0

+ **Reliability-based auto-baud sweep.** New `UtilBaudSweep` value class (with 12-test unit suite) replaces the old "first PING wins" score logic. The slave now averages over `cfg.pingSamplesPerBaud` PINGs per baud (default 4) and requires `cfg.minAcceptRate` (default 0.5) decode rate before trusting a baud. A flaky top baud falls back to a reliable lower one instead of locking in. New `Locked at N baud` log on every LCK→OK transition for visibility. The pick is conservative: a working slow link beats a flaky fast one.
+ **New knob: `cfg.pingSamplesPerBaud` (default 4).** Set to `1` to restore the old "first one wins" behavior. Each additional sample costs one sweep tick (`cfg.delayMs`), so a 5-baud list with 4 samples takes 20 ticks instead of 5.
+ **New knob: `cfg.minAcceptRate` (default 0.5).** Fraction of PINGs that must decode at a baud for it to be considered reliable. `0.8` is strict; `0.1` is lenient; `0` means any single success counts.

---

## v2.8.0

+ **Lifetime disconnect counter:** `getStats()` now has a 3-arg form `getStats(tx, rx, errs)` that returns a lifetime count of OK→SWP transitions. One count per disconnect regardless of how noisy the recovery is: a slave reset that emits a burst of BREAKs while rebooting is still one event. Counter is monotonic across link drops; `resetStats()` zeros only tx/rx. The new `resetErrors()` zeros the disconnect counter explicitly (e.g. on operator ack). Designed for longevity testing: "how many bounces has this link survived?"

---

## v2.7.0

+ **Refactor to utility classes:** `UtilCrc`, `UtilCobs`, `UtilBlink` (+ `IBlinkHal`/`EspBlinkHal`), and `UtilFrameRx` extracted from `ALink.cpp` / `AutoLink.h`. `ALink` implements `UtilFrameRx::Listener`; `AutoLink::blinkWait` is a two-line forward into `UtilBlink`. `ALink.cpp` shrank by ~200 lines and now reads as pure protocol. Public API unchanged.
+ **Per-class unit suites:** `UtilCrcTest`, `UtilCobsTest`, `UtilBlinkTest`, `UtilFrameRxTest` (24 new tests: known-answer CRC vectors, single-bit error detection, COBS 0xFF group boundaries and malformed-input rejection, exact LED on/off/timer sequences, frame splitting/desync/drop semantics). `make test` runs all five binaries; build is `-Wall -Wextra` clean.
+ **🐛 CRC-16 LUT fix:** four corrupt entries (indices 76–79) in the CCITT-FALSE table, present since the LUT was introduced, found by the new known-answer test (`"123456789"` → `0x29B1`) and regenerated.

  > **Wire-compat note:** a v2.7 node exchanging *messages* with a ≤v2.6 node will see CRC-16 rejects (`recv()` → `-1`) on payloads whose checksum touches those entries. Upgrade both ends together.

---

## v2.6.0

+ **Async `blinkWait`:** with the default `delayMs == 0` the call is non-blocking — the flash pattern runs on an `esp_timer` and a new call replaces a running one. `delayMs > 0` keeps the original blocking flash + pause. Per-packet LED feedback no longer throttles throughput.
+ **OK-state idle watchdog fixed:** entering `OK` now arms a periodic timer (`idleTimeoutMs / 3`). In v2.5 the watchdog only ever received one stray LCK tick after entering `OK` and then went silent — a dead slave was never actually detected.
+ **Keepalive:** while in `OK`, a TX-quiet side emits a lone `0x00` each tick; the peer's parser skips it as an inter-frame zero but its watchdog counts it as RX. Healthy-but-quiet links no longer bounce every `idleTimeoutMs`. Reliable mode only — a `0x00` in raw mode would reach the app as data.

  > **Note (raw mode):** if your raw-mode sketch can be TX-quiet longer than `idleTimeoutMs`, raise `idleTimeoutMs` or set it to `0` to disable the watchdog.

+ **App-buffer overflow detected:** a full stream buffer used to drop bytes silently and desync the message stream. `ILink::pushAppBuf()` now returns bytes accepted; a shortfall counts toward the error threshold so the link drops and resyncs instead of corrupting quietly.
+ **Parser yields after a threshold trip:** when the error threshold fires mid-event, the rest of that UART event is routed to the command parser instead of discarded — a re-sweep PING arriving in the same burst is no longer lost.
+ **Locking hardened:** `dropLink` holds the mutex throughout; `onRx` retune paths are fully locked.
+ **UART task stack safety:** the RX scratch buffer moved from `alloca` to a one-time heap allocation, eliminating a potential stack overflow with large `rxBufferSize` configs.
+ **Tooling:** `extract_readme.py` embeds its Arduino/ESP-IDF stubs (temp dir at run time; no `host_stubs/` directory needed); its block finder now checks every README example.

---

## v2.5.0

+ **Asymmetric peer-death recovery:** slave's `err_unlocked()` now performs the same local state reset (`SWP` + retune to `allowedBauds[0]` + score/buffer clear) that `onBreak()` does. Previously a side that tripped its own error counter would send a BREAK but leave its UART tuned to the locked baud — the peer's re-sweep was then inaudible. Now both sides re-arm symmetrically.
+ **Idle-channel watchdog:** new `cfg.idleTimeoutMs` (default 3000 ms, set 0 to disable). In `OK`, if no RX bytes arrive for the window, the link is dropped locally and a BREAK is sent to the peer. Closes the "master is the originator of traffic; slave dies silently" deadlock where the master's RX pin goes silent and it never notices.
+ **LCK timeout:** master tracks `lckRetries` and restarts the sweep if more than `2 * allowedBauds.size()` REQ ticks pass with no slave reply. Prevents the master from getting stuck in `LCK` forever when the peer is gone.
+ **BREAK debounce:** spurious `UART_BREAK` events closer than 50 ms apart are collapsed; the FIFO is still drained so a noise burst can't leave stale bytes in the parser.
+ **FIFO flush on retune:** `EspHal::setSpd()` now calls `uart_flush_input()` before `uart_set_baudrate()`. The first bytes after a baud change are no longer samples from the old baud, so a clean re-negotiation doesn't accumulate false-positive errors.

---

## v2.4.0

+ **Thread safety:** `recvMsg()` and the reliable-mode `onRx` path now hold the protocol mutex for the full reassembly / frame-parse sequence, eliminating a data race between the UART task and the app loop. `err_unlocked()` was added so the parser can count errors without re-entering the lock.
+ **Reliable RX parser hardening:** bad CRC, malformed COBS, and buffer overflow no longer `break` out of the event. They now call `err_unlocked()` and keep scanning, so back-to-back frames in one event are all processed.
+ **COBS / CRC speed:** inner loops replaced with `memcpy` and a 256-entry constexpr LUT respectively, ~5–10× faster on ESP32 at high baud.
+ **Write/sendMsg locking:** a single mutex acquisition per frame in `write()`; `sendMsg()` holds the lock across header + payload so the link can't drop between them.
+ **UART event task:** `std::vector<uint8_t>` replaced with an `alloca`-backed scratch buffer to avoid per-iteration heap churn.
+ **`blink()` renamed `blinkWait()`** to make the blocking behavior obvious at the call site. No behavior change.

---

## v2.3.0

+ **Status LED:** added `blink(n, onMs, offMs, delayMs)` to flash the onboard blue LED for at-a-glance link state, with an optional trailing `delayMs` pause to pace a loop. Configurable pin via `cfg.ledPin` (default GPIO 2). *(Renamed to `blinkWait()` in v2.4.0; behavior unchanged.)*

---

## v2.2.0

+ **One-object API:** construct `AutoLink` as a global (no `new` / pointer), call `begin()`, then `send()` / `recv()`. Added `ready()` so `State` never has to appear in user code.
+ **Reliable by default:** `reliableMode` now defaults on, so the message API is protected out of the box.
+ **Auto-sized buffers:** the reassembly buffer grows from `maxMsg` automatically; the old `maxMsg <= streamBufferSize` rule (and its construction-time error, which the *default* config used to trip) is gone.
+ **Saner defaults:** `maxMsg` defaults to 1 KB instead of 8 KB, so a default-constructed link no longer over-allocates.
+ Raw `Stream` byte API, explicit `sendMsg`/`recvMsg`, and manual error control retained under the advanced API — no behavior change for existing sketches.

---

## v2.1.1

+ **Auto-baud fix (critical):** the slave now retunes its baud in lockstep with the master during the sweep, so scoring works on real hardware and the link selects the fastest working baud instead of always collapsing to `allowedBauds[0]`.
+ **Message API:** added `sendMsg()` / `recvMsg()` — boundary-preserving, length-framed, CRC-16-protected transfer of arbitrary payloads.
+ **Throughput counters:** `getStats()` / `resetStats()` expose TX/RX byte totals for one-call B/s logging.
+ **Write feedback:** `write()` now returns bytes actually accepted (0 when the link is down) instead of silently claiming success.
+ **Throughput ceiling removed:** dropped the artificial 1 ms per-chunk delay in the reliable writer; throughput now tracks the real baud rate.
+ **Memory safety:** per-chunk heap allocations in the writer replaced with static scratch buffers; `static_assert` added coupling `MAX_CHUNK` to the frame buffers.

---

## v2.0.0

+ Inherited `AutoLink` from the Arduino `Stream` class for native `.println()` / `.parseInt()` compatibility.
+ Added opt-in `reliableMode` (COBS + CRC-8 framing).
+ Replaced bitwise CRC loops with an O(1) 256-byte LUT.
+ Concurrency fixes: bounded `uart_event_task` waits for clean teardown; TX mutex held across full transmissions.
+ Memory safety: `std::make_unique` allocations; patched init-failure leaks.
+ Consolidated the constructor into an `AutoLinkConfig` struct.

---

## v1.0.0

+ Initial master/slave auto-baud negotiation.
+ Basic FreeRTOS stream buffer and hardware interrupt integration.
