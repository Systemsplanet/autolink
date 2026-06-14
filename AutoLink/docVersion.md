# 📅 AutoLink Version History

All releases, most recent first.

---

## v4.0.1

**Decouples app-buffer-full from the wire error threshold, and enlarges the default app buffer to fit a full Ping pipeline.** Two related changes, both motivated by the v4.0.0 hardware log analysis.

### Change 1: app-buffer-full is no longer a wire error

v4.0.0 treated an app-side `pushAppBuf` shortfall as a wire error and counted it toward `errThreshold` (default 20). With v4.0.0's stable link, this surfaced as a new failure mode: a fast Ping node sending a burst of 8 frames in 200 ms would overflow the Pong's 2 KB app buffer, the per-frame shortfall logged as an error, 15-20 of those in a row tripped the threshold, the link dropped, re-sweep, repeat. The wire itself was fine — the gap was in the app layer, not the COBS layer.

`ALink::onPayload` (v4.0.1). App-buffer-full no longer calls `err_unlocked()`. Instead:
+ The shortfall is logged at INFO level: `app buffer full: wanted N, accepted M  (app falling behind wire; frame dropped, link stays OK)`.
+ `cobsGaps_` is incremented so the gap is visible on the dashboard (a `cobsSeq` number went missing from the wire).
+ The frame is dropped; the next valid `cobsSeq` will be accepted normally.
+ The link stays in OK; no BREAK, no re-sweep.

Wire errors (CRC-8 fail, COBS desync, oversize frame) still count toward `errThreshold` — those are real wire-layer problems and the existing behavior is correct.

### Change 2: default app buffer auto-sized to (WINDOW + 2) * (maxMsg + MSG_HDR)

`AutoLink` facade constructor (v4.0.1). The app buffer auto-size formula was:
+ **v4.0.0:** `2 * (maxMsg + MSG_HDR)` = 2 * 1030 = 2060 bytes for default `maxMsg=1024`. With Ping's 8-message pipeline (~8 KB) arriving in 200 ms, the buffer overflows after just 2 messages.
+ **v4.0.1:** `(WINDOW=8 + PONG_HEADROOM=2) * (maxMsg + MSG_HDR)` = 10 * 1030 = **10300 bytes** for default `maxMsg=1024`. An entire Ping pipeline fits in the app buffer, and the 2-message Pong headroom covers the round-trip echo. The buffer never overflows under normal Ping/Pong traffic.

The user can still override `cfg.streamBufferSize` explicitly if they need more. The new `AutoLink::getStreamBufferSize()` getter returns the post-auto-size value for the dashboard to display.

### Diagnostic in the v4.0.0 → v4.0.1 change

The v4.0.0 hardware log (2-board Ping/Pong, ~30 seconds of steady state) showed exactly this pattern: link locks cleanly at 115200 baud, Ping's 8-frame pipeline drained into Pong in ~200 ms (`RX cobsSeq=0..12` arriving at ~15 ms intervals), Pong's 2 KB app buffer overflowed at `cobsSeq=13` (`wanted 250, accepted 42`), 15+ more `app buffer full` errors fired in a row, all of them counted toward `errThreshold=20`, the threshold tripped, `BREAK received -> re-sweep`, repeat. The cobsSeq layer itself was working correctly throughout — only 1 gap and 2 stale frames were observed in 50+ successful echoes, vs the v3.x behavior of perpetual desync storms every 3 seconds. The v4.0.1 fix separates the wire error counter from the app-side back-pressure indicator, and gives the app buffer enough room to absorb a full Ping pipeline without overflowing.

### Backward compatibility

v4.0.1 is wire-compatible with v4.0.0. No change to the protocol, wire format, or cobsSeq semantics. The only changes are:
+ Error accounting: app-buffer-full is no longer a wire error.
+ Auto-sized default: app buffer is now 5x larger by default (10300 vs 2060 for maxMsg=1024).

---

## v4.0.0

**MAJOR WIRE-FORMAT CHANGE. v4.0.0 nodes are NOT interop-compatible with v3.x nodes.** Both ends of every AutoLink link must be on v4.0.0 (or later, v4-compatible) firmware.

Eliminates the **v3.0.0..v3.2.10 disconnect storm** that has been present since the pipelined FIFO compare was introduced. Every reliable-mode frame now carries a 1-byte `cobsSeq` (0..255, wraps) and the receiver drops stale or out-of-order frames **at the wire layer**, before they can desync the message parser. The FIFO length/CRC compare in `UtilPing` is gone — echoes are matched by `cobsSeq`, so a wire-byte shift that used to lock Ping in OK with `rx=0` forever while Pong bounced on desyncs is now an out-of-window `cobsSeq` that the receiver rejects before the message layer ever sees it.

**Wire format changes (v4.0.0 — not v3-compatible).**

+ **Control frames (PING, REQ, best-ack):** `{0xAA, 0x55, cobsSeq, payload, CRC8(first-4)}` — 5 bytes, was 4. The `cobsSeq` field carries the sender's per-link counter but is NOT used to order command frames (each command is independent). It is logged at INFO for diagnostic visibility.
+ **Reliable-mode data frames:** `[0x00] [COBS(cobsSeq | payload) | CRC8(cobsSeq | payload)] [0x00]` — the `cobsSeq` byte is the first decoded byte of every reliable-mode frame. Payload is the message data without CRC8; the trailing CRC8 is verified by the wire layer before the payload is handed to the message layer.
+ **Keepalive (in OK with reliable mode):** a 0-payload reliable frame so the receiver's gap detection sees a continuous `cobsSeq` stream. Wire: `[0x00, COBS(cobsSeq | CRC8(cobsSeq)), 0x00]` — 5 bytes on the wire, was 2 bytes (`0x00 0x00`).
+ **`UtilFrameRx::Listener::onPayload(uint8_t cobsSeq, const uint8_t* b, int n)`** — now takes the `cobsSeq` byte so the owner can do gap/stale detection before invoking the message parser.

**`ALink::onPayload(uint8_t cobsSeq, const uint8_t* b, int n)`** — gap/stale detection rules:

+ First valid frame: `lastRxCobsSeqSet_ = true`, `lastRxCobsSeq_ = cobsSeq`. No rejection.
+ Subsequent frames: `expected = (uint8_t)(lastRxCobsSeq_ + 1)`. If `cobsSeq == expected`, accept and advance. If `cobsSeq != expected`:
  - `diff = (cobsSeq - lastRxCobsSeq_) mod 256`. If `0 < diff <= 3` → **GAP** (one or two lost frames in flight). Drop the frame, do NOT advance `lastRxCobsSeq_` (the next valid frame is still expected+1, so the receiver resyncs in one frame). `cobsGaps_` increments.
  - Otherwise (duplicate, wraparound duplicate, big skip) → **STALE** (a frame from an earlier session or a previous-window duplicate). Drop without advancing. `cobsStale_` increments.

**Sender side:** `ALink::cobsSeq_` is incremented on every reliable-mode **data frame** TX (one number per frame sent). Keepalive consumes a number. Control frames (PING, REQ, best-ack) do NOT consume a number — they're independent.

**Link drop resets both sides.** `dropLink_unlocked()` sets `cobsSeq_ = 0` and `lastRxCobsSeqSet_ = false` on the local side. The peer does the same when it receives the BREAK. After re-sweep, both sides start from `cobsSeq=0`, so any stale bytes from the previous session are immediately rejected as out-of-window on the very first frame.

**`UtilPing::loop()` FIFO rewrite.** v3.x had an 8-deep FIFO of `{len, crc16, seq}` keyed by `pendHead_/pendTail_/pendCount_` with a `msgSeq_` counter. v4.0.0 has an 8-element `pending_[WINDOW]` array of `{active, len, crc16}` scanned linearly for a free slot on send and a matching `(len, crc)` on echo. The head/tail/seq-pointer dance is gone — the wire layer's `cobsSeq` does the ordering. `postSettleDrained_` and the `tSweepStall_` watchdog are retained as belt-and-suspenders safety nets, but in normal operation the cobsSeq layer handles every case they used to catch.

**New public diagnostics** (all on `AutoLink`, with matching `get*()` on `ALink`):

+ `getCobsSeq()` — sender's next `cobsSeq` to use.
+ `getLastRxCobsSeqSet()` / `getLastRxCobsSeq()` — receiver's last-accepted `cobsSeq` (or "unset" before the first frame).
+ `getCobsGaps()` — total gap events seen on RX since boot.
+ `getCobsStale()` — total stale events seen on RX since boot.

**New debug logs** at every `cobsSeq` event:

+ `TX cobsSeq=N  M payload bytes  K wire bytes` (debug) — every reliable-mode data frame.
+ `TX keepalive cobsSeq=N` (debug) — every keepalive.
+ `RX cobsSeq=N  M payload bytes  -> app buffer` (debug) — every accepted reliable-mode frame.
+ `RX cobsSeq=N GAP: expected E, last good=L  M payload bytes DROPPED` (info) — a gap was detected and a frame was dropped.
+ `RX cobsSeq=N STALE: expected E, last good=L  M payload bytes DROPPED` (info) — a stale frame was dropped.
+ `Locked at N baud (fast-ack cobsSeq=C)` / `Locked at N baud (cobsSeq=C)` — control-frame lock transitions now include the `cobsSeq` byte of the received command.
+ `SWP Pong: first PING at baud[N]=B (cobsSeq=C)` — first PING at a baud now logs the `cobsSeq`.

**New host test suite `ALinkCobsSeqTest.cpp`** (11 tests) pins the `cobsSeq` behavior:

+ First frame is accepted and sets `lastRxCobsSeqSet_`.
+ Consecutive frames advance `cobsSeq`.
+ Gap in `cobsSeq` drops the frame, app buffer stays clean.
+ Gap then recover on the expected next seq.
+ Duplicate `cobsSeq` is stale (not a gap).
+ Wraparound at 256 is continuous (seq=255 → seq=0 is NOT a gap).
+ Post-wraparound gap is detected.
+ Sender's `cobsSeq` increments per data frame.
+ `dropLink()` resets `cobsSeq` on both sides.
+ Wire-byte-shift is caught at the `cobsSeq` layer (the v3.x bug, now an explicit test).
+ Gap/stale counters are accessible via the public API.

**New `UtilFrameRx` tests** (15 tests, two new for `cobsSeq`):

+ Zero-byte payload with `cobsSeq` is delivered (lets a sender emit a "seq-only" frame if it ever needs to).
+ `cobsSeq=0xFF` passes through (no special-casing in the wire layer).

**Pre-existing v3.x test bugs fixed** as a side effect of the protocol rewrite:

+ `test_error_counter`, `test_error_counter_during_swp`, and the Case 4 sub-test in `test_error_counter_link_failures` (ALinkErrorTest) were not setting `cfg.errThreshold` and were testing against the default 20. They now set `errThreshold=2` to actually trip the threshold in 3 errs.
+ `test_best_baud_selection` (ALinkNegotiationTest) was written for the v3.0.0 slowest-first `pickBest()` and was failing under v3.2.10's fastest-first behavior. Updated expectations.
+ `test_highest_baud_with_threshold_wins`, `test_baud_below_threshold_falls_back`, `test_strict_threshold`, `test_lenient_threshold_picks_flaky_top`, `test_explicit_expected_samples_overrides`, `test_realistic_cable_scenario` (UtilBaudSweepTest) — all written for the v3.0.0 slowest-first `pickBest()`. Renamed to `test_fastest_baud_above_threshold_wins` and updated to verify the v3.2.10+ fastest-first contract.

**Motivation — the bug that has been here since v3.0.0.** Pipelined echo verification (introduced v3.0.0) compared the oldest pending slot's `(len, crc)` to the incoming echo's `(len, crc)`. A wire-byte shift in the middle of the pipeline produced a "valid" COBS frame whose decoded message had the wrong length (e.g. `sent=656 echoed=780` from the user's log). The v3.x protocol's recovery was: drop the FIFO, send a BREAK, re-sweep. But the wire bytes-in-flight at the time of the BREAK (Pong's TX ring still draining) arrived at Ping's UART AFTER the re-sweep completed, and Ping's settle-drain didn't catch them (the drain ran immediately on link-up, but Pong's TX ring finishes draining ~300 ms later). The stale echoes were matched against the new pipeline, failed the FIFO compare, and triggered another BREAK — the v3.x "PING sent 8 msgs pendCount=8 → 3 second stall → BREAK sent (desync recovery: stall)" cycle. The user's `16:51:35`..`16:52:22` log shows this pattern running continuously. v4.0.0's cobsSeq gap detection catches every one of these stale frames at the wire layer — the pipeline never sees them.

**Upgrade notes.** Both ends of every AutoLink link must be flashed to v4.0.0 (or later v4-compatible firmware) before they can talk to each other. A v3.x node on one end and a v4.0.0 node on the other will not establish a link: the 4-byte control frames from v3.x will fail the v4.0.0 5-byte CRC check, and the v3.x node will not recognize the v4.0.0 cobsSeq byte in the wire stream.

---

## v3.2.11

Adds millisecond precision to live log timestamps so timing-sensitive diagnostics (PING-settling countdown, BREAK storm, CRC-reject burst, MISMATCH + re-sweep) are resolvable to the millisecond instead of getting smeared across the second.

**`AutoLinkWeb::logSinkCb()` timestamp format.** Changed from `HH:MM:SS` to `HH:MM:SS.mmm` (12 chars + optional `*` uptime marker, buffer bumped from 12 to 16). The NTP-synced path now uses `gettimeofday()` + `localtime_r()` to read the wall-clock seconds and sub-second microseconds from the same `timeval` snapshot — no skew between the seconds and millis fields. The no-NTP fallback uses `millis()` directly. Three-digit zero-padded millis (`%03d`) so the column lines up in monospace. Format examples:
+ `16:51:37.842 I ALinkWeb NTP synced: 2026-06-13 16:51:37 EST/EDT`
+ `16:52:08.451 D Ping settling  10 ms remaining`
+ `00:00:39.001* D Pong echo #1  76 bytes  ok`  ← the `*` suffix still marks uptime-only timestamps

**Motivating use case.** `UtilPing`'s settle-phase debug log prints "N ms remaining" on every `loop()` call (~4 ms apart). At second-resolution the entire 300 ms settle collapses to a single timestamp; with milliseconds, the per-iteration ticks are individually visible. The Ping log in v3.2.10 was producing ~80 nearly-identical lines per settle that all shared the same `HH:MM:SS` prefix, making the log file unusable; the millisecond column makes each entry a unique point on a time axis.

---

## v3.2.10

Adds the `test_embedded.ino` self-loopback test for the `AutoLink` facade and hardens `UtilBlink::flashBlocking()` against the race where the esp_timer task is still dispatching the previous async pattern's callback when the blocking variant is entered.

**Embedded facade test (`test/test_embedded/test_embedded.ino`).** New self-contained on-hardware test that covers the public surface the host test suite cannot reach: real UART peripheral, FreeRTOS stream buffer, esp_timer, and the AutoLink wiring. Flash to a single board with GPIO17 (TX) jumpered to GPIO16 (RX) — external self-loopback, the board talks to itself — and watch the serial monitor. Eleven sub-tests cover construction, state API (`ready`, `getCurrentBaud`, `getErrCount`, `getLifetimeErrors`), the two- and three-arg `getStats`/`resetStats`/`resetErrors` forms, the Stream byte API (`available`/`peek`/`flush`/`write`), the message API (`sendMsg`/`recvMsg`), `isHealthy`, the async and blocking `blinkWait` paths, the `n<=0` ignored case, `dropLink()` safety before negotiation, and `err()`/`clearErr()`. The host test suite covers `ALink`, `UtilBlink`, `UtilCobs`, `UtilCrc`, `UtilFrameRx`, `UtilBaudSweep`, and the `AutoLink` facade stubs; this file is the missing end-to-end coverage path for the hardware layer. The test is invoked from `loop()` (not `setup()`) via a `facCheckStarted`/`facCheckDone` state machine so the Arduino-ESP32 core's deferred "After Setup End" task fires its board-info dump before the suite starts; `[ALL_TESTS_DONE]` is printed on success.

**`UtilBlink::flashBlocking()` race fix.** The blocking variant calls `cancel()` to stop any in-flight async pattern. ESP-IDF's `esp_timer_stop()` is non-blocking: per the framework source, *"after esp_timer_stop() the timer is disarmed, but its callback may still be running."* If the esp_timer task is mid-dispatch on the previous pattern's callback (`cb()` → `tick()`) when `flashBlocking` enters, the callback can run concurrently with the blocking loop and re-arm the timer via `startOnce()`. Fixed by adding a new `IBlinkHal::yield()` (no-op default; `portYIELD()` in the ESP implementation) called once after `cancel()` and before the for-loop, so any in-flight callback runs to completion and observes the post-cancel state (`left=0`, `on=false`) before the loop starts toggling the pin. The cost is a single yield-from-task switch — a few microseconds. No-op on host; the existing `UtilBlinkTest` cases (`test_blocking_sequence`, `test_blocking_cancels_async`) still pass because the `MockBlinkHal` doesn't override `yield()` and gets the default no-op.

**`docVersion.md` reorder.** The v3.2.6 / v3.2.7 / v3.2.9 entries were appended at the bottom of the file even though the header says "most recent first" — they now sit in the correct position immediately below v3.2.5, restoring the descending-order invariant.

---

## v3.2.9

Fixes auto-baud negotiation locking at the wrong (slower) baud when both Ping and Pong enter the sweep state simultaneously, and fixes a stale-echo contamination window in Ping's post-reconnect settle period.

**Root cause 1 — `pickBest()` iterated slowest-first:** The loop in `UtilBaudSweep::pickBest()` searched from the highest baud index (9600, slowest) to the lowest (115200, fastest), returning the first baud that met the reliability threshold. When late SWP entry caused Ping to miss the first one or two 115200-baud PINGs (score 2/4, threshold 3), 115200 fell below the threshold while 19200 accumulated a full score (3/4) later in the sweep. `pickBest()` returned 19200 and both sides locked there, producing a 2-minute re-negotiation storm as Ping continued sweeping at 115200 while Pong was locked at 19200.

**Root cause 2 — fast-ack and `bestSpd_unlocked()` did not prefer faster bauds with partial scores:** Even after fixing the loop direction, a slower baud that happens to be the only one meeting the strict threshold was still selected. A faster baud (e.g. 115200) that scored 2/4 is physically reachable — it missed the threshold due to timing jitter, not a link failure — and should always be preferred over a slower baud.

**Root cause 3 — post-settle stale-echo window in UtilPing:** The pre-settle drain in `UtilPing` ran immediately on link-up, before Pong's TX ring had finished draining residual echoes from the previous session. Those bytes arrived at Ping's UART during the 300 ms settle and were not cleared, causing the first one or two echoes of the new session to be mismatched against the wrong message.

+ **`UtilBaudSweep::pickBest()`** — loop direction reversed to fastest-first (j = 0 → size-1). Now returns the fastest baud meeting the threshold rather than the slowest. Fallback (no baud meets the strict threshold) also searches fastest-first, returning the fastest baud with any score instead of the slowest.
+ **Fast-ack baud preference in `ALink.cpp` (Pong SWP path):** After `pickBest()` selects a baud, a secondary scan checks whether any faster baud (lower index) also received PINGs. If so, that faster baud is used regardless of whether it met the strict threshold. Locking at the fastest physically-reachable baud is always correct; downgrading to a slower baud because timing jitter reduced its score is not.
+ **`ALink::bestSpd_unlocked()` (LCK path):** Same fastest-with-any-score preference applied to the non-fast-ack negotiation path.
+ **`UtilPing` post-settle re-drain:** A second drain pass now runs after the 300 ms settle timer expires, immediately before the first new send. This catches stale echoes that arrived at Ping's UART during the settle window. A `comm_.flushRx()` follows to clear any residual partial frame. The drain guard (`postSettleDrained_`) resets on each link-loss so it re-arms for every reconnect.

---

## v3.2.7

Fixes TX ring overflow causing silent frame truncation, and adds Pong pacing to prevent TX/RX lock contention.

**Root cause:** `uart_write_bytes` return value was silently ignored throughout. A maxMsg=1024 payload produces ~1270 COBS bytes on the wire. With txBufferSize=1024 (previously shared with rxBufferSize), the TX ring overflowed on the 5th frame of a max-size message. uart_write_bytes accepted fewer bytes than requested, the frame was truncated, and the receiver got a CRC8 mismatch — logged as a frame error with no indication that TX was the source. Separately, when Pong echoed multiple large messages per loop() in a tight while loop, uart_write_bytes blocked (portMAX_DELAY) while holding the ALink protocol lock, preventing the UART event task (which also needs the lock) from draining the RX ring, causing RX overflow and data loss.

+ **`AutoLinkConfig::txBufferSize`** — new config field (default 0 = auto-sized). AutoLink auto-sizes it to `2 × ((maxMsg + MSG_HDR) × 5/4 + 64)` bytes, enough to hold two max-size COBS-encoded messages without any blocking. EspHal now uses `cfg.txBufferSize` separately from `cfg.rxBufferSize` in `uart_driver_install`.
+ **`ILink::tx()` returns `int`** — bytes actually accepted by the UART driver. MockHal returns n (always accepts), AutoLinkTest mock updated.
+ **TX truncation detection in `write()`, `writeLocked()`, `sendFrame()`, `sendFrame_unlocked()`, keepalive** — all now check the `hw.tx()` return value. A short return logs `Log.error()` with the frame size, bytes accepted, and a hint about txBufferSize, then calls `err_unlocked()` so repeated truncations trip errThreshold and drop the link cleanly.
+ **`UtilPong::MAX_TX_PER_LOOP = 2`** — mirrors Ping's pacing. Pong now echoes at most 2 messages per `loop()` call, allowing the TX ring to drain between iterations and releasing the ALink lock so the UART event task can process RX.
+ **Error log audit** — `FIFO cleared (dropped>0)` promoted from debug to `Log.error()` (data was dropped). `link lost` promoted from debug to `Log.info()`. `drain: partial msg at link-up` promoted to `Log.error()`.

---

## v3.2.6

Fixes a persistent recv-desync storm that survived the v3.2.5 `flushRx()` fix.

**Root cause:** `flushRx()` only reset the FreeRTOS stream buffer. The UART driver has a separate ring buffer (`rxBufferSize` bytes). The UART event task immediately pumped the ring buffer back into the stream buffer after every `clearAppBuf()` call. The next `recvMsg` read freshly-arrived stale bytes and rejected, causing an infinite reject loop. Evidence: every reject after `resetFifo_` showed `head=0 tail=2` — the new 2-message sends had barely been queued, yet stale bytes were already back in the stream.

No amount of stream-buffer flushing can win this race against live wire traffic from Pong. The only reliable fix is to stop Pong's TX via a BREAK.

+ **`ILink::flushRxHw()`** — new virtual method (no-op default for MockHal). Flushes the hardware receive buffer (UART ring on ESP32).
+ **`EspHal::flushRxHw()`** — calls `uart_flush_input(uart_num)`.
+ **`ALink::flushRx()`** — now calls both `clearAppBuf()` and `hw.flushRxHw()`. Logs bytes discarded for diagnostics.
+ **`UtilPing::resetFifo_(reason, dropLink)`** — desync paths (recv reject, CRC/length mismatch, stall) now call `comm_.dropLink()` which sends a BREAK to Pong, stopping its echo stream. The "link drop" path still calls `flushRx()` only (link is already going down). Each dropLink path logs `"BREAK sent (desync recovery: <reason>)"` so the cause is visible in the web monitor.
+ **`UtilPong::loop()`** — link-up drain loop now calls `comm_.flushRx()` after exiting (drain loop exits on `recv=-1` from partial stale messages, previously leaving residual bytes; this caused the 13-reject burst at link-up seen in the v3.2.5 soak). `recv=-1` during normal operation now also calls `comm_.flushRx()`.

---

## v3.2.5

Fixes a persistent recv-desync that locked Ping in OK with `rx=0` forever while Pong echoed cleanly (`disc=0 errs=0` on both sides, no link drop, no re-sweep).

**Root cause:** When `UtilPing::resetFifo_()` was called (recv reject, CRC mismatch, or stall timeout), it cleared the in-flight FIFO but left stale echo bytes in the ALink app buffer. The next `recvMsg` call started fresh at `rxMsgLen=-1` (correct) but read a 6-byte header from the old echo stream, CRC16 failed against the new in-flight sequence, and this repeated indefinitely. Recovery was impossible because `onPayload()` resets the consecutive-error counter on every valid COBS frame (the wire layer was fine), so `errThreshold` was never reached. The idle watchdog also didn't fire because `lastRxMs` stayed current.

+ **`ALink::flushRx()` / `AutoLink::flushRx()`** — new public method that clears the receive app buffer and resets `rxMsgLen=-1` without dropping the link or restarting the sweep.
+ **`UtilPing::resetFifo_()`** — now calls `comm_.flushRx()` on every path (recv reject, CRC mismatch, length mismatch, stall, link drop) so the next send/recv batch starts against a clean buffer.
+ **New host test** `test_flushRx_after_desync` in `ALinkMessageTest`: verifies that a clean round-trip succeeds immediately after a corrupt-frame reject + `flushRx()` call.

---

## v3.2.4

Fixes a recovery deadlock exposed once the v3.2.3 drop->BREAK storm was gone: with staggered reboots (Pong restarting after Ping had already locked), the Ping would sit in OK forever at 115200 sending data with `rx=0`, never re-sweeping, while the Pong stayed in SWP receiving those data bytes but scoring `0/3` PINGs (a node in OK streams application frames, not the PING command frames the Pong locks onto).

+ **Removed a stray `hw.stopTimer()` in the master fast-ack lock path.** It ran immediately after `hw.startTimer(okTickMs())`, killing the OK idle-watchdog and keepalive timer. With the watchdog dead, a Ping that fast-ack locked never noticed a peer that rebooted into SWP. With the timer left running, the no-RX idle watchdog fires (default 5 s), drops the stale link, sends a BREAK, and both ends re-sweep with PINGs so the Pong can re-lock. This is the recovery path the watchdog was designed to provide; it was simply never armed on a fast-ack lock.

---

## v3.2.3

Stability pass — fixes a self-reinforcing drop->BREAK storm and a terminal sweep deadlock observed in the ping-pong soak test (both nodes bouncing on `Error threshold exceeded (6 > 5)` and `BREAK received`, ending with one node wedged in SWP while the peer reports a false `WIRING CHECK` / 0 raw bytes).

+ **`errThreshold` default 5 -> 20.** A burst that overruns the peer, or mutual-sweep garbage at boot, produces a short run of COBS desyncs with no good frame between to reset the consecutive-error counter. At 5 that transient tripped the threshold, dropped the link, and fired a BREAK that re-swept the peer — a loop. 20 absorbs the transient; a genuinely broken line still trips it (it never yields a good frame to reset the count).
+ **`UtilPing` paced sends (`MAX_TX_PER_LOOP = 2`).** Filling the full `WINDOW` (8) in one `loop()` dumped up to eight KB-sized frames back-to-back, overrunning Pong's RX (partial writes + COBS desync, the root of the error bursts above). Sends now fill the pipeline over several ticks.
+ **`EspHal::startTimer/stopTimer` now block briefly and retry.** The FreeRTOS timer-service command queue can fill under a rapid drop->sweep->drop storm; with a 0 block time the change-period/start command was silently dropped, leaving the sweep timer stopped — the node sat in SWP emitting no PINGs, so the peer read 0 raw bytes and printed the misleading wiring warning. Commands now use a 20 ms block time with one retry.
+ **`idleTimeoutMs` default 3000 -> 5000.** A full five-baud sweep plus re-lock can exceed 3000 ms once a side is in SWP, so the idle watchdog was dropping links mid-recovery (`Idle for 3950 ms`). 5000 clears the worst-case recovery window.
+ **`minAcceptRate` default 0.5 -> 0.75.** Fast-ack could lock on ~2 decoded PINGs and then collapse under the first burst. Requiring 3 of 4 samples favors a stable lock.

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

+ **NTP wall-clock timestamps in web log.** `AutoLinkWeb::begin()` now calls `configTime()` immediately after WiFi connects and waits up to 5 s for an SNTP response. On success, `logSinkCb` uses `getLocalTime()` so the web log shows real EST/EDT wall-clock times that match ArduinoDroid. The timezone is `EST5EDT,M3.2.0,M11.1.0` (Eastern, auto-DST). A successful sync logs: `NTP synced: YYYY-MM-DD HH:MM:SS EST/EDT`. *(Superseded by v3.2.11: timestamps now have millisecond resolution; the `getLocalTime()` call was replaced by `gettimeofday()` + `localtime_r()` so the seconds and millis come from one snapshot.)*
+ **Uptime fallback with `*` marker.** If NTP doesn't respond within 5 s (no internet, isolated LAN, etc.) the web log falls back to `HH:MM:SS*` uptime timestamps. The `*` suffix makes it unambiguous that the time is uptime, not wall-clock. This is the same behaviour as before for no-WiFi builds, since `AutoLinkWeb` is only constructed when WiFi credentials are provided. *(Superseded by v3.2.11: format is now `HH:MM:SS.mmm*`, e.g. `00:01:23.456*`.)*

---

## v3.0.6

+ **Live log timestamps.** Each log entry is now stored as `HH:MM:SS I Tag message` (uptime-based, from `millis()`). Previously the format was `[I][Tag] message` with no time component. *(Superseded by v3.2.11: timestamp now `HH:MM:SS.mmm` with millisecond resolution; NTP path uses wall-clock, no-NTP path stays uptime with `*` marker.)*
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
+ **UART event task:** `std::vector<uint8_t>` replaced with an `alloca`-backed scratch buffer to avoid per-event heap churn.
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
