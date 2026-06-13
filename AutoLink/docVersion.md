# 📅 AutoLink Version History

All releases, most recent first.

---

## v3.2.2

+ **`docTests.md`.** New document covering how to build, run, and extend the host test suite: the 13 binaries (one per class / functionality area), the AddressSanitizer + UBSan integration mode (`make test_asan`, the valgrind equivalent in gcc-native form), the gcov line + branch coverage mode (`make coverage`, output in `coverage/coverage.txt`), CI recipes, the embedded self-loopback test, and a step-by-step "adding a test" guide. Linked from the README document index. The doc is the single source of truth for "how do I run the tests" — answers that previously lived only in scattered Makefile comments.

---

## v3.2.1

+ **Keepalive is now a proper empty frame (`0x00 0x00`).** v3.2.0 emitted a lone `0x00` as a keepalive. After any drop / resync the sender and receiver disagree on frame boundaries for at least one heartbeat, so the lone zero would flush a partial COBS group and trip `onFrameError()` on every tick — exactly the noise the keepalive was supposed to prevent. Replaced with the two-byte sequence `0x00 0x00`, which is unambiguously an empty frame on the wire (COBS bytes are `0x01..0xFF`, so a real frame never produces two zeros back-to-back). `UtilFrameRx::feed()` recognises the pair as a self-resyncing no-op: at a clean boundary both zeros are skipped with no callback; mid-COBS the first zero closes the partial (one `onFrameError()`) and the second is the keepalive start. Backwards compatible: single stray `0x00`s still skip silently.
+ **Host tests split one-per-class, with integration build modes.** The 26 tests in `test_desktop/test.cpp` moved to dedicated files: `MockHalTest.cpp` (9 tests for the host ILink mock), and the ALink tests split by functionality into `ALinkIOTest` (4), `ALinkMessageTest` (4), `ALinkNegotiationTest` (3), `ALinkErrorTest` (7), `ALinkWatchdogTest` (6). Shared helpers — `MockHal` class, `pipe_data`, `negotiate_to_ok` — moved to `MockHal.h`. The Makefile gained two integration build modes: `make test_asan` (AddressSanitizer + UBSan, the valgrind-equivalent in gcc-native form) and `make coverage` (gcov line + branch coverage, written to `coverage/coverage.txt`). The coverage tool uses per-source canonical `.gcno`/`.gcda` pairs (one binary chosen per source for the most-comprehensive view) because `gcov-tool merge` is fragile and often produces empty outputs.
+ **`AutoLink` facade host tests.** New `AutoLinkTest` covers the public API surface that doesn't need WiFi / ESP-IDF: `send/recv/ready`, `dropLink`, the `Stream` byte interface, `getStats/resetStats/resetErrors`, `err/clearErr/getErrCount`, `getState/getCurrentBaud/getLifetimeErrors`, `sendMsg/recvMsg`, `isHealthy`, and `blinkWait` paths. `AutoLinkWeb` and `EspHal` remain Arduino-only (WiFi, esp_http_server) but every other class is now host-tested.
+ **Comment audit.** All new and modified comments kept terse; verbose design notes trimmed to the minimum needed to explain non-obvious decisions.

---

## v3.2.0

+ **Root-cause fix for the connect/disconnect thrash.** `errs` (the error-threshold counter) was effectively a *lifetime* counter in the OK state: `onPayload` pushed good frames but never cleared it, while `onFrameError` kept incrementing. A link could move dozens of messages perfectly and still get dropped the moment the Nth scattered CRC reject (ordinary RF noise) pushed the lifetime total past `errThreshold`. Each drop sent a BREAK, the peer re-swept and BREAKed back, and the two nodes thrashed in SWP indefinitely — the logs showed 16 clean echoes immediately followed by `Error threshold exceeded (6 > 5)`. Now a successfully received frame resets `errs` to 0 in both reliable (`onPayload`) and raw (`pushAppBuf`) paths, so the threshold counts *consecutive* failures. Scattered noise is tolerated; only a genuine run of back-to-back errors (a truly broken line) drops the link.
+ **Regression test added** (`Scattered Errors Don't Drop a Working Link`): interleaves 20 corrupt frames with good traffic and asserts the link stays in OK, then sends `errThreshold+1` corrupt frames back-to-back and asserts it drops. Verified to fail against the old lifetime-counter behaviour.

---

## v3.1.8

+ **`UtilPong`: rate-limit "not ready" log.** The `not ready` line was logged every loop iteration (~every 3–4 ms) with no throttle, producing hundreds of entries per second and burying all meaningful log output. Now rate-limited to 1/sec using `tNotReady_`, matching the behaviour in `UtilPing`.
+ **`UtilPong`: drain stale RX bytes on link-up.** When Pong transitions from SWP to OK, the UART RX buffer contains residual bytes from the baud sweep (partial PING frames at the wrong baud, garbage, etc.). These cause the COBS frame parser to start mid-frame and reject every valid PING that follows, producing `0/2` scores despite Ping sending continuously. Fixed by draining the buffer in a tight recv loop immediately on link-up before entering the echo loop.

---

## v3.1.7

+ **Compile fix.** `tNotReady_` was used in `UtilPing` but only declared in `UtilMain` — which uses it for a different purpose in a different source tree. Added `tNotReady_` directly as a private member of `UtilPing`.

---

## v3.1.6

+ **SWP stall watchdog in `UtilPing`.** After an error-threshold link drop, the FreeRTOS timer service queue can overflow (the UART task at priority 5 queues faster than the timer task at priority 1 can drain), causing `xTimerStart` to silently fail. The sweep sends one PING at baud[0] then hangs for minutes. Fixed: `UtilPing` now tracks time spent in `!ready()` state. If no sweep progress occurs for 5 s (`SWEEP_STALL_MS`), `comm_.dropLink()` is called to send a BREAK and restart the sweep from the application layer, bypassing the stalled timer. The log shows `E Ping SWP stall — forcing BREAK` when triggered.
+ **`AutoLink::dropLink()` / `ALink::dropLink()` added.** Public method that acquires the lock, calls `dropLink_unlocked()`, then sends a BREAK to wake the peer. Safe to call from `loop()`.
+ **Log DOM limit increased to 500 KB** (from 10 KB). Trim target is 400 KB, keeping ~15,000+ lines of history. Fill bar scale updated to match (100% = 500 KB).

---

## v3.1.5

+ **Log fix.** v3.1.4 introduced two undeclared JS variables (`logPaused`, `logFullOpen`) that caused a `ReferenceError` on page load, killing the entire script and breaking the log. The `var` declaration line was never updated from `var paused=false,...` to include the new variables, and `togglePause` still referenced the old `paused` name. Fixed.

---

## v3.1.4

+ **Log fill bar.** A 5px green progress strip sits below the log panel showing how full it is relative to the 10 KB trim threshold. Fills left-to-right from green to grey.
+ **Maximize / minimize log.** A ⛶ button next to Pause opens the log fullscreen as an overlay with its own toolbar (Clear, Copy, Pause, Close). The fullscreen panel mirrors the inline log and stays in sync. Close (✕) returns to normal view.
+ **Reboot clears log.** The reboot button calls `clearLog()` and resets `lastSeq=0` so the panel shows only lines from the new boot session.
+ **Version in footer.** `vX.Y.Z` is served in the `/stats` JSON and shown in the footer, updated each poll.
+ **Pause pauses log only.** Stats, gauges, uptime, RSSI, and heap keep updating. `lastSeq` advances during pause so resume shows only new lines.
+ **Baud shows sweep state.** `SWP` → `115200 ⇄ sweeping`; `LCK` → `115200 ⇄ locking`; `OK` → `115200 baud`.

---

## v3.1.3

+ **Log truncation fixed.** The web log DOM was trimmed at 100 entries regardless of size. Changed to size-based trimming: the log is only trimmed when it exceeds 10 KB of text content, preserving full session history for analysis. The server-side ring was also increased from 48 to 200 entries so recently connected clients see more history.

---

## v3.1.2

+ **Reboot clears log.** The reboot button now calls `clearLog()` and resets `lastSeq=0` before sending the reboot command, so the log panel shows only lines from the new boot session.
+ **Version shown in web GUI footer.** Version is added to the `/stats` JSON and displayed as `vX.Y.Z` in the footer. Updates on each poll.
+ **Pause only pauses log.** Stats, gauges, uptime, RSSI, and heap continue updating. `lastSeq` advances during pause so resume shows only new lines.
+ **Baud shows sweep/locking state.** `SWP` → `115200 ⇄ sweeping`; `LCK` → `115200 ⇄ locking`; `OK` → `115200 baud`.

---

## v3.0.12

+ **Debug logging throughout `UtilPing`, `UtilPong`, `UtilMain`.** Key events now logged at `DEBUG` level:
  + `UtilMain`: role, baud, WiFi SSID (or "disabled"), and "setupCommon complete" bracket the startup sequence.
  + `UtilPing`: link-up (with baud) and link-lost (with `pendCount` and `seq`) on transitions; per-loop send count and `pendCount` after each fill; stall detection log when pipeline is full ≥3 s with no echoes (including a FIFO reset); `send failed` on `comm_.send()` returning false.
  + `UtilPong`: link-up (with baud) and link-lost (with lifetime echo count) on transitions; per-echo log with sequence number and byte count; CRC-reject log; per-loop processed message count.
+ **Pipeline stall recovery in `UtilPing`.** If `pendCount == WINDOW` for ≥3 s with no echoes draining the pipeline (e.g. because Pong rebooted and Ping's link stayed in OK), `pendHead_/pendTail_/pendCount_` are reset to 0 and an `[E]` log is emitted. This prevents Ping from freezing silently when Pong drops mid-session.

---

## v3.0.11

+ **`master`/`slave` removed from all runtime log output, current docs, and tests.** The sweep/lock log lines now read `SWP Ping baud[...]`, `SWP Pong testing baud[...]`, `SWP Pong: full sweep done`, etc., and the role label logged at startup is `Ping`/`Pong`. The WIRING CHECK message now says "The Ping node's TX is not reaching this RX pin. Required: Ping TX -> Pong RX AND Pong TX -> Ping RX". Comments throughout `ALink.cpp`, `ALink.h`, `UtilBaudSweep.{h,cpp}`, the `docAPI.md` state-machine description, and the desktop test suite were updated to Ping/Pong as well. The C++ identifier `isMaster`/`isMasterNode` (the role bool in the `AutoLink`/`ALink` constructors) is unchanged to preserve API compatibility — it is never shown to users. Historical `docVersion.md` entries are left as-is as an accurate record. Desktop test suite re-run green after the rename.

---

## v3.0.10

Diagnostic release to confirm two separate root causes for the ping/pong connection failures seen since v3.0.0.

+ **Core/priority diagnostic in `uart_event_task`.** The task now logs `uart_event_task running on core N, priority P` on startup. This confirms the v3.0.9 fix: before pinning, the task ran with no affinity at priority 12 and could land on core 0, starving its idle task and tripping the Task Watchdog at ~20 s (the consistent `rst:0x1` hard reset). After the fix this should print `core 1, priority 5`.
+ **FIFO desync diagnostic in `UtilPing`.** The `recv rejected` and `MISMATCH` log lines now include `pendCount`, `head`, and `tail`. This exposes the second, independent bug: the pipelined echo-compare FIFO (added in v3.0.0) drops exactly one pending entry per CRC reject, but a reject does not map cleanly to one echo. The FIFO drifts out of alignment with the real echo stream, so every subsequent good echo compares against the wrong entry and counts as a fresh error — a cascade of 6 errors that trips the link-drop threshold. A burst of rejects/mismatches with a shrinking `pendCount` is the desync signature.

### Why ping/pong has not connected since v3.0.0

Two regressions landed together in v3.0.0 and compound:

1. **Pipelined echo verification (`UtilPing`).** Before v3.0.0, Ping sent one message and waited for its echo — a strict round-trip with no possibility of desync. v3.0.0 introduced an 8-deep in-flight window verified in FIFO order. A single link glitch desyncs the FIFO and cascades into a forced link drop; stale echoes queued on Pong then re-desync the fresh FIFO after every reconnect, so it never stabilises.
2. **Unpinned high-priority UART task (`EspHal`).** The priority-12, no-affinity UART task periodically starved core 0's idle task and hard-reset the board (~20 s), injecting UART garbage on reboot that manifested as the CRC rejects feeding bug #1.

v3.0.9 addressed #2 (pinning + priority). The FIFO desync (#1) is diagnosed here and should be fixed by reverting `UtilPing` to a strict round-trip or making the FIFO resync-safe — see next release.

---

## v3.0.9

+ **UART task: pinned to core 1, priority 12 → 5.** `uart_event_task` was created with `xTaskCreate` (no core affinity) at priority 12. On a dual-core ESP32, a priority-12 task with no affinity can land on core 0 and starve its idle task, tripping the Task Watchdog at ~20 seconds — exactly the hard reset seen in testing. Fixed by switching to `xTaskCreatePinnedToCore(..., 1)` so the task always runs on the same core as Arduino's `loop()`, leaving core 0's idle task free. Priority reduced to 5: well above `loop()` (1) and WiFi/BT tasks (3-4), but no longer able to starve system housekeeping.

---

## v3.0.8

+ **NTP log appears in web log panel.** `setSink()` was being called after the NTP sync block, so the `NTP synced:` and `NTP not available` lines went to serial only. Fixed by registering the sink immediately after the log ring is allocated, before NTP runs. The "Web monitor at …" line is now also captured.

---

## v3.0.7

+ **NTP wall-clock timestamps in web log.** `AutoLinkWeb::begin()` now calls `configTime()` immediately after WiFi connects and waits up to 5 s for an SNTP response. On success, `logSinkCb` uses `getLocalTime()` so the web log shows real EST/EDT wall-clock times that match ArduinoDroid. The timezone is `EST5EDT,M3.2.0,M11.1.0` (Eastern, auto-DST). A successful sync logs: `NTP synced: YYYY-MM-DD HH:MM:SS EST/EDT`.
+ **Uptime fallback with `*` marker.** If NTP doesn't respond within 5 s (no internet, isolated LAN, etc.) the web log falls back to `HH:MM:SS*` uptime timestamps. The `*` suffix makes it unambiguous that the time is uptime, not wall-clock. This is the same behaviour as before for no-WiFi builds, since `AutoLinkWeb` is only constructed when WiFi credentials are provided.

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
