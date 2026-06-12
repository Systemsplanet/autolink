# 📅 AutoLink Version History

All releases, most recent first.

---

## v3.0.6

+ **Live log timestamps.** Each log entry is now stored as `HH:MM:SS I Tag message` (uptime-based, from `millis()`). Previously the format was `[I][Tag] message` with no time component.
+ **Copy button fixed.** `navigator.clipboard.writeText` requires a secure context (HTTPS). Since the monitor serves plain HTTP, the Copy button was silently failing. It now falls back to `document.execCommand('copy')` via a temporary textarea, which works on HTTP.
+ **Log DOM capped at 100 entries.** Previously 200; trimmed to match the 48-entry server-side ring more sensibly.

---

## v3.0.5

+ **All user-facing includes use angle brackets.** `test_embedded.ino` and the `docWebMonitor.md` code snippet were using `#include "AutoLink.h"` / `#include "AutoLinkWeb.h"` (quoted). Fixed to `<AutoLink.h>` / `<AutoLinkWeb.h>`. Quoted includes only resolve relative to the including file; angle brackets are required for installed Arduino library headers.
+ **`master`/`slave` removed from `test_embedded.ino` comment.**

---

## v3.0.4

+ **Explicit `util/` prefix on user-facing includes.** `Ping.ino`, `Pong.ino`, and `README.md` now use `#include <util/UtilPing.h>` / `#include <util/UtilPong.h>`. The prefix removes ambiguity if other installed libraries contain headers with the same bare filename, and makes the library layout self-documenting at the include line.

---

## v3.0.3

+ **Include fix: `"UtilPing.h"` → `<UtilPing.h>`.** User sketches must include library headers with angle brackets so the Arduino IDE resolves them through the library search path. Quoted includes only work for files in the same directory as the sketch. Fixed in `Ping.ino`, `Pong.ino`, and `README.md`.

---

## v3.0.2

+ **`UtilMain` base class.** The shared boilerplate common to `UtilPing` and `UtilPong` — hardware construction (`AutoLink`, `AutoLinkWeb`, `Log`), `setup()` body, the 1 KB scratch buffer, `wasReady_`, and the 5-second `logStats()` ticker — is now factored into `UtilMain`. Both `UtilPing` and `UtilPong` inherit from it; their files contain only the logic unique to each role. Adding `UtilMain.h` to `src/util/` keeps the change within the existing utility layer.
+ **`master`/`slave` purged from comments and docs.** All remaining references to "master" and "slave" in `UtilPing.h`, `UtilPong.h`, `UtilMain.h`, `README.md`, and `docVersion.md` are replaced with "Ping"/"Pong" or the appropriate neutral term. Protocol-layer identifiers inside `ALink.cpp` and `UtilBaudSweep` that mirror ESP-IDF naming are unchanged.
+ **`library.properties` URL set.** `url=` now points to `https://github.com/Systemsplanet/autolink`.
+ **`README.md` at repository root; zip ships as `AutoLink/`.** The top-level zip directory is now `AutoLink/` (unversioned) with `README.md` alongside it at the root, matching a standard GitHub repository layout.

---

## v3.0.1

+ **B/sec throughput logging.** Both `UtilPing` and `UtilPong` now log rates in **B/sec** instead of cumulative byte totals. The 5-second stats line format changed from `tx=N B  rx=N B  baud=N  disconnects=N  errors=N` to `tx=N B/sec  rx=N B/sec  baud=N  disc=N  errs=N`, making it immediately readable without mental arithmetic. The tag name changed from `Main` to `Ping` / `Pong` so the two nodes are distinguishable in a shared serial log.
+ **README moved to repository root.** `README.md` now lives at the root of the repository rather than inside the `AutoLink/` library directory, matching the standard GitHub layout. The document index updated to relative links so all four docs are one click away from the rendered README.

---

## v3.0.0

Major release: project restructured into a standard Arduino library layout and several long-standing behavior questions resolved.

+ **Library version logged at startup.** `begin()` now logs `AutoLink v3.0.0 starting (master|slave)` at INFO level, so the running firmware version is always visible in the serial monitor. The version is defined once as `AUTOLINK_VERSION` in `AutoLink.h` (the public facade header).
+ **Directory restructure.** Sources moved to `src/` (core) and `src/util/` (utility classes). Runnable sketches moved to `examples/PingPong/` as `Ping.ino` and `Pong.ino` (formerly the README Ping/Pong snippets). Tests split into `test/test_desktop/` (host, pure logic) and `test/test_embedded/` (on-hardware). The `extract_readme.py` compile-check tool was removed — the examples are now real `.ino` files.
+ **Master/Slave renamed to Ping/Pong** throughout the examples. The sketch-helper headers were renamed `UtilPing.h` and `UtilPong.h` (classes `UtilPing` / `UtilPong`) and moved into `src/util/` alongside the other utility classes.
+ **Pipelined throughput.** `UtilPing` previously sent one message and blocked waiting for its echo before sending the next — a strict round-trip that left each direction idle half the time, capping throughput near 5 kB/s on a 115200 link. It now keeps a window of up to 8 messages in flight and verifies echoes in FIFO order by length + CRC-16, keeping both directions busy and roughly doubling effective throughput.
+ **Lifetime error counter.** Added `getLifetimeErrors()` — a cumulative count of every frame error (bad CRC, malformed COBS, overflow) that, unlike the internal rolling counter, only ever increases. The web dashboard's "Errors" card now shows this lifetime tally (it previously showed the rolling counter, which resets to 0 after every good frame and so almost never appeared to move).
+ **5-second stats log on both nodes.** The throughput log line now includes lifetime errors (`tx=… rx=… baud=… disconnects=… errors=…`) and is emitted by *both* Ping and Pong (previously Ping only). Note: tx and rx totals track app-payload bytes and are equal over time; a small instantaneous gap just reflects messages in flight at the sampling instant.
+ **Web dashboard Reboot button.** A new **⏏ Reboot** control restarts the device via a `POST /reboot` endpoint (`esp_restart()`) after a confirmation prompt. The page polls until the device answers again and reloads automatically.
+ **Web Monitor docs moved** out of the README into `docWebMonitor.md`, with a new "Errors vs. Disconnects" explanation. The README gained a Web Monitor blurb under Features Under the Hood and an updated document index.

---

## v2.12.2

+ **RX pin pull-up — the real fix for the noise/hang problem.** `EspHal::begin()` now enables `GPIO_PULLUP_ONLY` on the RX pin. UART idle is logic HIGH; a pulled-up unconnected pin sits at a stable mark state and generates zero bytes, instead of floating and producing a continuous noise stream (~11 kB/s at 115200, ~375 kB/s at 3 MHz) that floods the event queue and can hang the system. A driven peer TX easily overrides the ~47 kΩ pull-up.
+ **Default baud list reverted to the conservative five** (`115200, 57600, 38400, 19200, 9600`). The high-speed bauds (≥230400) caused a floating-pin noise storm at boot on jumper-wired setups; they remain available by setting `cfg.allowedBauds` explicitly.

---

## v2.12.1

+ **Raw-bytes-per-window diagnostic in slave sweep log.** The "scored 0/2, advancing" log line now includes the count of raw bytes received in that window: `SWP slave baud[6]=115200 scored 0/2 (0 raw bytes rx), advancing`. A count of 0 means the master's TX signal is not reaching the slave's RX pin at all — wiring issue. A non-zero count means the wire is live but the baud rate is wrong for the current window.
+ **WIRING CHECK error message corrected.** GPIO16/17 *are* on the FireBeetle header (GPIO17=D10, GPIO16=D11); the message now gives the correct crossover wiring.
+ **EspHal UART init now logs success and checks all three init calls.** `uart_param_config` and `uart_set_pin` were previously silent on failure. Both now check the return value and log a specific error including the UART and pin numbers. On success: `[I][EspHal] UART2 ready: tx=GPIO17 rx=GPIO16`.

---


## v2.12.0

+ **`UtilPing.h` and `UtilPong.h` — zero-boilerplate sketch helpers.** The full ping-pong echo test (send, compare, echo, stats log, reconnect) is now a single-object API. The README Ping sketch went from ~100 lines to 3. Constructor takes debug baud, UART number, RX pin, TX pin, and optional WiFi credentials; `setup()` and `loop()` do everything else. The underlying `AutoLink` and `AutoLinkWeb` members are constructed in-place — no heap allocation, no pointers. Web monitor is opt-in: omit the SSID argument (or pass `nullptr`) and the server never starts, but the UART link runs exactly as before.

---

## v2.11.4

+ **Slave sweep timer — critical fix for fast baud lists.** The slave previously relied entirely on receiving decodable PINGs to advance its baud index. When the physical wiring can't handle the new high-speed bauds (3 MHz, 2 MHz, etc.), the slave got zero decodable PINGs and stayed stuck at index 0 while the master swept ahead. By the time the master reached a working baud like 115,200, the slave was still listening at 3,000,000 — so they never synchronized. Fix: the slave now runs a baud-window timer (`pingSamplesPerBaud × delayMs`). If a window produces no reliable PINGs, the slave advances to the next baud automatically, staying in lockstep with the master.
+ **Sweep diagnostic logging.** `begin()` logs the baud range, sample count, and fastAck state. The master logs `SWP master baud[N]=X` once at the start of each baud window. The slave logs `SWP slave testing baud[N]=X` each time it starts a new baud (on startup, after timer-advance, and after fast-PING advance), `SWP slave: first PING at baud[N]=X` on the first decoded PING, and `SWP slave baud[N]=X scored Y/Z, advancing` when the timer fires with insufficient PINGs. Together these make it immediately obvious whether the two sides are in sync and at which baud they lock.
+ **Web monitor hang during reconnects fixed.** Three root causes addressed: (1) `fetch()` calls now use an `AbortController` with a 2.5 s timeout — stalled requests abort cleanly instead of holding open connections; (2) a `busy` flag prevents `setInterval` from stacking concurrent `poll()` calls when the ESP is slow to respond; (3) all four HTTP handlers now send `Connection: close`, forcing the browser to use a fresh TCP connection per request and preventing stale keep-alive sockets from filling the server's `max_open_sockets` limit after a WiFi reconnect. The `/logs` chunked handler also bails out immediately on a send error (client disconnected mid-response) rather than hanging.

---

## v2.11.3

+ **`getCurrentBaud()` fixed — was returning 0.** Root cause: when the sweep exhausts all bauds and transitions to LCK, `spdI` was left at `allowedBauds.size()` (one past the end), causing `getCurrentBaud()` to return 0. Additionally, none of the five lock-transition paths in `onRx` updated `spdI` to reflect the actual locked index. Both are fixed: `spdI = 0` on LCK entry; `spdI = <locked_index>` in all five SWP/LCK lock paths.
+ **Baud rate and dashboard display now work correctly** as a result of the above fix.
+ **Faster default baud list.** `allowedBauds` now leads with `{3000000, 2000000, 1000000, 921600, 460800, 230400}` before the original `{115200, 57600, 38400, 19200, 9600}`. The ESP32 APB clock is 80 MHz; 3 MHz divides to 2.963 MHz (1.2% low, within the ±2.5% UART tolerance). A full 11-baud sweep at 4 samples × 50 ms takes ~2.2 s.
+ **Cumulative stats — `resetStats()` removed from Ping sketch.** The 5-second serial log now shows running totals instead of resetting them every interval.
+ **Copy button on log panel.** A `Copy` button next to Clear copies the entire visible log to the clipboard via `navigator.clipboard`.

---

## v2.11.2

+ **Light theme.** Dashboard now uses a white/light-gray background with high-contrast dark text, larger fonts, and stronger card borders. The dark theme was difficult to read in bright light.
+ **Baud rate on dashboard.** The WiFi/RSSI card now shows a `baud` hint that displays the current locked UART baud rate. While the master is still sweeping, it shows `sweeping…` instead of a number.
+ **Disconnect counter label clarified.** The hint under the error count now reads `N disconnects` (formerly `lifetime N`). A disconnect only counts when the link drops from OK→SWP — not during baud negotiation. If the slave is reset before the link ever reaches OK, the counter stays at 0; this is correct behavior.
+ **Stats logging restored in Ping sketch.** The `loop()` example now logs TX/RX bytes, current baud, and lifetime disconnects to serial every 5 seconds via `LOG.info()`. The web dashboard and serial output work simultaneously.
+ **Pause button symbols fixed.** `textContent` does not decode HTML entities, so `&#9646;` appeared as literal text. Changed to `innerHTML`.

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
