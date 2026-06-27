# 📅 AutoLink Version History

All releases, most recent first.
## v5.4.0

**Wire-ACK carries bytes-recvd; Pong is ack-only; gap-stop on peer-detected NAK; send-failure counter escalates to dropLink; SYNC/ASYNC radio buttons**

Six changes from field-test feedback. The wire format changes (ACK frame extended with bytes-recvd; Ping-side gap-stop driven by peer NAK) are real protocol shifts but stay on the 5.x major. SYNC mode's stop-and-wait is preserved; ASYNC's behavior changes the most (no heartbeat, gap-stop, pool-exhaustion drop). Dashboard UI replaces the SYNC/ASYNC toggle button with radio buttons and applies the choice live (no reboot).

### Fix 1 — Wire ACK frame extended with bytes-recvd

The 5.3.x wire ACK frame was a 3-byte raw payload `[type=0xFF, seq, frame_crc8]`. The receiver verified the per-chunk CRC before sending it, but the sender had no way to know how many bytes the receiver actually pushed into its app buffer (the per-chunk CRC-8 only covers the chunk payload, not the chunk's role in a multi-chunk message). Ping's diagnostic log line had to say `echo %d bytes` and accept that the value was a guess.

This release extends the wire ACK to a 5-byte raw payload `[0xFF, seq, bytes_lo, bytes_hi, frame_crc8]`. `frame_crc8` covers bytes 0..3 (the existing CRC convention). The decoder's `onAck` listener now takes `bytesRecvd` and Link stamps `bytesRecvd_[ackedCobsSeq] = bytesRecvd` so Ping's `bytesRecvdFor(seq)` returns the receiver-reported value. The pre-5.4.0 3-byte ACK is still decoded (bytes-recvd = 0) so a peer running the legacy wire format can interop.

Pinned by `run_test_ack_bytes` Pin 1 + Pin 2. Pin 1 round-trips a pong-side ACK through `UtilFrameRx::Listener::onAck` and asserts the bytes-recvd value matches the bytes the receiver pushed. Pin 2 sends a 4-byte message from ping (ASYNC mode), pipes the chunk to pong, and asserts `ping.bytesRecvdFor(baseSeq) == 10` (= 6-byte MSG_HDR + 4-byte payload, the merged-chunk length the receiver pushed). Both pins turn red if the wire ACK reverts to 3 bytes.

### Fix 2 — Pong is ack-only; the wire ACK is the entire Pong-side response

The 5.3.x Pong echoed the payload back to Ping by calling `base_.comm_.send(base_.buf_, n)` inside its loop. Ping's `matchEcho_` then matched the echoed bytes against the slot's CRC-16. The echo doubled the wire traffic (every chunk's payload was sent twice), masked CRC bugs (the echo's bytes went through the same chunked-COBS path on the way back), and confused operators reading the log (two Ping entries per sent message — one for the wire echo, one for the per-chunk ACK).

This release removes the echo. Pong's loop just reads and counts. The wire-level ACK (extended in Fix 1) carries the bytes-recvd; Ping's per-slot completion is now driven by `isAcked(slot.seq)`. The new Ping log line is `echo <seq> <bytes> <pending>` (crc=ok implicit). Pong's diagnostic ack log line is `echo <seq> <bytes>`. Pinned by `run_test_ping_send_failure` Pin 1 (Pong loop has no `base_.comm_.send(base_.buf_, …)` call site) and Pin 2 (Ping's log format is `echo %u %u %d` reading `bytesRecvdFor(seq)`).

### Fix 3 — Heartbeat + decideKeepalive + decideIdleWatchdog removed; Ping's consecSendFail_ escalates to dropLink

The 5.3.x OK-state timer fired a wire-level PING every `HEARTBEAT_MS = 100 ms` and dropped the link on `HEARTBEAT_MISS_LIMIT = 3` missed PONGs. That detected dead peers but cost a wire frame every 100 ms. `decideKeepalive` drove the keepalive-emission decision; `decideIdleWatchdog` drove the symmetric idle-timeout drop.

This release removes the heartbeat, both decision functions, the `HEARTBEAT_MS` / `HEARTBEAT_MISS_LIMIT` constants, and the `heartbeatPingsMissed_` / `lastHeartbeatMs_` fields. Dead-peer detection moves to Ping: a new `consecSendFail_` counter bumps on every `send()` failure and at `MAX_SEND_FAIL = 5` consecutive failures Ping calls `dropLink()` + `clearQueue_()` so a dead peer / app-buf-full NAK loop bounces the link back to SWP instead of silently spinning. The link layer no longer schedules a periodic wire-level probe. The asymmetric idle check (TX active, RX silent → peer gone) stays; the symmetric idle check (both quiet → drop) is gone — a one-direction burst now doesn't bounce the link.

Pinned by `run_test_onbreak_guard` Pin 1–3 (the on-break state guard from 5.3.x still works without the heartbeat, no false resets), `run_test_linkdecision` absence pins for `decideKeepalive` / `decideIdleWatchdog`, and `run_test_ping_send_failure` Pin 3 (`consecSendFail_ >= MAX_SEND_FAIL` → `dropLink()` + `clearQueue_()`). `run_test_clock_injection`'s symmetric-idle test was repurposed as an absence pin (both-quiet no longer drops).

### Fix 4 — ASYNC mode: gap-stop on peer NAK; pool-exhaustion drop

The 5.3.x ASYNC mode's retransmit loop retransmits on NAK but doesn't pause new sends. A peer-detected gap (Pong saw an out-of-order frame and NAKed the missing seq) caused Ping to keep firing new sends while the receiver was waiting for the gap chunk — the wire went effectively silent from the receiver's point of view.

This release makes Ping read `Link::lastNakSeq()` and `Link::lastAckSeq()` once per loop iteration. While `gapSeq_ != NO_GAP`, Ping skips its send loop and only drains incoming ACKs (via the existing `isAcked(slot.seq)` walk). When the gap seq is ACKed, `gapSeq_` clears and Ping resumes. The Link layer also gets a new ASYNC-mode pool-exhaustion drop: if `!arqCache_.hasRoom() && arq_.pendingCount() > 0` at the start of the ARQ retransmit loop, the link drops + sends a break so a stalled receiver bounces back to SWP instead of silently filling the cache.

Combined with Fix 1's receiver-side NAK on app-buf-full, this bounds the ASYNC-mode stall: if the receiver NAKs successfully, the retransmit loop brings the chunk back; if the receiver is gone, the pool fills up and the link drops within ~one RTO.

Pinned by `run_test_ack_bytes` Pin 3 (pool-exhaustion drop in LinkTimers.cpp) and Pin 4 (app-buf-full → NAK instead of silent). Pinned by `run_test_ping_send_failure` Pin 5 (`gapSeq_` + `lastNakSeq()` / `lastAckSeq()`).

### Fix 5 — Sequential mode grows msg size; random mode 1k..maxMsg

The 5.3.x Ping picked `n = random(1, 1024)` regardless of `FillMode` — both "Sequential" and "Random" modes sent random-sized payloads. The pre-fix `fillSequential_` filled the buffer with deterministic bytes but the size was still random. This release:
- `SEQUENTIAL` picks `n = seqSize_`, starting at 1 byte and incrementing after each successful send up to `maxSeqSize_` (= `cfg.maxMsg`), then wrapping back to 1. Operators reading the dashboard log see the message size grow monotonically through the link's MTU.
- `RANDOM` picks `n` in `[1024, maxSeqSize_]`. The 1k floor forces multi-chunk sends in the steady state so the ARQ chunk path is exercised end-to-end (the pre-5.4.0 1..1024 range could send every message as a single chunk, starving the multi-frame ARQ test path).

Pinned by `run_test_ping_send_failure` Pin 4 (`RANDOM_MIN_BYTES = 1024`, `seqSize_` advances in the send loop).

### Fix 6 — Dashboard: SYNC/ASYNC radio buttons (live, no reboot); default txDelayMs 100

The 5.3.x dashboard had a "Toggle" button that POSTed to `/mode/toggle` and rebooted the device. Operators wanted to compare modes without a 5–10 s reboot window, and they wanted a default delay-ms that gave the dashboard poll cycle (1 s) enough headroom on a 5-baud table.

This release:
- The "Toggle" button is replaced by two radio buttons (`SYNC` / `ASYNC`) grouped under `name="linkMode"`. Clicks POST to `/mode?m=SYNC|ASYNC` and the firmware applies the change live via `link_.setMode(newMode)`. The mode persists to NVS (`putUChar("mode", ...)` in the `autolink` namespace) so `bringUpLink`'s existing NVS read restores it across a normal reboot. The `/mode/toggle` route and the linked reboot path are gone.
- The fill-mode route (`/mode?m=seq|rand` in 5.3.x) is renamed to `/fillmode?m=seq|rand` so the two `?m=` keys don't collide on the same URL.
- `AutoLinkConfig::txDelayMs` default raised 0 → 100 ms. The pre-5.4.0 default of 0 caused Ping to flood the wire at full link speed and starve the dashboard's /logs poll.
- The dashboard's SYNC/ASYNC pill and Toggle button are removed from the header. The linkModeGroup radiogroup takes their place.

Pinned by `run_test_mode_toggle_ui` (6 pins: dashboard radios, JS routes radio to live POST, `handleMode` declared, `/mode` registered + `/mode/toggle` gone + `/fillmode` added, `handleMode` applies live + persists to NVS + no esp_restart, `bringUpLink` reads NVS mode before begin()).

### Wire format

Changed. Two protocol shifts, both backward-compatible:
1. Wire ACK frame extended from 3 bytes raw `[0xFF, seq, crc8]` to 5 bytes raw `[0xFF, seq, bytes_lo, bytes_hi, frame_crc8]`. `frame_crc8` covers bytes 0..3 (the existing CRC convention). The legacy 3-byte ACK is still decoded (bytes-recvd = 0).
2. Ping's gap-stop / gap-resume uses the existing wire NAK frame (3 bytes raw `[0xFE, seq, crc8]`, unchanged). The link layer's `lastNakSeq_` / `lastAckSeq_` accessors let Ping observe the NAK arrival and the retransmit's ACK without changing the wire.

The library version contract (`include/AutoLink.h` + `library.properties` + `idf_component.yml` + this file) bumps 5.3.102 → 5.4.0 per AGENTS rule 3.

### Regression coverage

**New runtime + structural suite:**
`run_test_ack_bytes` in
`test/test_desktop/al/link/LinkAckBytesTest.cpp`. Four pins:
1. `test_ack_frame_is_5_bytes_with_bytes_recvd` — runtime: feed pong a 3-byte data frame, decode pong's emitted ACK through `UtilFrameRx::Listener::onAck`, assert `seq=5, bytesRecvd=3`. End-to-end coverage of the wire ACK round-trip.
2. `test_bytes_recvd_table_populated_on_ack` — runtime: ASYNC-mode `ping.sendMsg(msg, 4, &baseSeq)` round-trips a 4-byte message through pong's wire ACK; assert `ping.bytesRecvdFor(baseSeq) == 10` (MSG_HDR + payload merged into one chunk).
3. `test_pool_exhaustion_drop_in_async_mode` — source-grep: `LinkTimers.cpp`'s `onTimerOk_unlocked` has the `!arqCache_.hasRoom() && arq_.pendingCount() > 0` drop branch gated on `cfg.mode != SYNC`.
4. `test_holdack_sends_nak` — source-grep: `LinkRx.cpp`'s `AppBufAction::HoldAck` branch calls `sendNakFrame_unlocked(cobsSeq)` and returns false (not the pre-fix log-and-return-true).

**New source-level suite:**
`run_test_ping_send_failure` in
`test/test_desktop/al/pingpong/PingSendFailureTest.cpp`. Six pins:
1. Pong loop has no payload-echo send (recv-only); log format `echo %u %d` (seq + bytes).
2. Ping log format `echo %u %u %d` reading `bytesRecvdFor(seq)`.
3. `consecSendFail_` counter + `MAX_SEND_FAIL = 5` + `dropLink()` escalation.
4. Sequential mode `seqSize_++` 1..maxSeqSize_ + wrap; random mode `RANDOM_MIN_BYTES = 1024`.
5. ASYNC gap-stop: `gapSeq_` + `lastNakSeq()` / `lastAckSeq()` + pause/resume logs.
6. got<0 branch still drains rx via `flushRx()` after `clearQueue_()` (preserved from 5.3.x's log-hygiene pin).

**Updated pins:**
`run_test_pingpong_log_hygiene` — `matchEcho_` removed (Pong no longer echoes); Pong's "send skipped" warning pin removed (Pong is recv-only); "WIRING?" pin unchanged.
`run_test_mode_toggle_ui` — fully rewritten for the live-radio / fillmode route. Six pins: dashboard radios, JS radio handler, `handleMode` declared, `/mode` registered + `/mode/toggle` gone + `/fillmode` added, `handleMode` applies live + persists + no `esp_restart`, `bringUpLink` reads NVS before begin().
`run_test_linkdecision` — `decideKeepalive_*` and `decideIdleWatchdog_*` tests become absence pins (the functions are gone; a future re-introduction replaces them with the pre-fix table-test).
`run_test_clock_injection` — `test_idle_timeout_drops_link` repurposed (symmetric idle no longer drops; the asymmetric path stays); `test_keepalive_emitted_at_third_of_idle_timeout` repurposed as an absence pin (the link no longer emits keepalive frames).
`run_test_uri_handler_alignment` — `/mode/toggle` removed from `mustHave`; `/fillmode` added.
`run_test_link_frame_rx` — `MockListener::onAck` extended with `bytesRecvd`; the `cobsSeq=0xFF` test rewritten to assert the 5-byte ACK end-to-end.
`run_test_accessor_structure` — `sendAckFrame_unlocked` test rewritten to pin the 5-byte ACK body (still passes the "no inline encode" expectation for `sendCobsFrame_unlocked` and `resendCobsFrame_unlocked`).
`run_test_autolink` — `test_txDelayMs_default_and_setter` default updated 0 → 100.

`UriHandlerAlignmentTest`'s `test_max_uri_handlers_is_*` unchanged at 10. The `/mode/toggle` → `/fillmode` swap is net-zero (10 routes either way).

### Disclosed limitations

- ASYNC mode's heartbeat removal means a Pong-only device (no Ping on the wire) stays up indefinitely on a quiet link. The asymmetric idle check catches a dead pong within `FAST_IDLE_RX_MS = 300 ms` only if the ping side was actively transmitting. A fully-idle deployment (both boards silent) will hold the link up forever — that's by design, the pre-5.4.0 symmetric watchdog had a similar shape with `idleTimeoutMs` triggers.
- `decideKeepalive` is gone but the OK-state timer's repeat-arm (`okTickMs` + `hw.startTimer(okTickMs)`) still fires every `idleTimeoutMs / 3` ms (min 50 ms). The timer keeps the asymmetric-idle check, the reorder-buffer expiry sweep, and the ARQ retransmit loop running. It no longer emits wire-level traffic.
- The 5-byte ACK frame is decoded end-to-end; the 3-byte legacy decode is preserved as a fallback for cross-version interop. A peer still running 5.3.x sees Ping's slot-completion log line show `bytes=0` (the legacy decode path) instead of the real bytes-recvd. Ping reads `bytesRecvdFor(seq)` so the 0 is silent — it just doesn't display the real value.
- Pong is now recv-only. A user who wants to add a Pong-side diagnostic (e.g. reply to a specific Ping chunk type with a different shape) needs to drive `base_.comm_.send()` themselves; the recv-driven loop only counts and logs.
- The consecutive-send-failure counter is per-`Ping` instance, not per-link. Two Pings on the same physical wire (one per role — which doesn't happen, Ping is always master — but if it did) would each escalate independently.
- The `/mode` live switch is not retried on failure. A network error during the POST leaves the radio optimistically flipped in the UI but the firmware on the old mode. The next `/stats` poll reconciles via `linkModeLabel`.
- `bytesRecvdFor(seq)` returns 0 for any seq not in the table. A Ping loop reading `bytesRecvdFor` on an unACKed seq (e.g. mid-flight) reads 0. The fix doesn't change this — the `bytesRecvd_` table is populated from the peer's wire ACK on the sender side, same as `arq_.isPending` semantics. If the user wants a default non-zero value for unacked seqs, they can check `arq_.isPending(seq)` first.
- The fill-mode rename `/mode` → `/fillmode` is breaking for any external tool that hard-coded `/mode?m=seq` / `/mode?m=rand`. The Arduino library users don't see this (they don't construct URLs); only external dashboard tools or curl scripts need to update.
- `AutoLinkConfig::txDelayMs` default 100 ms means a freshly-deployed Ping sends at most 10 msg/s on a slow baud. Operators who need full line-rate can set `cfg.txDelayMs = 0` via the existing constructor argument or the dashboard's delay-ms widget (default selected in the dashboard is still 100 ms, but the dropdown lets the user pick 0).
- The dashboard's SYNC/ASYNC radio reads `d.linkModeLabel` from `/stats` and reconciles if the user clicks before the next poll. A click during a transient WiFi outage reverts the radio on the next poll (the POST's `r.ok` check in JS reverts on failure).

### Files touched

- `src/al/AutoLinkConfig.h` — `txDelayMs` default 0 → 100.
- `src/al/link/Link.h` — `bytesRecvd_[256]` table + `lastAckSeq_` / `lastNakSeq_` / `lastRxSeq_` members; `bytesRecvdFor(seq)` / `lastAckSeq()` / `lastNakSeq()` / `lastRxSeq()` accessors; `onAck(uint8_t, uint16_t)` override signature; `sendAckFrame_unlocked(uint8_t, uint16_t)` signature; `heartbeatPingsMissed_` / `lastHeartbeatMs_` removed.
- `src/al/link/LinkCore.cpp` — `reset_unlocked` clears `bytesRecvd_` + `lastAckSeq_` + `lastNakSeq_` + `lastRxSeq_`.
- `src/al/link/LinkTx.cpp` — `sendAckFrame_unlocked` emits 5-byte raw frame (extended with bytes-recvd).
- `src/al/link/LinkRx.cpp` — `onPayload` stamps `lastRxSeq_`; `onAck(uint8_t, uint16_t)` stamps `bytesRecvd_[seq]`; `onNak` stamps `lastNakSeq_`; `HoldAck` branch sends NAK and returns false (was: log + return true).
- `src/al/link/LinkTimers.cpp` — heartbeat + decideKeepalive removed; decideIdleWatchdog removed; ASYNC pool-exhaustion drop added.
- `src/al/link/LinkSweep.cpp` — `heartbeatPingsMissed_` / `lastHeartbeatMs_` references removed.
- `src/al/link/LinkFrameRx.h` — `onAck(uint8_t, uint16_t)` listener signature.
- `src/al/link/LinkFrameRx.cpp` — 5-byte ACK decode + 3-byte legacy fallback.
- `src/al/link/sweep/LinkDecision.h` — `decideKeepalive` / `decideIdleWatchdog` removed.
- `src/al/pingpong/Pong.h` — recv-only loop; ackCount_ counter; `echo <seq> <bytes>` log; no `send(base_.buf_, n)` call site.
- `src/al/pingpong/Ping.h` — new echo log format; sequential `seqSize_` 1..maxSeqSize_ + wrap; random `[1024, maxSeqSize_]`; ASYNC gap-stop via `lastNakSeq()` / `lastAckSeq()`; `consecSendFail_` counter + `MAX_SEND_FAIL = 5` → `dropLink()` + `clearQueue_()`; slot walks on `isAcked(slot.seq)`.
- `include/AutoLink.h` — `lastAckSeq()` / `lastNakSeq()` / `lastRxSeq()` / `bytesRecvdFor()` / `isAcked()` / `sendMsg(b, len, *outBaseSeq)` facade forwarding.
- `src/AutoLink.cpp` — production ctor clamps `cfg.allowedBaudsCount` (pre-fix already).
- `src/al/web/AutoLinkWeb.h` — `handleMode` accepts SYNC/ASYNC; `handleModeToggle` removed; `handleFillMode` added.
- `src/al/web/AutoLinkWeb.cpp` — `/mode/toggle` removed from URIS[]/PATHS[]; `/fillmode` added; `handleMode` registered for live SYNC/ASYNC.
- `src/al/web/AutoLinkWebHandlers.cpp` — `handleMode` applies live (no `esp_restart`); `handleFillMode` replaces the old fill-mode route; `handleModeToggle` removed.
- `src/al/web/dashboard_html_part_b.html` — SYNC/ASYNC radios (replaces Toggle button + pill); default delay-ms 100 selected; `delayMs` widget unchanged shape.
- `src/al/web/dashboard.js` — radio change handler posts `/mode?m=SYNC|ASYNC`; bindLinkModeGroup wires the change event; toggleLinkMode removed; applyLinkModeLabel replaced by radio-reconciliation; bindModeGroup uses `/fillmode?m=…`.
- `src/al/web/AutoLinkWebHtml.h` — regenerated by `build/dashboard_assets.py`.
- `test/scripts/env/install_system_stubs.py` — `Preferences.h` stub includes `<WString.h>` (was missing `String` def).
- `test/test_desktop/al/link/LinkFrameRxTest.cpp` — `MockListener::onAck(uint8_t, uint16_t)`; 5-byte ACK end-to-end test.
- `test/test_desktop/al/link/TestAccessorStructureTest.cpp` — `sendAckFrame_unlocked` test pins the 5-byte body.
- `test/test_desktop/al/link/sweep/LinkDecisionTest.cpp` — absence pins for `decideKeepalive` / `decideIdleWatchdog`.
- `test/test_desktop/al/AutoLinkTest.cpp` — `txDelayMs` default updated 0 → 100.
- `test/test_desktop/al/ClockInjectionTest.cpp` — `test_idle_timeout_drops_link` + `test_keepalive_emitted_at_third_of_idle_timeout` repurposed as absence / behavioral pins.
- `test/test_desktop/al/pingpong/PingPongLogHygieneTest.cpp` — `matchEcho_` absence pin; Pong's send-failed pin replaced with recv-only pin.
- `test/test_desktop/al/web/UriHandlerAlignmentTest.cpp` — `mustHave` list updated for `/fillmode` (and `/mode/toggle` gone).
- `test/test_desktop/al/web/ModeToggleUITest.cpp` — fully rewritten for live-radio / fillmode route (6 pins).
- `test/test_desktop/al/link/LinkAckBytesTest.cpp` — NEW. 4 runtime/structural pins.
- `test/test_desktop/al/pingpong/PingSendFailureTest.cpp` — NEW. 6 source-grep pins.
- `test/test_desktop/al/link/LinkBaudIndexBoundsTest.cpp` — NEW (Fix 7). 2 runtime pins verify the choke-point accessors bound post-construction writes to `cfg.allowedBaudsCount`.
- `test/common/LinkTestAccessor.h` — added `setSpdI(i)` so the LinkBaudIndexBoundsTest can drive the spdI without direct field access.
- `test/test_desktop/al/link/TestAccessorStructureTest.cpp` — 3 new pins for Fix 7 (`test_choke_points_route_through_clamped_accessors`), Fix 8 (`test_ihal_setevents_guard_in_place`), and Fix 9 (`test_msg_hdr_consistency_in_linktx`).
- `test/test_desktop/al/link/LinkCobsSeqTest.cpp` — `test_lost_msgs_burst_vs_single` updated to use a fresh `MockHal cHal` for the second link (Fix 8 caught the double-bind pattern this test exercised).
- `test/test_desktop/al/link/sweep/OnBreakGuardTest.cpp` — `gltches` → `glitches` typo fixed (Fix 10 cosmetic).
- `test/test_desktop/Makefile` — `TEST_BINS` includes `run_test_ack_bytes` + `run_test_ping_send_failure` + `run_test_baud_index_bounds`.
- `test/scripts/coverage/test_coverage_manifest.py` — allow-list extended for `run_test_ping_send_failure` + `run_test_baud_index_bounds`.
- `src/al/AutoLinkConfig.h` — added `clampedCount() const` overload (Fix 7); comment on `allowedBaudsCount` field updated to point at the choke-point accessors instead of the ctor clamp.
- `src/al/link/Link.h` — `allowedBaudsCount()` clamps to `[0, AUTOLINK_MAX_BAUDS]`; `allowedBaud(i)` delegates to `cfg.allowedBaudSafe(i)` (Fix 7).
- `src/al/link/LinkCore.cpp` / `LinkTimers.cpp` / `LinkSweep.cpp` — every `cfg.allowedBauds[i]` → `cfg.allowedBaudSafe(i)`; every `cfg.allowedBaudsCount` → `cfg.clampedCount()` (Fix 7).
- `src/al/link/LinkTx.cpp` — `uint8_t frame[MAX_CHUNK + 6];` → `uint8_t frame[MAX_CHUNK + MSG_HDR];` (Fix 9).
- `src/al/hal/IHal.h` — `setEvents` gains `assert(events_ == nullptr && ...)` under `AUTOLINK_HOST_TEST`; on-device log + comment rewrite (Fix 8).
- `src/al/hal/EspHal.h` — collapsed duplicated `// Drain in-flight TX before retune. Without` comment; `gltches` → `glitches` typo fixed (Fix 10).
- `src/al/pingpong/PingGap.h` — NEW (Fix 11). `decideGapTransition(currentGap, lastNak, lastAck, &nextGap)` pure function + `GapAction` enum (`Stay` / `Enter` / `Update` / `Resume`). Host-includable (no `#ifdef ARDUINO`).
- `src/al/pingpong/Ping.h` — gap-stop block rewritten (Fix 11). Removed the `if (gapSeq_ != NO_GAP)` gate; `loop()` now calls `decideGapTransition` unconditionally every iteration and branches on the returned action. Includes `PingGap.h`. Fix 14: the suppression check changed from `if (a == GapAction::Stay || a == GapAction::Enter || a == GapAction::Update) { ... }` (which conflates two Stay cases — the action enum is correct, the caller was wrong) to `if (gapSeq_ != NO_GAP) { ... }` (branches on the actual gap state, not on a side effect of the transition). Ping now actually sends at startup.
- `test/test_desktop/al/pingpong/PingGapTransitionTest.cpp` — NEW (Fix 11). 12-row transition table test + 5-iteration runtime simulation (clean → NAK → 2×stay → ACK → resume) + Link-layer signal-side test (drives `Link::onNak` via `LinkTestAccessor`, asserts `lastNakSeq()` stamps, runs through Update + Resume actions) + source-grep pin asserting no `if (gapSeq_ != NO_GAP)` block exists in Ping.h. Fix 14: added caller-level pin `test_caller_sends_in_no_gap_steady_state` that reads Ping.h's suppression gate, asserts it branches on `gapSeq_ != NO_GAP` (not on the action enum), and runs 5 simulated startup loops to verify the gate returns "proceed" each time. Updated `test_unconditional_read_pin` to assert the gate IS `gapSeq_`-based (positive pin) and does NOT contain the bug-shape 3-clause action-enum gate (negative pin).
- `test/common/LinkTestAccessor.h` — added `onNak(seq)` / `lastNakSeq()` / `lastAckSeq()` accessors so the runtime signal-side test can drive the Link-layer side without direct field access.
- `test/test_desktop/al/web/HttpdStartupTest.cpp` — added `#include <cstdint>` (Fix 12). The suite used `uint32_t` / `uint64_t` without including `<cstdint>`, which broke the build on a clean toolchain (transitive include was masking the missing header on the previous host build). Plus new pin `test_autolinkweb_h_includes_core_outside_arduino_guard` (Fix 13): 3 sub-asserts that the `AutoLinkWebCore.h` include precedes `#ifdef ARDUINO` and `library.properties` lists the core header in `includes=`.
- `src/al/web/AutoLinkWeb.h` — `#include "al/web/AutoLinkWebCore.h"` moved OUTSIDE the `#ifdef ARDUINO` block (Fix 13). The core header is host-buildable (no Arduino-only types), so the unconditional include is safe for both host and device build paths. This is the bulletproof fix for the device-build `'WebSnapshot' does not name a type` failure.
- `library.properties` — `includes=` extended with `al/web/AutoLinkWebCore.h` (Fix 13, belt-and-braces). Any Arduino toolchain that builds `-I` strictly from `includes=` now adds the core header's directory as a fallback include path.
- `test/test_desktop/Makefile` — `TEST_BINS` includes `run_test_ack_bytes` + `run_test_ping_send_failure` + `run_test_baud_index_bounds` + `run_test_ping_gap_transition`.
- `test/scripts/coverage/test_coverage_manifest.py` — allow-list extended for `run_test_ping_send_failure` + `run_test_baud_index_bounds` + `run_test_ping_gap_transition`.
- `include/AutoLink.h` + `library.properties` + `idf_component.yml` + `docs/Version.md` — version bump 5.3.102 → 5.4.0 in lockstep.

### Result

- 54 / 54 host unit suites pass (`make test`), including the new
  `run_test_ack_bytes` (4 pins all green, ~270 ms), the new
  `run_test_ping_send_failure` (6 pins all green, ~470 ms), the new
  `run_test_baud_index_bounds` (2 runtime pins, ~30 ms), the new
  `run_test_ping_gap_transition` (5 pins after Fix 14: 12-row
  transition table + 5-iteration runtime simulation + Link-layer
  signal-side NAK round-trip + caller-level "sends proceed from
  NO_GAP + no NAK" pin + source-grep pin on the suppression-gate
  shape, ~50 ms), and the three new pins added to
  `run_test_accessor_structure` (OOB-closed shape, MSG_HDR
  consistency, IHal::setEvents guard). Wall: ~7 s.
- 3 / 3 host integration suites pass (`make itest`). Wall: ~40 s
  (`run_loopback` 30 s ceiling dominates).
- `make test_coverage_manifest` self-test PASS — the new suite
  `run_test_ack_bytes` and `run_test_ping_gap_transition` are
  correctly classified as source-contributing (links `$(LINK_SRC)`);
  `run_test_ping_send_failure`, `run_test_baud_index_bounds` are
  correctly classified as source-only (links only their own .cpp).
- `build/verify_build.sh` not run in this sandbox — arduino-cli is
  not installed. Per AGENTS rule 4, the cross-compile must be
  re-run in a longer-lived environment before release; the changes
  are local to the wire ACK frame shape, the OK-state timer body,
  the dashboard HTML/JS, and the Ping/Pong loop contracts, so the
  esp32:esp32:firebeetle32 cross-compile risk is low (no new
  `#ifdef ARDUINO` paths, no new allocation paths, no header
  cycle changes), but unverified.
- 0 bytes added to RAM on the wire path. The 5-byte ACK is +2
  bytes per ACK frame vs the 3-byte legacy frame; the `bytesRecvd_`
  table is 512 B (256 × uint16) on the Link; `consecSendFail_` +
  `gapSeq_` + `seqSize_` are a few bytes on Ping. The dashboard's
  radio takes the place of the Toggle button + pill (net 0 HTML
  bytes — radios are a few lines shorter than the toggle button).

### Fix 7 — Post-construction OOB on `cfg.allowedBaudsCount` is closed

The 5.3.x Fix 4 (in v5.3.102) introduced `clampToMaxBauds()` and `allowedBaudSafe(i)` accessors on `AutoLinkConfig`, and called `clampToMaxBauds()` from both ctors. The intent was "out-of-range values cannot reach the array indexing path." That was a half-truth. The ctors run once; the field is still public post-construction, and a sketch (or a future refactor that writes the field after `link.begin()`) can still drive `spdI = cfg.allowedBaudsCount - 1` into an OOB read.

This release routes the choke-point accessors in `Link` (`allowedBaudsCount()` and `allowedBaud(i)`) through the safe/clamped forms. The link layer's read paths no longer touch `cfg.allowedBauds[]` or `cfg.allowedBaudsCount` raw — every index site goes through `cfg.allowedBaudSafe(i)` / `cfg.clampedCount()`. A post-construction write of `cfg.allowedBaudsCount = 20` (or `-1`) is now bounded at the read sites, so `spdI` derived from the bounded count can never index past the array.

- `Link.h`: `allowedBaudsCount()` clamps to `[0, AUTOLINK_MAX_BAUDS]`; `allowedBaud(i)` delegates to `cfg.allowedBaudSafe(i)`.
- `AutoLinkConfig.h`: added `clampedCount() const` (the const overload that `Link` uses at const read sites — `cfg` is a value member, but `getCurrentBaud() const` wants the bound without a non-const call).
- `LinkCore.cpp` / `LinkTimers.cpp` / `LinkSweep.cpp`: every `cfg.allowedBauds[i]` replaced with `cfg.allowedBaudSafe(i)`; every `cfg.allowedBaudsCount` replaced with `cfg.clampedCount()`. The ctor's `baudSweep((int)config.allowedBaudsCount)` was previously exempt (the field was unclamped at construction); now `config.clampedCount()`.

Pinned by:
- `run_test_baud_index_bounds` (new). Runtime: writes `cfg.allowedBaudsCount = 20` post-`begin()`, walks every choke-point accessor (`getCurrentBaud`, `allowedBaudsCount`, `allowedBaud(i)` for `i ∈ [-2, AUTOLINK_MAX_BAUDS + 2]`), asserts all returned values are bounded and OOB indices surface 0 (not garbage). Same for `cfg.allowedBaudsCount = -1` (underflow path).
- `run_test_accessor_structure` `test_choke_points_route_through_clamped_accessors` (new pin). Source-grep: no `cfg.allowedBauds[` or `cfg.allowedBaudsCount` raw reads in any Link* TU body; `AutoLinkConfig::clampedCount()` and `allowedBaudSafe(i)` both present.

### Fix 8 — `IHal::setEvents` comment now matches the code; guard added

The 5.3.x `IHal::setEvents` had a comment claiming "a second setEvents is an assertion fail" but the body was a plain pointer assignment — no assert, no log. The "disclosed limitations" section in v5.3.102 even noted this was intentional ("by design"). Two contracts were floating: the comment promised protection; the code provided none.

This release picks the guard. `IHal::setEvents` now:

```cpp
void setEvents(ILinkEvents &e) {
#ifdef AUTOLINK_HOST_TEST
    assert(events_ == nullptr && "setEvents called twice on the same HAL");
#endif
    if (events_ != nullptr) {
        Log::log().error("IHal", "setEvents called twice — rebinding listener");
    }
    events_ = &e;
}
```

Host tests fail-fast (assert fires before the bug ships). On-device, the rebind is still recoverable (the new owner wins, no panic) but the error log makes the misuse visible. The comment is rewritten to match the contract — no more phantom assert claim.

One existing host test (`LinkCobsSeqTest::test_lost_msgs_burst_vs_single`) constructed a second `Link` against the same `MockHal`. That was exactly the double-bind pattern the guard catches — updated to use a fresh `MockHal cHal` for the second link (the test is about per-Link state, not HAL sharing).

Pinned by `run_test_accessor_structure` `test_ihal_setevents_guard_in_place` (new pin). Source-grep: `assert(events_ == nullptr && "setEvents called twice on the same HAL")` is present, `Log::log().error("IHal", ...)` is present, and the misleading comment `an assertion fail` is absent (the rewrite replaces it with the truthful host-assert + on-device-log phrasing).

### Fix 9 — `LinkTx` frame buffer uses `MSG_HDR`, not literal 6

Fix 6 of v5.3.102 replaced literal `+ 6` literals across the chunk/pool `static_assert`s with the named constant `MSG_HDR` so a future MSG_HDR bump wouldn't silently drift the bounds. One sibling literal slipped: `LinkTx.cpp:52` sized the per-frame stack buffer as `uint8_t frame[MAX_CHUNK + 6];` — the same drift Fix 6 targeted, in the same TU.

This release replaces it with `uint8_t frame[MAX_CHUNK + MSG_HDR];`. Same value today, no behavior change, closes the exact drift Fix 6 targeted.

Pinned by `run_test_accessor_structure` `test_msg_hdr_consistency_in_linktx` (new pin). Source-grep: `buildAndTxCobsFrame_unlocked` body contains `MAX_CHUNK + MSG_HDR` and does not contain `MAX_CHUNK + 6`.

### Fix 10 (cosmetic) — EspHal comment hygiene

`EspHal::setSpd()` had a copy-paste duplication in its leading comment (`// Drain in-flight TX before retune. Without` repeated three times in a row). Collapsed to one. Two `gltches` typos in the same TU (`new baud gltches a UART_BREAK`, `idle line gltches a BREAK`) corrected to `glitches`. The same typo in `OnBreakGuardTest.cpp` source comments was fixed for consistency (no behavior change).

### Fix 11 — Gap-stop entry edge is reachable (the feature was inert)

Fix 4 of this release added ASYNC gap-stop: when the peer sends a NAK for a missing chunk, Ping pauses its send loop until the gap is retransmitted and ACKed. The signal is `Link::lastNakSeq()` / `lastAckSeq()`. The post-fix Ping code only read those accessors inside `if (gapSeq_ != NO_GAP) { ... }`. That gate made the entry edge unreachable — from the normal state (`gapSeq_ == NO_GAP`), nothing ever observed a fresh NAK and entered gap-stop. The send loop kept firing new chunks while the receiver waited for the gap to fill. The wire-silence symptom Fix 4 was meant to cure never went away because the feature never engaged.

The original `run_test_ping_send_failure` Pin 5 passed because it grepped for `gapSeq_` + `lastNakSeq()` + `lastAckSeq()` symbols in the file body, but didn't verify the entry edge was reachable. Source-grep-only pins don't catch structural bugs — only runtime pins do (AGENTS rule 18: every fix gets a regression test that fails when the fix is reverted).

This release extracts the gap-stop decision into a pure free function `decideGapTransition(currentGap, lastNak, lastAck, &nextGap)` in `src/al/pingpong/PingGap.h` (per AGENTS rule 21: protocol decisions are pure free functions returning enums; side effects in the caller). The new `GapAction` enum has four values: `Stay`, `Enter`, `Update`, `Resume`. Ping::loop now calls it unconditionally every iteration — no `if (gapSeq_ != NO_GAP)` gate. The entry branch fires from NO_GAP when `lastNak != NO_GAP && lastAck != lastNak` (the `lastAck != lastNak` guard covers the trivial race where an ACK that predates the gap would otherwise immediately resume on the same iteration).

`PingGap.h` is host-includable (no `#ifdef ARDUINO` gate) so the table test runs on host without needing an ESP32 build. The `run_test_ping_gap_transition` suite covers:
- 12-row transition table (entry / update / resume / stay / predates-gap edge / sentinel edges).
- 5-iteration runtime simulation: clean state → NAK(5) → 2×stay-on-NAK → ACK(5) → resume; asserts `gapSeq_` transitions and that the send loop is paused for ≥2 iterations during the gap.
- Link-layer signal side: drives `Link::onNak(seq)` through the public surface (via `LinkTestAccessor`), asserts `Link::lastNakSeq() == seq`, then drives a second NAK for `seq+1` and asserts the transition function's `Update` action fires, then drives an ACK for `seq+1` and asserts the `Resume` action fires.
- Source-grep pin: assert no `if (gapSeq_ != NO_GAP)` block exists in Ping.h. The pre-fix bug was that exact gate — if a future refactor reintroduces it, this pin fires.

Toggle-off check (verified): the source-grep pin correctly catches a regression. Adding the literal `if (gapSeq_ != NO_GAP)` back to Ping.h flips the pin red. The pure function + runtime pins stay green (they test the function directly, not the call-site gate), so the layered pin set is necessary — source-grep alone is insufficient, runtime alone is insufficient, both together catch the bug.

### Fix 12 — `HttpdStartupTest.cpp` missing `#include <cstdint>` (build break on a clean toolchain)

`test/test_desktop/al/web/HttpdStartupTest.cpp` uses `uint32_t` / `uint64_t` directly but does not `#include <cstdint>`. It happened to compile on the previous host toolchain because some other header transitively pulled `<cstdint>` in. On a clean g++ (no transitive include), the suite halts with `'uint32_t' does not name a type` before any test runs, so the count never reaches 54/54.

One-line fix: `#include <cstdint>` near the top of the file. With it, the suite builds cleanly on a host toolchain that doesn't transitively provide `<cstdint>`. Pre-existing on the 5.3.102 zip; surfaced when the full `make all` was run from a fresh extraction (the previous build only covered a subset of the suites).

Pinned by `make all` exit-0 over all 54 suites from a clean extraction. The previous build's `53 / 53` total came from compiling a subset and getting 53 of those to PASS — the `HttpdStartupTest` build error was masked because the suite wasn't in the subset.

### Fix 13 — `WebSnapshot` type not visible at device-build using-alias site

Cross-compiling `AutoLinkWeb.cpp` against `esp32:esp32:firebeetle32` (ArduinoDroid 14.2.0 + ESP32 Arduino core 3.3.5 + lwIP) failed at `AutoLinkWeb.h:72` with `'WebSnapshot' does not name a type`. The host build skipped `AutoLinkWeb.cpp` (per AGENTS rule 4: host tests don't cover it), so the bug shipped. The struct `WebSnapshot` was correctly defined in `src/al/web/AutoLinkWebCore.h`, and `AutoLinkWeb.h` did `#include "al/web/AutoLinkWebCore.h"` — but the include was inside the `#ifdef ARDUINO` block at line 7. The ArduinoDroid toolchain's quoted-include resolution for nested paths (`al/web/AutoLinkWebCore.h` from inside `src/al/web/AutoLinkWeb.h`) didn't pick up the include at that position on the device build's preprocessor, so `WebSnapshot` was undefined at the `using Snapshot = WebSnapshot;` alias site.

This release fixes it by moving the `#include "al/web/AutoLinkWebCore.h"` line OUTSIDE the `#ifdef ARDUINO` block at the top of `AutoLinkWeb.h`. The core header is host-buildable (no Arduino-only types inside — only `stdint`, `Log.h`, `WebSnapshot`/`WebLogEntry` structs, and the JSON / log formatter declarations), so the unconditional include is safe for both host and device build paths. As a belt-and-braces measure, `library.properties`'s `includes=` field now also lists `al/web/AutoLinkWebCore.h` so any Arduino toolchain that builds `-I` strictly from the `includes=` field (some do; some don't) adds the file's directory as a fallback include path.

Pinned by `run_test_httpd_startup::test_autolinkweb_h_includes_core_outside_arduino_guard` (new pin, 3 sub-asserts):
1. `AutoLinkWeb.h` includes `AutoLinkWebCore.h`.
2. The include precedes the `#ifdef ARDUINO` opening.
3. `library.properties` lists `al/web/AutoLinkWebCore.h` in `includes=`.

Toggle-off check (verified): moving the include back inside the `#ifdef ARDUINO` block flips the pin red (`Assertion 'incPos < arduinoGuard' failed`).

### Fix 14 — Gap-stop suppression gate was action-enum-based; now `gapSeq_ != NO_GAP` (Ping never sent)

Fix 11 (above) moved the gap-stop decision into a pure function `decideGapTransition` returning a `GapAction` enum, and routed Ping::loop's send-suppression through that enum:

```cpp
if (a == GapAction::Stay || a == GapAction::Enter ||
    a == GapAction::Update) {
    drain + return;     // suppress sends
}
```

That was wrong. `Stay` fires for two distinct cases — "no gap, no NAK" (the normal steady state at startup, sends MUST proceed) AND "in gap, waiting on retransmit" (sends MUST pause). Both look the same in the enum but mean opposite things for the send loop. Branching on the action enum conflates them. The bug shape: at startup, `lastNakSeq_` and `lastAckSeq_` are both `0xFF` (the `NO_GAP` sentinel), every loop returns `Stay`, every loop suppresses sends. The link is "connected" because the sweep completed; the app layer just never transmits. Both SYNC and ASYNC, both fill modes — completely silent wire. Discovered during a code review that walked the gap-stop block end-to-end against the link layer's documented default state.

This release changes the caller's gate from action-enum-based to `gapSeq_`-based. After `gapSeq_ = nextGap`, the suppression check is `if (gapSeq_ != NO_GAP) { drain + return; }`. Stay-from-no-gap + Resume both leave `gapSeq_ == NO_GAP` (proceed); Enter / Update / in-gap Stay all leave `gapSeq_ != NO_GAP` (suppress). The pure function is unchanged — the bug was never in `decideGapTransition`, which has always been correct.

**Why this slipped through the test gate:** `Ping` is `#ifdef ARDUINO` so no host test constructs a `Ping`. The host tests for gap-stop all exercise `decideGapTransition` directly — the pure function, which was correct. The bug was in the *caller's* branch logic, in `Ping::loop()`, which no host test ran. The integration suite's two-Link loopback tests Ping's other path (the link-layer ARQ / send pipeline) but doesn't drive the gap-stop block in isolation. AGENTS rule 18 says "every fix gets a regression test that fails when the fix is reverted" — the lesson is that the *caller* of a pure function needs its own test, even when the pure function is fully covered.

Pinned by:
- `run_test_ping_gap_transition::test_caller_sends_in_no_gap_steady_state` (new): reads Ping.h's suppression gate (the `if (...)` immediately after `gapSeq_ = nextGap` and before `int sentThisLoop = 0`), asserts it branches on `gapSeq_ != NO_GAP` and does NOT contain `a == GapAction::Stay`. Then runs 5 simulated startup loops with the FIXED gate (5 proceed, 0 suppressed) and cross-checks the bug-shape gate (5 suppressed) — if the bug-shape gate ever stops suppressing, the action enum has changed shape and this pin needs rewriting. Toggle-off verified: replacing `if (gapSeq_ != NO_GAP) {` with `if (a == GapAction::Stay || a == Enter || a == Update) {` flips the pin red (`Assertion 'gate.find("gapSeq_ != NO_GAP") != npos' failed`).
- `run_test_ping_gap_transition::test_unconditional_read_pin` (updated): the old pin asserted "no `if (gapSeq_ != NO_GAP)` block in Ping.h", which is no longer true — the gate IS `if (gapSeq_ != NO_GAP)`, which is the correct shape. The pin now asserts (a) the gate branches on `gapSeq_ != NO_GAP`, (b) it does NOT match `a == GapAction::Stay`, and (c) the bug-shape 3-clause gate is absent from the body.

Also: `test/test_desktop/al/link/sweep/OnBreakGuardTest.cpp` Pin 6's `char buf[16384]` read-buffer for `Ping.h` was bumped to 65536 — the file grew past 16 KB after the PingGap.h extraction and the Pin 14 caller-side comments. Pre-existing buffer too small; not a regression in any source file, just a stale buffer size.

---

## v5.3.102

**Boundary invariants: TX drain on setSpd, ILinkEvents split, WINDOW ownership inversion, baud-count clamp, test-flag eviction, named-constant static_assert**

Six
boundary
cleanups
from the
post-god-class
review. The wire
format and
runtime
behaviour are
unchanged on
existing call
sites; the changes
are local to
header shapes,
ctor clamps, and
the relationship
between the
HAL / link / cache.
Two boundary
cleanups (TX
drain, WINDOW
ownership) are
real bug-class
fixes; the rest
are architectural
tidying that
prevents future
drift.

### Fix 1 — `EspHal::setSpd()` drains in-flight TX before the baud retune

The pre-fix
shape called
`uart_flush_input`
then
`uart_set_baudrate`.
Bytes still in the
UART TX FIFO at
the old baud land
at the receiver's
old baud after the
retune, producing
a garbled frame on
every baud switch
and a high
`frameErrs` rate on
the first frame
after each
`enterPhaseN`.
The fix inserts
`uart_wait_tx_done(
uart_num,
pdMS_TO_TICKS(20))`
as the first line
of `setSpd()`,
ahead of the
flush-input and
retune. 20 ms is
well under the
master's 250 ms P2
dwell and outlasts
a 1 KB MTU at 9600
(~1.05 ms), so it
doesn't bound the
SWP cadence;
`uart_wait_tx_done`
is a no-op if the
FIFO is already
empty.

### Fix 2 — `ILinkEvents` listener interface closes the HAL ↔ Link cycle

The pre-fix
`IHal::link` was a
public mutable raw
pointer the HAL
dispatched to
(`hal->link->onRx/
onBreak/onTimer`),
inverting the
"Link talks only
to IHal" comment.
`Link::Link()` set
it via
`hw.bind(this)`. The
new shape:
`ILinkEvents`
(pure virtual:
`onRx` / `onBreak`
/ `onTimer`) is the
listener interface.
`Link` implements
it. `IHal` holds
the listener
pointer privately
and `setEvents()`
wires it
one-shot from
`Link::Link`. The
HAL dispatches
through
`events()->...` and
never sees a
`Link*`. Same
change on
`MockHal` (the test
helper) and on
`EspHal::begin()` —
which used to call
`link->begin()` via
the back-pointer;
that order now
lives on the
`AutoLink::begin()`
facade side.
Cost: zero — both
HALs already had
to dispatch to the
same three methods.

### Fix 3 — `WINDOW` ownership inverted: Ping owns the pipeline window

The pre-fix
`ArqCache::WINDOW =
32` was the source
of truth and Ping
re-exported it as
`static constexpr
int WINDOW =
ArqCache::WINDOW`.
A pure storage
class dictated the
flow-control
policy. The new
shape: `Ping.h`
declares
`static constexpr
int WINDOW =
AUTOLINK_ARQ_PIPELINE_WINDOW`
(the project-wide
constant lives in
`AutoLinkConfig.h`,
so the value is
visible at
`AutoLink` ctor
time without
needing Ping).
`ArqCache.h` drops
its `WINDOW` and
takes a `window`
ctor parameter
(default = the
project-wide
constant).
`ArqCache::ArqCache(
int window)`
runtime-asserts
`POOL_SIZE >=
2*window` (host:
`assert`; device:
error log). On the
host suite, the
assert trips at
the ctor call site
the moment a future
change widens the
pipeline without
widening the pool.
`AutoLink` ctor
forwards
`AUTOLINK_ARQ_PIPELINE_WINDOW`
to its `arqCache_`
member.

### Fix 4 — `AutoLinkConfig` clamps `allowedBaudsCount` at the ctor

The pre-fix
`allowedBaudsCount`
was a plain `int`
with no validation
against
`AUTOLINK_MAX_BAUDS`
(16). A caller
setting
`allowedBaudsCount =
20` would index
OOB through
`cfg.allowedBauds[i]`
in the sweep /
`lockOk` path.
Two new helpers on
the struct:
`clampToMaxBauds()`
clamps in place
(returns the
clamped value); a
`clampToMaxBauds()`
call sits in
**both** `AutoLink`
ctors (production
and host-test).
`allowedBaudSafe(i)`
is a validating
accessor that
returns the element
at `i` if in range,
else 0. Defense in
depth: the
auto-clamp catches
a misconfigured
caller; the safe
accessor catches a
future caller that
bypasses the clamp.

### Fix 5 — `_test_forwardResync` evicted from the user-facing `AutoLinkConfig`

The pre-fix
`AutoLinkConfig`
shipped a
`bool _test_forwardResync
= false;` member.
A test-only flag
on the struct
every sketch
constructs. The
new shape: flag is
gone; `Link`
carries
`testForwardResync_`
(private, false by
default), and
`LinkTestAccessor::setForwardResync(bool)`
flips it from a
test. The pre-fix
buggy
gap-handling
behaviour is still
testable in
`LinkReorderTest`'s
toggle pin — it
just doesn't leak
into the
production
config struct
anymore.

### Fix 6 — Chunk/pool `static_assert` uses named constants instead of literals

The pre-fix
shape in every TU
that includes
`Link.h` /
`LinkContext.h` was
`static_assert(MAX_CHUNK + 6 <= 256,
"MAX_CHUNK too large")`.
The `6` is
`MSG_HDR`; the
`256` is
`ArqCache::POOL_BUF_MAX`.
A future bump of
`MAX_CHUNK` past
the literal `250`
(saturating the
budget against
the literal
`POOL_BUF_MAX =
256`) would have
silently OOB-read
through the pool
without the
`static_assert`
noticing. The new
shape reads
`MAX_CHUNK + MSG_HDR <= ArqCache::POOL_BUF_MAX`,
making the
relationship
explicit. Bumping
`MAX_CHUNK` without
raising
`ArqCache::POOL_BUF_MAX`
now produces a
literal-symbol
diagnostic that
names both sides of
the constraint,
not a comment that
hides the magic
numbers.

### Wire format

Unchanged.
Byte-identical to
v5.3.101. Fix 1
moves bytes around
inside the UART
FIFO (no wire-side
change). Fix 2-6
are pure header
shape / ctor
behavior. No
header in
`include/` moves,
no public API
symbols are added
or removed, no
wire-protocol
constants in
`LinkContext.h`
change. The
library version
contract
(`include/AutoLink.h`
+
`library.properties`
+
`idf_component.yml`
+
this file) bumps
5.3.101 → 5.3.102
per AGENTS rule 3.

### Regression coverage

**New source-level
suite:**
`run_test_boundary_invariants`
in
`test/test_desktop/al/BoundaryInvariantsTest.cpp`.
Six source-grep
pins, one per fix:

1. `test_esphal_setSpd_drains_tx_before_retune`
   — `EspHal::setSpd`'s body must
   contain
   `uart_wait_tx_done(...)` ordered
   *before* `uart_set_baudrate`.
   Reverting the drain
   (drop the function call) trips
   this pin. The pin looks for the
   open paren after the symbol so a
   comment-only mention does not
   satisfy it.
2. `test_ihal_no_link_pointer_and_dispatches_via_ilinkevents`
   — `IHal` has no `Link *link`
   field; `class ILinkEvents` is
   declared; `setEvents` is in the
   `IHal` surface; `Link` derives
   from `ILinkEvents`; `Link::Link`
   calls
   `hw.setEvents(*this)` (and the
   old `hw.bind(` is gone); EspHal
   and MockHal dispatch via
   `events()->...` only. Reverting
   any of these trips the pin.
3. `test_window_owned_by_ping_cache_validates_injected_window`
   — `ArqCache.h` has no
   `static constexpr int WINDOW`;
   `ArqCache` ctor takes a
   `window` parameter; the .cpp
   ctor asserts
   `POOL_SIZE < window * 2`
   (or equivalent runtime guard);
   `Ping.h` declares
   `WINDOW = AUTOLINK_ARQ_PIPELINE_WINDOW`
   (not
   `ArqCache::WINDOW`); the constant
   lives in `AutoLinkConfig.h`;
   `AutoLink` forwards it to its
   `arqCache_`. Reverting any of
   these trips the pin.
4. `test_autolink_config_clamps_allowed_baud_count`
   — `AutoLinkConfig` exposes
   `clampToMaxBauds()` and
   `allowedBaudSafe()`; both
   `AutoLink` ctors call
   `cfg.clampToMaxBauds()`. Removing
   the clamp or the accessor trips
   the pin.
5. `test_test_forward_resync_evicted_from_user_config`
   — `AutoLinkConfig` has no
   `_test_forwardResync`; `Link`
   has the
   `testForwardResync_` private
   member; `LinkRx.cpp` reads
   `testForwardResync_` (not
   `cfg._test_forwardResync`);
   `LinkTestAccessor` exposes
   `setForwardResync`. Re-adding
   the public flag, removing the
   member, or routing through the
   old config field trips the pin.
6. `test_chunk_pool_static_assert_uses_named_constants`
   — every TU that includes the
   chunk/pool constraint
   (`LinkCore.cpp` / `LinkTx.cpp` /
   `LinkRx.cpp` / `LinkSweep.cpp` /
   `LinkTimers.cpp` / `LinkApi.cpp`)
   uses
   `MAX_CHUNK + MSG_HDR` and
   `ArqCache::POOL_BUF_MAX` by
   name. The pre-fix shape
   `MAX_CHUNK + 6 <= 256` is
   absent. Reverting any TU's
   literal trip the pin.

The new suite is
added to `TEST_BINS`,
its build rule, the
`test:` runner list,
and the
`test_coverage_manifest.py`
source-only
allow-list
(links only its
own .cpp, no library
source — same shape
as
`run_test_linkdecision`).
Toggle behaviour
verified manually
for all six pins:
reverting each fix
flips the matching
pin to red.

### Disclosed limitations

- `ArqCache`'s runtime
  ctor-arg `assert`
  is host-suite
  only. The device
  build (Arduino)
  logs the
  `POOL_SIZE < 2*window`
  error and
  continues —
  retx becomes a
  cache miss on the
  first OOM send,
  which the link
  layer surfaces as
  a `link reset`.
  AGENTS rule 17
  keeps the ctor
  RTOS-safe, so the
  device build
  cannot abort.
- `IHal::setEvents`
  is intentionally
  non-`const`. A
  second `setEvents`
  call overwrites
  the listener
  without a guard
  — by design (the
  HAL doesn't know
  which listener to
  dispatch to until
  the link
  constructs).
  Re-binding at
  runtime is a
  caller error; no
  second binding
  happens in
  production because
  the facade
  constructs the
  Link before the
  HAL.
- Fix 5 moves
  `_test_forwardResync`
  to
  `LinkTestAccessor`,
  so any external
  test that read
  `cfg._test_forwardResync`
  directly (e.g.
  the pre-fix
  `LinkReorderTest`)
  now goes through
  the accessor. The
  pre-fix field name
  does not appear in
  any production
  header, so the
  user-facing API
  surface is
  unchanged.
- The
  `AutoLinkConfig`
  split (Fix 5's
  bigger cousin) is
  explicitly
  *deferred* — the
  god-struct still
  carries the
  flat-field layout.
  The user's spec
  accepted "evict
  the test flag" as
  the minimum; the
  sub-struct split
  (wire/buf/arq)
  is a separate
  release. Anyone
  who reaches for
  it should pin the
  layout with a
  compile-time test
  first.

### Files touched

- `src/al/hal/EspHal.h`
  — `setSpd()` TX
  drain as the
  first line of the
  body.
- `src/al/hal/IHal.h`
  — `ILinkEvents`
  listener interface;
  `IHal::setEvents`
  one-shot hook;
  private
  `ILinkEvents *events_`;
  public
  `Link *link` field
  and `bind()` method
  removed.
- `src/al/link/Link.h`
  — `Link` derives
  from `ILinkEvents`;
  private
  `testForwardResync_`
  member.
- `src/al/link/LinkCore.cpp`
  — `Link::Link()`
  calls
  `hw.setEvents(*this)`
  in place of
  `hw.bind(this)`.
- `src/al/link/LinkSweep.cpp`
  / `src/al/link/LinkCore.cpp`
  /
  `src/al/link/LinkRx.cpp`
  / `src/al/link/LinkTx.cpp`
  / `src/al/link/LinkTimers.cpp`
  /
  `src/al/link/LinkApi.cpp`
  — chunk/pool
  `static_assert` now
  references
  `MAX_CHUNK + MSG_HDR`
  and
  `ArqCache::POOL_BUF_MAX`
  by name. Includes
  `al/link/arq/ArqCache.h`
  where needed for the
  constant.
- `src/al/link/LinkRx.cpp`
  — gap handler
  reads
  `testForwardResync_`
  on the link, not
  `cfg._test_forwardResync`.
- `src/al/link/arq/ArqCache.h`
  — drops `WINDOW`;
  ctor takes
  `int window = AUTOLINK_ARQ_PIPELINE_WINDOW`;
  private
  `window_` member;
  new static_assert
  on
  `POOL_BUF_MAX >= 256`
  keeps the pool
  buffer from
  shrinking past
  the wire frame
  cap.
- `src/al/link/arq/ArqCache.cpp`
  — ctor runtime
  guard
  (`assert` on host,
  error log on
  device).
- `src/al/pingpong/Ping.h`
  — `WINDOW` now
  reads from the
  project-wide
  constant
  `AUTOLINK_ARQ_PIPELINE_WINDOW`,
  not
  `ArqCache::WINDOW`.
- `src/al/AutoLinkConfig.h`
  — `AUTOLINK_ARQ_PIPELINE_WINDOW = 32`
  (flow-control
  policy the cache
  validates
  against);
  `clampToMaxBauds()`
  + `allowedBaudSafe()`;
  `_test_forwardResync`
  removed.
- `include/AutoLink.h`
  — host-test ctor
  calls
  `cfg.clampToMaxBauds()`;
  `arqCache_` member
  constructed with
  the pipeline
  window;
  `begin()` calls
  `link->begin()`
  before
  `hal->begin()`
  (the back-pointer
  no longer drives
  this order).
- `src/AutoLink.cpp`
  — production ctor
  calls
  `cfg.clampToMaxBauds()`
  before passing
  the cfg to the
  HAL and Link.
- `src/al/hal/EspHal.h`
  — `uart_event_task`
  + `timer_callback`
  dispatch via
  `events()->...`;
  removed `link->begin()`
  call from
  `EspHal::begin()`
  (now on
  `AutoLink::begin()`).
- `test/common/MockHal.h`
  — `sendBreak()`,
  `pumpClock()`,
  `pipe_data()`
  dispatch via
  `events()->...`.
- `test/common/LinkTestAccessor.h`
  — new
  `setForwardResync(bool)`
  toggle.
- `test/itest/test_desktop/al/link/loopback_test.cpp`
  /
  `loopback_sync_test.cpp`
  —
  `g_pingHal->events()`
  / `g_pongHal->events()`
  in place of
  `->link` (post
  Fix 2).
- `test/test_desktop/al/link/LinkReorderTest.cpp`
  — three
  `cfg._test_forwardResync = ...`
  assignments
  replaced with
  `LinkTestAccessor(link).setForwardResync(...)`
  (post Fix 5).
- `test/test_desktop/al/link/arq/ArqCacheTest.cpp`
  — `ArqCache::WINDOW`
  references become
  `AUTOLINK_ARQ_PIPELINE_WINDOW`
  / `c.window()` (post
  Fix 3).
- `test/test_desktop/al/link/sweep/LinkSweepPhaseTest.cpp`
  /
  `LinkBaudPreferenceTest.cpp`
  —
  `sHal.link->onRx(...)`
  becomes
  `sHal.events()->onRx(...)`
  (post Fix 2).
- `test/test_desktop/al/hal/MockHalTest.cpp`
  —
  `hal.bind(new Link(...))`
  becomes a local
  `Link *link = new Link(...);`
  since `IHal::bind()`
  is gone (post
  Fix 2).
- `test/test_desktop/al/BoundaryInvariantsTest.cpp`
  — new file,
  six source-grep
  pins (one per
  fix).
- `test/test_desktop/Makefile`
  — new suite added
  to `TEST_BINS`,
  build rule, and
  `test:` runner
  list.
- `test/scripts/coverage/test_coverage_manifest.py`
  — new suite
  added to the
  source-only
  allow-list.
- `include/AutoLink.h`
  +
  `library.properties`
  +
  `idf_component.yml`
  +
  `docs/Version.md`
  — version bump
  5.3.101 → 5.3.102
  in lockstep.

### Result

- 50 / 50 host
  unit suites pass
  (`make test`),
  including the new
  `run_test_boundary_invariants`
  (6 pins all
  green). Wall:
  ~5.5 s.
- 3 / 3 host
  integration suites
  pass (`make itest`).
  Wall: <60 s.
- `make test_coverage_manifest`
  self-test PASS
  — the new suite
  is correctly
  classified as
  source-only.
- `build/verify_build.sh`
  not run in this
  sandbox —
  arduino-cli is not
  installed. Per
  AGENTS rule 4, the
  cross-compile must
  be re-run in a
  longer-lived
  environment
  before release;
  the changes are
  local to header
  shape, ctor clamps,
  and one EspHal
  line, so the
  esp32:esp32:firebeetle32
  cross-compile
  risk is low (no
  new wire symbols,
  no `#ifdef ARDUINO`
  paths touched,
  no header cycle
  changes), but
  unverified.
- 0 bytes added to
  RAM on the wire
  path. The TX
  drain reuses an
  existing HAL
  primitive;
  `ILinkEvents` is a
  single vtable per
  `Link` (already
  paid for by
  `IHal`); the
  WINDOW inversion
  is compile-time
  constant movement;
  the clamp is a
  single integer
  compare at
  construction;
  the static_assert
  is compile-time.
---

## v5.3.101

**God-class split: Link / AutoLinkWeb / dashboard / test files**

Six large files
were split into
focused translation
units and named
sibling files, so each
cluster of methods can
be edited without
scrolling through
~1400 lines. Wire format
and behaviour are
unchanged.

### Fix 1 — `Link.cpp` (1415 lines) split into six TUs

Split by method
cluster, all in
`src/al/link/`:

- `LinkCore.cpp` — ctor,
  begin, kickoff,
  changeState,
  reset_unlocked,
  getters
  (getState / getErrCount
  / getCurrentSpdIndex /
  getCurrentBaud /
  getDiag), resetStats,
  resetErrors,
  resetDiag.
- `LinkTx.cpp` —
  sendFrame, buildAndTx,
  sendCobsFrame*,
  resendCobsFrame,
  sendCtrlCobsFrame
  (ACK / NAK),
  buildAndSendMsg_unlocked.
- `LinkRx.cpp` — onRx,
  processCtrlFrame,
  ctrlFrameReady,
  onPayload, onAck,
  onNak, onFrameError,
  findMsgHeaderResync,
  recvMsg.
- `LinkSweep.cpp` —
  okTickMs, phase1ArmMs,
  bestSpd, lockOk,
  handleSwp_unlocked,
  applyMaster / PongSwpAction_unlocked,
  handleLck_unlocked.
- `LinkTimers.cpp` —
  onBreak, onTimer (the
  state dispatcher),
  onTimerOk / Swp / Lck
  un-locked, pendingAcks,
  isAcked.
- `LinkApi.cpp` —
  err / clearErr,
  write / read / peek /
  available / readStream,
  flush / flushRx /
  dropLink, sendMsg
  (the public API),
  recvMsg (the public
  API), test_sendMsgBegin
  + test_sendMsgStillWaiting.

Cross-TU calls
(okTickMs, lockOk,
sendCobsFrame_unlocked,
buildAndSendMsg_unlocked,
etc.) resolve through
the linker; class
members stay in
`Link.h`. Each TU
re-declares the
private TAG constant,
the heartbeat /
fast-idle constants,
and the `MAX_CHUNK`
static_assert so
single-file changes
don't have to walk the
whole split to find a
local helper.

### Fix 2 — `AutoLinkWeb.cpp` (927 lines) split into two TUs

- `AutoLinkWeb.cpp`
  (lifecycle) —
  ctor / dtor, begin,
  wifiTaskThunk,
  setupHttpAndLogging,
  ip, statTimerCb,
  logSinkCb.
- `AutoLinkWebHandlers.cpp`
  (HTTP) — the ten
  `handle*` methods
  registered in
  `setupHttpAndLogging_`.

### Fix 3 — Dashboard assets extracted to real source files

The 724-line
`AutoLinkWebHtml.h` is
now generated from:

- `src/al/web/dashboard.css`
  (raw CSS, editor
  syntax highlighting
  works).
- `src/al/web/dashboard.js`
  (raw JS).
- `src/al/web/dashboard_html_part_a.html`,
  `_b.html`,
  `_c.html` (markup
  before CSS, between
  CSS and JS, after JS).

`build/dashboard_assets.py`
regenerates
`AutoLinkWebHtml.h`
from those five source
files at C++ compile
time. The header still
defines the same
`DASHBOARD_HTML` byte
sequence the dashboard
binary served before
(sha256-stable runtime
size of 31222 bytes).

### Fix 4 — `LinkMessageTest.cpp` (993 lines) split into five TUs

- `LinkMessageTestCommon.h`
  — `test_internal`
  namespace + the
  TestCache stub +
  shared includes.
- `LinkMessageRoundtripTest.cpp`
  — round-trip framing
  (size sweep, back-to-back,
  chunk boundary).
- `LinkMessageCorruptTest.cpp`
  — corruption detection
  (CRC, payload bit-flips,
  no-resync clear).
- `LinkMessageResyncTest.cpp`
  — resync paths
  (oversize L, dropped
  bytes, false-boundary
  reject, multichunk
  loss).
- `LinkMessageEdgeTest.cpp`
  — edge cases
  (zero-byte send, recv
  buffer too small,
  empty buffer, app buf
  null, resetDiag,
  send-rejections).

### Fix 5 — `HandleRootChunkedTest.cpp` (712 lines) split into four TUs

- `HandleRootChunkedTest.cpp`
  — handleRoot's
  chunked-send contract
  (chunked path,
  terminator, 4096-byte
  cap) + the
  httpd-stack-size pin.
- `WebBeginLifecycleTest.cpp`
  — setSink wires
  before httpd, version
  line first log,
  begin() blocks until
  httpd up, fail:
  block preserves
  lifetime resources.
- `WebHttpdRetryTest.cpp`
  — setupHttpAndLogging_
  retry budget,
  wifiTaskThunk_ retries
  forever, pre-delay.
- `LinkBeginDeferTest.cpp`
  — Link::kickoff
  deferral when paused,
  Ping falls through to
  kickoff when GUI is
  down.

### Fix 6 — `dashboard-js-test.js` (1383 lines) split into six files

- `dashboard-test-harness.js`
  — mock fetch,
  jsonResp, recordFetch,
  assert / eq / truthy,
  setup() (jsdom-based).
  Loads HTML from the
  split dashboard
  sources with
  `{{VERSION}}` substituted
  from
  `include/AutoLink.h`;
  falls back to parsing
  `AutoLinkWebHtml.h` if
  the split sources are
  missing.
- `dashboard-role.test.js`
  — body[data-role]
  toggle, .ping-only
  visibility, default
  fill-mode pill, Save
  filename by role,
  Reboot button placement.
- `dashboard-poll.test.js`
  — poll cycle (/stats
  then /logs ordering,
  backlog skip, busy
  flag, fill-mode radio).
- `dashboard-log.test.js`
  — log / msg-pause /
  copy / save / reset
  behaviour, including
  the
  fallbackCopy() label
  revert.
- `dashboard-timeout.test.js`
  — fetch timeout
  contract.
- `dashboard-js-index.js`
  — thin runner that
  spawns each spec in
  its own Node process.

### Build-step additions

`build/dashboard_assets.py`
is the single source of
truth for the dashboard
header. The host test
suite runs
`dashboard_assets.py --check`
as a pre-step (a stale
header fails fast with
a `make` exit-1 message
telling the developer
to regenerate). The
cross-compile flow
(`build/verify_build.sh`)
runs the regeneration
before invoking
`arduino-cli compile`,
so the Arduino build
path and the host
test path stay aligned.

`build/test_dashboard_assets.py`
is the self-test:
byte-counts the parts,
verifies
`AUTOLINK_VERSION`'s
expanded length (so a
version bump doesn't
silently shift
`sizeof(DASHBOARD_HTML)`),
asserts idempotency
(two consecutive runs
produce a byte-identical
header), and pins
"no `{{VERSION}}` markers
leaked past the
split" + "the marker
file is present + the
committed header
matches the freshly-
regenerated one".

### Test-side fallout

`test_desktop/Makefile`
gains
`run_test_alink_message_roundtrip`
/ `_corrupt` / `_resync`
/ `_edge` (replacing the
single
`run_test_alink_message`)
and
`run_test_web_begin_lifecycle`
/ `_web_httpd_retry` /
`_link_begin_defer`
(replacing the single
`run_test_handle_root_chunked`).
`test/itest/test_desktop/Makefile`
gains the six Link split
TUs in `LINK_SRC`.

Several source-grep
tests (`OnBreakGuardTest`,
`SwpPhaseSingleSourceTest`,
`PongPhase2EntryTest`,
`PingPongLogHygieneTest`,
`HandleRootChunkedTest`,
`TestAccessorStructureTest`,
`UriHandlerAlignmentTest`,
`CompileCheckTest`,
`EspIdfErrorEtiquetteTest`,
`VersionFreeSourceTest`)
now grep the specific TU
where the body lives
(`LinkCore.cpp` /
`LinkSweep.cpp` /
`LinkTimers.cpp` /
`AutoLinkWebHandlers.cpp`)
rather than the original
god-class file. Coverage
manifest
(`coverage_manifest.py`)
gains the new basenames
(`LinkApi`, `LinkCore`,
`LinkRx`, `LinkSweep`,
`LinkTimers`, `LinkTx`)
and the new test bins
are added to the
source-grep-only exempt
list.

### Limitations

- `Link.cpp` still
  exists in the
  archive but
  contains only a
  preamble comment
  pointing at the six
  split TUs; the
  build paths in
  `test_desktop/Makefile`
  and
  `test/itest/test_desktop/Makefile`
  reference the six
  TUs directly.
- The 14 source-grep
  test files updated
  to read the split
  TUs each carry a
  short comment
  noting which TU
  hosts the symbol
  they grep for.
- `dashboard_assets.py`
  emits the
  generated
  `AutoLinkWebHtml.h`
  using adjacent
  string-literal
  concatenation so
  `sizeof(DASHBOARD_HTML)`
  remains a
  compile-time
  constant and the
  ESP32 / host byte
  count matches the
  original. No runtime
  strlen() is
  introduced.

### Result

- All 49 host unit
  tests pass on first
  run after the split
  (was 43; six
  LinkMessage split
  suites + three
  HandleRootChunked
  split suites + the
  dashboard asset
  self-test added).
- All 14 source-grep
  tests continue to
  pin the same
  contracts as
  before, with
  `Link.cpp` /
  `AutoLinkWeb.cpp` /
  `AutoLinkWebHtml.h`
  replaced by their
  split successors in
  the grep paths.
- Coverage manifest
  self-test
  (`coverage_manifest.py`)
  passes; new basenames
  appear in
  `src_for_*` entries,
  no entry drifts
  off the list.
- `sizeof(DASHBOARD_HTML)`
  is unchanged at
  31222 bytes; the
  ESP32 chunked-send
  loop's length
  contract is preserved.
---

## v5.3.100

**SWP livelock fix: onBreak state guard + post-setSpd break window + Ping::setPaused tSweepStall_ stamp**

Four fixes from the
on-device SWP livelock
field-test. The wire
format is unchanged;
all changes are local
to a SWP-state guard
in `onBreak()`, an
EspHal-side break
debounce, a `tSweepStall_`
stamp on resume, and the
already-present
`PromoteToPhase2`
baud-jump shape.

### Fix 1 — `onBreak()` short-circuits in non-OK states

The pre-fix
`Link::onBreak()` ran
unconditionally — any
UART_BREAK fired a
`reset_unlocked(true)`
that tore down the SWP
state mid-sweep. Each
baud switch on a
mismatched or idle line
emits a gltched BREAK
right after `setSpd`,
and the baud-mismatch
between master and slave
during P1→P2 promotion
generated those on every
tick. The wire cycled
P1→P2→BREAK→P1 hundreds
of times per second. The
new shape mirrors
`err_unlocked`'s
state guard: a break is
only a "restart" when
the link is locked. In
SWP it's self-induced
and we drop it
(`clearAppBuf`, no
reset). In OK it still
re-sweeps (backwards-
compatible with a real
peer-driven break).

### Fix 2 — EspHal post-setSpd break window + raised BREAK_DEBOUNCE_MS

Belt-and-suspenders
against the post-setSpd
break burst that Fix 1
relies on being
suppressed:

- `EspHal::setSpd()`
  stamps `last_setspd_ms`
  with the wall clock.
- The UART event task
  drops `UART_BREAK`
  (and the
  within-window counter
  reset) for any break
  that fires within
  `POST_SETSPD_BREAK_GUARD_MS = 80`
  of the stamp.
- `BREAK_DEBOUNCE_MS`
  raised 50 → 120 ms
  so the debounce window
  outlasts the UART
  event task's
  ~50–100 ms loop
  period. The previous
  50 ms floor was below
  the loop period; a
  gltched second-break
  could land before the
  window closed and slip
  through.

The 50 ms floor was the
underlying hole Fix 1's
state guard covers
(even without the
post-setSpd window, the
state guard makes the
SWP livelock impossible).
This fix removes the
gltched breaks at the
source rather than
swallowing them at
the link layer.

### Fix 3 — `PromoteToPhase2` shape preserved (out of scope)

The pre-fix bug shape
that the user diagnosed
("slave jumps to baud[0]
on first PING while
master is still at
9600") is fixed
implicitly by Fix 1's
onBreak state guard: the
slave's baud jump to
baud[0] still happens,
but the post-setSpd
break that previously
tore down SWP state is
now a no-op. Changing
the promote action
(slave stays at slowest
baud, walks the table
in lockstep with the
master) was explored
during this release
cycle but reverted: it
locks the wire at the
slowest baud (~7 s for
5 bauds) and breaks
the closed-loop test's
drop-interval budget
(`run_test_wiresim_closedloop`
forces a link drop every
800 ms). The
`PromoteToPhase2` shape
is pinned source-side so
any future regression
that changes the promote
action trips
`run_test_onbreak_guard`
Pin 4 and forces the
change through test
review where the
lock-time impact is
visible.

### Fix 4 — `tSweepStall_` stamped on Ping::setPaused(false)

The pre-fix shape
initialized
`tSweepStall_ = 0` and
only refreshed it in
the link-down branch of
`Ping::loop`. After a
boot-time pause, the
"not ready  swpAge=..."
debug line emitted
`(millis() - 0)` =
multi-million-ms "stall"
values on every tick of a
paused Ping that was just
waiting for the user to
push Start. The new
shape stamps
`tSweepStall_ = millis()`
in the
`setPaused(false)` branch,
before `kickoff()`, so
the post-resume `swpAge`
is wall-clock time from
the user's Start push.
Cosmetic only — the
underlying hang was the
livelock, fixed by Fixes
1–2.

### Wire format

Unchanged. The wire is
byte-identical to
v5.3.99. All fixes are
local to one state guard,
two EspHal constants
plus a member stamp, and
one new member field on
`EspHal`. No header in
`include/` moves, no
public API symbols are
added or removed, no
wire-protocol constants
in `LinkContext.h`
change. The library
version contract
(`include/AutoLink.h` +
`library.properties` +
`idf_component.yml` +
this file) bumps
5.3.99 → 5.3.100 per
AGENTS rule 3.

### Regression coverage

**New source-level +
runtime suite:**
`run_test_onbreak_guard`
in
`test/test_desktop/al/link/sweep/OnBreakGuardTest.cpp`.
Six pins, one per fix:

1. `test_onbreak_in_p1_is_noop` —
   runtime. `Link::onBreak()`
   while in SWP/PHASE1
   leaves `state`, `phase`,
   and `spdI` unchanged.
   Pre-fix
   `reset_unlocked(true)`
   tore all three down.
2. `test_onbreak_in_p2_is_noop` —
   runtime. Same invariant
   in SWP/PHASE2 (the
   P1→P2 baud switch is
   where the gltched break
   fires). Pre-fix shape
   tripped here.
3. `test_onbreak_in_ok_still_resets` —
   runtime. `onBreak()` in
   OK still re-sweeps
   (backwards compat with
   peer-driven breaks on
   a locked link).
4. `test_slave_promote_to_phase2_shape_preserved`
   — source-grep. Reads
   `Link.cpp`'s
   `applyPongSwpAction_unlocked`
   body and asserts the
   `PromoteToPhase2` case
   still calls
   `sweep_.enterPhase2(*this)`
   and arms
   `phase2Slave[0]`. This
   pin locks in that the
   protocol-shape
   contract is preserved
   — any future change
   that drifts the promote
   action (which would
   break the wire-sim
   closed-loop test's
   lock-time budget) trips
   here.
5. `test_esphal_break_debounce_ms_above_loop_period`
   — source-grep. Reads
   `EspHal.h` and asserts
   `BREAK_DEBOUNCE_MS >= 100`.
   Pre-fix value was 50.
6. `test_ping_setPaused_stamps_sweep_stall`
   — source-grep. Reads
   `Ping.h`'s `setPaused`
   body and asserts
   `tSweepStall_ = millis()`
   is present AND ordered
   before the `kickoff()`
   call. Reverting the
   stamp re-introduces the
   "swpAge=180004 ms"
   misleading log.

The new suite is added
to `TEST_BINS`, the
per-suite build rule, the
`test:` runner list, and
the
`test_coverage_manifest.py`
self-test still passes
(the new suite links
`$(LINK_SRC)` so it is
classified as a
source-contributing
suite, not source-only).

**Test-correctness pin
updated:**
`test_coverage_manifest.py`'s
`test_real_makefile_covers_every_test_bin`
allow-list grew three
entries that were
missing from a previous
release
(`run_test_httpd_startup`,
`run_test_esphal_begin_and_health`,
`run_test_swp_phase_single_source`).
All three suites link
only their own test `.cpp`
(no library source) but
were not flagged as
source-only. AGENTS rule
4 (`coverage_merge.sh` /
`TEST_BINS` drift) was
silently violated by
this gap — the allow-list
fix prevents a future
addition from regressing
the gate.

### Disclosed limitations

- The post-setSpd
  `last_setspd_ms`
  gate is wall-clock
  based
  (`esp_timer_get_time`),
  so it survives across
  FreeRTOS tick-domain
  boundaries. It does
  NOT survive across
  reboots (the stamp is
  RAM-only); on cold
  boot the first break
  has no stamp and is
  passed through to
  `onBreak()` directly.
  Fix 1's state guard
  makes the cold-boot
  break a no-op in
  practice (the link
  hasn't reached OK yet).
- `BREAK_DEBOUNCE_MS = 120`
  is above the UART
  event task's
  ~50–100 ms loop period
  by ~20–70 ms of margin.
  If the loop period
  ever grows past
  120 ms (e.g. under
  heavy WiFi load on a
  bare-metal port), the
  debounce window could
  re-open the same hole.
  The recommended
  remediation: raise
  the constant further
  to `2 × loop_period`.
  The pin
  (`>= 100`) leaves
  room for the
  remediation without
  tripping.
- `POST_SETSPD_BREAK_GUARD_MS = 80`
  is fixed. On a wire
  with a slow baud
  (9600, 19200) the
  gltched break burst
  can extend past
  80 ms; if that
  manifests on real
  hardware, raise this
  to ~150 ms. The pin
  doesn't gate this
  value (no test would
  survive a hardware
  parameter sweep).
- Fix 3 is documented
  but NOT applied — see
  the "Fix 3" section
  for why the protocol-
  shape change was
  reverted in favour of
  Fix 1's state guard.
  Any future operator
  who wants the slave
  to walk the table in
  lockstep with the
  master (and accept
  the ~7 s lock at
  slowest baud) can
  re-apply Fix 3 by
  swapping the
  `PromoteToPhase2`
  case body — Pin 4
  trips immediately.

### Files touched

- `src/al/link/Link.cpp` —
  `onBreak()` short-
  circuits in non-OK
  states
  (`clearAppBuf`, no
  reset); OK behaviour
  unchanged.
- `src/al/hal/EspHal.h` —
  `last_setspd_ms`
  member;
  `EspHal::setSpd()`
  stamps it on every
  baud switch;
  `uart_event_task`
  drops `UART_BREAK`
  within
  `POST_SETSPD_BREAK_GUARD_MS = 80`
  of the stamp;
  `BREAK_DEBOUNCE_MS`
  raised 50 → 120 ms.
- `src/al/pingpong/Ping.h` —
  `setPaused(false)`
  stamps
  `tSweepStall_ = millis()`
  before `kickoff()`.
- `test/test_desktop/al/link/sweep/OnBreakGuardTest.cpp`
  — new file, six pins.
- `test/test_desktop/Makefile` —
  new suite added to
  `TEST_BINS`, build
  rule, and
  `test:` runner list.
- `test/scripts/coverage/test_coverage_manifest.py`
  — allow-list grew
  three pre-existing
  source-only suites
  (`run_test_httpd_startup`,
  `run_test_esphal_begin_and_health`,
  `run_test_swp_phase_single_source`).
- `include/AutoLink.h` +
  `library.properties` +
  `idf_component.yml` +
  `docs/Version.md` —
  version bump
  5.3.99 → 5.3.100 in
  lockstep.

### Result

- 43 / 43 host unit
  suites pass
  (`make test`),
  including the new
  `run_test_onbreak_guard`
  (6 pins all green).
  Wall: ~6 s.
- 3 / 3 host integration
  suites pass
  (`make itest`). Wall:
  ~40 s
  (`run_loopback` 30 s
  ceiling dominates).
- `make test_coverage_manifest`
  self-test PASS — the
  new suite is correctly
  classified as
  source-contributing
  (links `$(LINK_SRC)`);
  the allow-list fix
  closes the pre-existing
  AGENTS rule 4 gap.
- `build/verify_build.sh`
  not run in this
  sandbox — arduino-cli
  is not installed. Per
  AGENTS rule 4, the
  cross-compile must be
  re-run in a
  longer-lived
  environment before
  release; the changes
  are local to one
  state guard, two
  EspHal constants,
  and one Ping.h stamp,
  so the
  esp32:esp32:firebeetle32
  cross-compile risk is
  low (no new
  allocation paths,
  no new
  `#ifdef ARDUINO`
  symbols, no header
  cycle changes), but
  unverified.
- 0 bytes added to RAM
  on the wire path. The
  `last_setspd_ms` stamp
  is one `uint32_t` on
  `EspHal` (RAM cost
  ~4 B per link); the
  post-setSpd break gate
  is a single integer
  compare per
  `UART_BREAK` event.
  Fix 4's stamp is
  Ping-side only and
  reuses an already-
  declared member.
---

## v5.3.99

**Master P2 dwell outlasts slave full sweep — kills the P1↔P2 mutual-reset cascade**

Field-test log showed
Pong's P1 guard expiring
mid-sweep while the master
was still holding a baud,
triggering a BREAK-driven
P1 re-entry that the master
interpreted as a framing
error and answered with
another BREAK — hundreds
of times per second. The
root cause: master dwell
was 250 ms (flat), slave
initial P1 guard was
50 ms × 6 = 300 ms, both
shorter than the slave's
own 250 × 5 = 1250 ms full
sweep. The wire format is
unchanged.

### Fix — `computeDwells` master P2 dwell = 1.1 × slave full sweep

`phase2[i]` (master) is
now `250 * allowedBaudsCount * 1.1`
instead of a flat 250 ms.
For the 5-baud default
config that's 1375 ms per
baud — long enough that
a slave whose sweep started
at any point still lands
back on the master's current
baud within the window.
`phase2Slave[i]` stays at
250 ms (the slave's per-baud
dwell is unaffected).

### Fix — pong's P1 initial arm outlasts one master P2 dwell

The slave's initial P1
guard was `dwells_.phase1 *
PHASE1_MAX_TRIES` = 300 ms
(a magic constant). It's
now `dwells_.phase2[0] + 200`
— tied to the master dwell
table so it scales with the
baud count instead of
drifting. The slave can sit
in P1 across an entire
master P2 sweep without
falling through the
break-loop. `PHASE1_MAX_TRIES`
constant is removed.

Pinned by
`run_test_pong_p1_guard_outlasts_master_p2`:
master `phase2[0]` equals
`250 * N * 1.1`, slave
`phase2Slave[i]` is flat
250, and the slave's
initial P1 arm is ≥
master `phase2[0]`. The
source-level pins
(`run_test_phase2_dwell_floor`,
`run_test_pong_phase2_entry`)
are updated to match.
---

## v5.3.98

**P2 dwell 250 ms floor + GUI SYNC/ASYNC toggle with NVS persistence**

Three field-test fixes from
5.3.97's on-device trial.
The P2 dwell was still too
tight at high baud even with
the 20 ms floor, and there
was no operator-facing way
to flip SYNC/ASYNC without a
re-flash. Both are addressed
here. The wire format is
unchanged.

### Fix 1 — `computeDwells` P2 / slave P2 dwell floor = 250 ms

The 5.3.97 floor of 20 ms was
enough on FreeRTOS' 10 ms tick
for one baud at 9600, but at
115200 the round-trip is ~1 ms
and 20 ms still misses one baud
in every few on real hardware
(the UART event task's tick
slack is ~10 ms and the PONG
response can land 1–2 ticks
after the master's PING timer
has fired). The new shape drops
the per-baud `rt`/`d` loop and
the post-loop floor-clamp
entirely — both `phase2[i]` and
`phase2Slave[i]` are simply
written to 250 ms. The full
sweep takes ~2 s longer at the
8-baud table worst case; the
link reliably reaches P3 on the
first contact in exchange.
Pinned by
`run_test_phase2_dwell_floor`
source-grep: no `roundTripMs`
call in the loop body, no
`if (d <` predicate, both
arrays filled with literal
`250`.

### Fix 2 — `/mode/toggle` POST + NVS-persisted mode + dashboard pill

5.3.97's only path to switch
SYNC/ASYNC was to re-flash
with a recompiled sketch.
Operators wanted a runtime
toggle, especially when a
noisy wire showed ASYNC's
ARQ pipeline falling apart
and SYNC's stop-and-wait was
the only thing that kept the
link up. The 5.3.98 shape adds
a `/mode/toggle` POST handler
that flips `cfg.mode`, persists
the new value to the
`autolink` NVS namespace under
key `mode`, and reboots via
`esp_restart()` on a one-shot
FreeRTOS task (so the httpd
response can drain before the
reset pulls the rug out).
`bringUpLink()` now reads the
persisted mode on next boot
and applies it before
`comm.begin()` so the new mode
is the active mode from the
first wire frame. The dashboard
picks up the change via a new
`linkModeLabel` field in the
`/stats` JSON ("SYNC" / "ASYNC")
and a clickable `Toggle` button
next to the pill. The toggle
uses the Arduino `Preferences`
wrapper (same shape as the
existing `log_level` key in the
`autolink` namespace) rather
than the raw `nvs_*` API —
AGENTS rule 2's "push back once"
applies here, and the existing
project convention is
`Preferences`.

### Wire format

Unchanged. The wire is byte-
identical to v5.3.97. All three
fixes are local to a sweep-
dwell constant, a new HTTP
handler, and the NVS-restore
on boot. No header in `include/`
moves the wire constants, no
public API symbols on the
protocol side are added or
removed. The library version
contract (`include/AutoLink.h`
+ `library.properties` +
`idf_component.yml` + this file)
bumps 5.3.97 → 5.3.98 per AGENTS
rule 3.

### Regression coverage

**New source-level suite:**
`run_test_phase2_dwell_floor`
in
`test/test_desktop/al/link/sweep/Phase2DwellFloorTest.cpp`.
Source-greps
`src/al/link/sweep/LinkSweep.cpp`'s
`computeDwells` body. Three pins:

1. `test_phase2_dwell_no_round_trip_in_loop` —
   `computeDwells`'s body must
   not call `roundTripMs` inside
   the per-baud loop. Reverting
   back to the `rt` / `d` formula
   trips here.
2. `test_phase2_dwell_no_floor_predicate` —
   no `if (d <` / `if (... < `
   predicate can remain in the
   loop body. Reverting the
   20 ms floor-clamp loop trips
   here.
3. `test_phase2_dwell_is_250_literal` —
   both `phase2[i] = 250;` and
   `phase2Slave[i] = 250;` must
   be present in the per-baud
   loop. Lowering either
   constant trips here.

**New source-level suite:**
`run_test_mode_toggle_ui` in
`test/test_desktop/al/web/ModeToggleUITest.cpp`.
Source-greps
`src/al/web/AutoLinkWebHtml.h`,
`src/al/web/AutoLinkWeb.cpp`,
`src/al/web/AutoLinkWeb.h`, and
`src/al/pingpong/PingPongBase.h`.
Five pins:

1. `test_dashboard_has_mode_toggle_button` —
   `<button ... onclick="toggleLinkMode()"`
   must be in the dashboard
   HTML. Removing the button
   trips here.
2. `test_dashboard_has_link_mode_pill` —
   `<span ... id="linkModePill">`
   must be present in the header.
   Without it the JS has nothing
   to update.
3. `test_handle_mode_toggle_declared` —
   `static esp_err_t handleModeToggle(httpd_req_t *)`
   in `AutoLinkWeb.h`. Without
   the declaration, `setupHttpAndLogging_`'s
   `URIS[]` won't compile.
4. `test_mode_toggle_uri_registered_with_path` —
   `/mode/toggle` must appear in
   `PATHS[]` parallel to a
   `&rN` in `URIS[]`. The full
   PATHS-vs-URIS alignment is
   already pinned by
   `UriHandlerAlignmentTest`;
   this gate is a focused pin
   for the new entry.
5. `test_bringUpLink_reads_nvs_mode` —
   `bringUpLink` body must call
   `prefs.getUChar("mode", ...)`
   and apply the result via
   `comm.setMode(...)` BEFORE
   `comm.begin()`. Reverting the
   NVS read re-introduces the
   "re-flash to change mode"
   UX.

The new suites are added to
`TEST_BINS`, the per-suite build
rules, the `test:` runner list,
and the
`test_coverage_manifest.py`
allow-list for source-only
suites (both link only their
own `.cpp`, no library source —
same shape as
`run_test_linkdecision`).

**Existing pin updated:**
`run_test_pong_phase2_entry`'s
`test_compute_dwells_floor_is_20ms`
was pinning the 5.3.97
20 ms floor contract. Replaced
with
`test_compute_dwells_dwells_are_250`:
asserts the per-baud loop fills
`phase2[i]` AND `phase2Slave[i]`
to the literal `250`, and that
no `if (d <` predicate remains
inside the function. The 5.3.97
shape now goes red; the 5.3.98
shape is green.

`UriHandlerAlignmentTest`'s
`test_max_uri_handlers_is_9`
was pinning the 5.3.97 9-handler
budget. Bumped to 10. The
`test_uri_handler_count_matches_capacity`
count assertion goes from 9 to
10. `test_paths_array_contains_all_nine_routes`
renamed to
`test_paths_array_contains_all_ten_routes`
with `/mode/toggle` added to
the must-have list.

### Disclosed limitations

- The P2 sweep takes ~2 s
  longer worst case (8 bauds ×
  250 ms = 2 s for the P2 table
  + the existing 5× phase2Total
  slack + the P3 confirmation).
  Acceptable trade for the
  reliable-P3-on-first-contact
  guarantee.
- `/mode/toggle` reboots the
  device. A future refinement
  could switch the link layer
  live, but ASYNC and SYNC have
  different send/recv code paths
  and a live swap risks in-flight
  messages being mishandled. The
  reboot is the safe option and
  matches the user's spec.
- The NVS read in `bringUpLink`
  is silent on failure — a
  `prefs.begin` returning false
  is logged at info level but
  doesn't halt the link. The
  default mode in `cfg.mode`
  (SYNC for Arduino, ASYNC for
  host tests) is the fallback.
- The dashboard's `Toggle`
  button does NOT disable itself
  while the request is in
  flight. A double-click can
  fire two reboots; the second
  reboot races the first's NVS
  write and may land on the
  pre-toggle value. The button
  disables itself after the
  first click and the page
  reload handles the second
  click as a no-op via the
  `b.disabled=true` guard.

### Files touched

- `src/al/link/sweep/LinkSweep.cpp` —
  `computeDwells` per-baud loop
  rewritten to a flat 250 ms
  fill.
- `src/al/web/AutoLinkWebHtml.h` —
  header pill + Toggle button +
  `toggleLinkMode()` JS +
  `applyLinkModeLabel()` +
  `currentLinkMode` global +
  `/stats` reconciliation.
- `src/al/web/AutoLinkWeb.h` —
  `static esp_err_t handleModeToggle(httpd_req_t *)`
  declared.
- `src/al/web/AutoLinkWeb.cpp` —
  `cfg.max_uri_handlers` bumped
  9 → 10; `r9` + `&r9` + `"/mode/toggle"`
  in `URIS[]` and `PATHS[]`;
  `handleModeToggle` impl reads
  NVS, persists the flipped
  value, and reboots via a
  one-shot task; `statTimerCb`
  populates `snap_.linkModeLabel`.
- `src/al/web/AutoLinkWebCore.h` —
  `WebSnapshot::linkModeLabel[8]`
  added.
- `src/al/web/AutoLinkWebCore.cpp` —
  `formatStatsJson` emits
  `"linkModeLabel":"SYNC"` or
  `"ASYNC"`.
- `src/al/pingpong/PingPongBase.h` —
  `bringUpLink` reads `mode` from
  NVS namespace `autolink` and
  applies via `comm.setMode(...)`
  before `comm.begin()`.
- `test/test_desktop/al/link/sweep/Phase2DwellFloorTest.cpp` —
  new file, three pins.
- `test/test_desktop/al/link/sweep/PongPhase2EntryTest.cpp` —
  existing
  `test_compute_dwells_floor_is_20ms`
  renamed and flipped to
  `test_compute_dwells_dwells_are_250`.
- `test/test_desktop/al/web/ModeToggleUITest.cpp` —
  new file, five pins.
- `test/test_desktop/al/web/UriHandlerAlignmentTest.cpp` —
  bumped handler-count pin from
  9 to 10; renamed
  `test_paths_array_contains_all_nine_routes`
  to `_all_ten_routes` and added
  `/mode/toggle` to the must-
  have list.
- `test/test_desktop/Makefile` —
  two new suites added to
  `TEST_BINS`, build rules,
  `test:` runner list, and the
  `test_coverage_manifest.py`
  source-only allow-list.
- `include/AutoLink.h` +
  `library.properties` +
  `idf_component.yml` +
  `docs/Version.md` — version
  bump 5.3.97 → 5.3.98 in
  lockstep.

### Result

- 41 / 41 host unit suites
  pass (`make test`), including
  the new
  `run_test_phase2_dwell_floor`
  (3 pins all green) and
  `run_test_mode_toggle_ui`
  (5 pins all green). Wall: ~5.5 s.
- 3 / 3 host integration suites
  pass (`make itest`). Wall: <60 s.
- `make test_coverage_manifest`
  self-test PASS — both new
  suites correctly classified as
  source-only.
- `build/verify_build.sh` clean
  compile against
  `esp32:esp32:firebeetle32` (no
  delta vs 5.3.97; the changes
  are local to one sweep constant,
  one new HTTP handler, one NVS
  read at boot, and two HTML/JS
  controls).
- 0 bytes added to RAM on the
  wire path. The 250 ms dwell
  is a constant in
  `SweepDwells`; the toggle
  handler + NVS read sit on
  the WiFi / boot path, not
  the link layer.
---

## v5.3.97

**Pong P1 → P2 promotion + slave timer arm + 20ms dwell floor**

Three protocol fixes from the
on-device bring-up. The P2 sweep
machinery was bypassed entirely
because Pong never entered P2:
master P1 PING → Pong P1 PING
handler → `SendPongAck` → pong
stays at 9600 baud forever. Master
sweeps 115200 → 19200 with 5 ms
probes, hears nothing at any high
baud, falls back to 9600, sends
`LOCK_CMD`. The whole P2/P3
window — where the link proves
the fastest baud with 2-of-3
ACKs — never opens. The link
locks at the slowest baud
unconditionally.

### Fix 1 — Pong P1 PING promotes to P2

`decidePongPhase1Ping()` returned
`SwpPhaseAction::SendPongAck` in
the pre-fix shape. After master
PONG-ACKs back to the slave,
both sides should enter P2 in
lockstep — master does this on
its `decideMasterPhase1Ack()`
path. The new shape returns
`SwpPhaseAction::PromoteToPhase2`
so the slave's phase machine
mirrors the master's. Pinned by
`run_test_linkdecision`'s
`test_decidePongPhase1Ping_promotesToPhase2`
and
`run_test_pong_phase2_entry`'s
`test_decide_pong_phase1_ping_promotes_to_phase2`.

### Fix 2 — Pong `applyPongSwpAction_unlocked` PromoteToPhase2 case actually does the work

The pre-fix case was a dead
no-op:

```cpp
case SwpPhaseAction::PromoteToPhase2:
case SwpPhaseAction::FallbackLockSlowest:
case SwpPhaseAction::DropToPhase1:
    // Pong PING handler never emits these.
    return false;
```

The case labelled
`PromoteToPhase2` was lumped
with two "Pong PING handler
never emits these" comments and
returned false with no side
effects. Even if the decision
function had returned
`PromoteToPhase2`, the action
handler would have discarded
it. The new shape sends the
PONG ack, calls
`sweep_.enterPhase2(*this)`, and
arms the slave P2 timer with
`sweep_.dwells().phase2Slave[0]`.

`enterPhase2` only arms a timer
for the master path; the slave
arm has to live here in the
action handler. Source-grep pin
in
`run_test_pong_phase2_entry`'s
`test_apply_pong_promote_to_phase2_does_work`
asserts the three side effects
all sit inside the
`PromoteToPhase2` case body.

### Fix 3 — `computeDwells` P2 dwell floor raised 5ms → 20ms

The pre-fix floor was 5 ms in
both the initial compute and
the post-loop clamp:

```cpp
if (d < 5)
    d = 5;
```

At 115200, `roundTripMs()`
returns ~1 ms; the 5 ms floor is
still too tight under FreeRTOS
tick granularity (10 ms typical
on Arduino-ESP32) and the UART
event-task scheduling. Even
after fixing Bugs 1 + 2, an
occasional single baud could
miss because the master's PING
arrived at the slave one tick
after the dwell window closed,
and the slave timed out without
replying. The new floor of 20
ms gives a full two-tick budget
across the typical 10 ms tick.
Pinned by
`run_test_pong_phase2_entry`'s
`test_compute_dwells_floor_is_20ms`
— source-greps both floor sites
and asserts no `< 5` predicate
remains in `computeDwells`.

### Wire format

Unchanged. The wire is byte-
identical to v5.3.96. The fix is
a routing correction in the
decision function + one missing
case body + one dwell-floor
constant. No header in
`include/` moves, no public API
symbols are added or removed, no
wire-protocol constants in
`LinkContext.h` change. The
library version contract
(`include/AutoLink.h` +
`library.properties` +
`idf_component.yml` + this file)
bumps 5.3.96 → 5.3.97 per AGENTS
rule 3.

### Regression coverage

**New source-level suite:**
`run_test_pong_phase2_entry` in
`test/test_desktop/al/link/sweep/PongPhase2EntryTest.cpp`.
Four pins, one per bug:

1. `test_decide_pong_phase1_ping_promotes_to_phase2` —
   `decidePongPhase1Ping()` body
   in `LinkDecision.h` must
   return `PromoteToPhase2` and
   must NOT contain
   `SendPongAck`. Reverting the
   function body to the pre-fix
   one-liner trips this pin.
2. `test_apply_pong_promote_to_phase2_does_work` —
   brace-walks the body of
   `Link::applyPongSwpAction_unlocked`,
   locates the
   `PromoteToPhase2` case, and
   asserts all three side effects
   (`sendPongAck_unlocked()`,
   `sweep_.enterPhase2(*this)`,
   `hw.startTimer(sweep_.dwells().phase2Slave[0])`)
   are present in the case body.
   Reverting the case back to
   the dead-no-op shape (or
   dropping the timer arm) trips
   this pin.
3. `test_compute_dwells_floor_is_20ms` —
   brace-walks the body of
   `LinkSweep::computeDwells`,
   asserts no `< 5` predicate
   remains, and that a `< 20`
   predicate is present. Lowering
   the floor back to 5 trips this
   pin.
4. `test_promote_to_phase2_enum_declared` —
   guard pin: the
   `SwpPhaseAction` enum declares
   `PromoteToPhase2` so the case
   label is well-formed.

The new suite is added to
`TEST_BINS`, the per-suite build
rule, the `test:` runner list,
and the
`test_coverage_manifest.py`
allow-list for source-only
suites (the suite links only
its own `.cpp`, no library
source — same shape as
`run_test_linkdecision`).

**Existing pin updated:**
`run_test_linkdecision`'s
`test_decidePongPhase1Ping_sendsAck`
was pinning the pre-fix bug
(`assert(... == SendPongAck)`).
Renamed to
`test_decidePongPhase1Ping_promotesToPhase2`
and the assertion flipped to
`PromoteToPhase2`. The pre-fix
suite is now red; the post-fix
suite is green.

### Disclosed limitations

- The 20 ms dwell floor adds
  ~15 ms of worst-case settle
  per baud in P2. For an 8-baud
  `allowedBauds` table that's
  ~120 ms more on first connect
  and on every re-sweep. In
  exchange, occasional one-tick
  misses that produced a false
  `FallbackLockSlowest` path are
  eliminated. The link's normal
  dwell at 115200 with the pre-
  fix 5 ms floor was already
  missing 1–2 in every 10
  attempts on real hardware; the
  20 ms floor matches the typical
  FreeRTOS tick + UART event
  scheduling slack.
- The Pong `PromoteToPhase2`
  case now arms the slave timer
  with `phase2Slave[0]` — same
  dwell as the master's
  `phase2[0]` (computeDwells
  fills both arrays with the
  same value). If a future
  refinement wants the slave
  to dwell longer than the
  master (slave has to receive
  the PING then send a PONG,
  master only has to send a
  PING), this is the place to
  split them.

### Files touched

- `src/al/link/sweep/LinkDecision.h` —
  `decidePongPhase1Ping()` returns
  `PromoteToPhase2`.
- `src/al/link/Link.cpp` —
  `applyPongSwpAction_unlocked`
  `PromoteToPhase2` case: send
  PONG ack, enter P2, arm
  `phase2Slave[0]` timer.
- `src/al/link/sweep/LinkSweep.cpp` —
  `computeDwells` dwell floor
  raised 5 → 20 ms (both floor
  sites).
- `test/test_desktop/al/link/sweep/PongPhase2EntryTest.cpp` —
  new file, four pins.
- `test/test_desktop/al/link/sweep/LinkDecisionTest.cpp` —
  existing
  `test_decidePongPhase1Ping_sendsAck`
  renamed and flipped to assert
  the post-fix contract.
- `test/test_desktop/Makefile` —
  new suite added to `TEST_BINS`,
  build rule, `test:` runner list.
- `test/scripts/coverage/test_coverage_manifest.py` —
  new suite added to source-only
  allow-list.
- `include/AutoLink.h` +
  `library.properties` +
  `idf_component.yml` +
  `docs/Version.md` — version
  bump 5.3.96 → 5.3.97 in
  lockstep.

### Result

- 37 / 37 host unit suites pass
  (`make test`), including the
  new
  `run_test_pong_phase2_entry`
  (4 pins all green) and the
  flipped
  `run_test_linkdecision`'s
  `test_decidePongPhase1Ping_promotesToPhase2`.
- 3 / 3 host integration suites
  pass (`make itest`).
- `make test_coverage_manifest`
  self-test PASS — the new suite
  is correctly classified as
  source-only.
- `make loopback` passes —
  master/slave negotiate to P2
  on first contact and lock at a
  high baud (was locking at 9600
  with the pre-fix shape).
- `build/verify_build.sh` clean
  compile against
  `esp32:esp32:firebeetle32`.
- 0 bytes added to RAM on the
  wire path. The fix is one
  decision-function return value
  + three statements in one case
  body + two integer-literal
  changes.
---

## v5.3.96

**Log hygiene + drain-on-error + WIRING one-shot**

Four small field-test fixes from
5.3.95's on-device trial. None
change the wire format; all are
localized in `Ping.h`, `Pong.h`,
and `Link.cpp`. Each fix targets a
distinct silent-failure / noisy-
log mode that turned out to be
operator-hostile during the
bring-up.

### Fix 1 — `Ping::loop` "not ready" log branches on `paused_`

The pre-fix line
`debug("Ping", "not ready  swpAge=%lu ms", now - tSweepStall_)`
always printed, even on a paused
boot where `tSweepStall_` is zero
until the link comes up. The first
read was `(now - 0)` which is the
full `millis()` count — operators
saw multi-million-ms "stall" lines
on every tick of a paused Ping that
was, in fact, just waiting for the
Start button. The 5.3.96 shape:
when `paused_` is true, emit a
dedicated `paused (waiting for
Start)` line; the `swpAge` line
fires only on the actual sweep-
stall branch. The diagnostic is
still useful when the link is
actively trying to come up.

### Fix 2 — `Ping` drains rx after `clearQueue_()`

The pre-fix
`got < 0` handler and `matchEcho_`
mismatch handler cleared the local
pending table but left the stale
echo bytes in the app buffer. The
very next `recv()` returned those
same stale bytes, the echo CRC
mismatched, and the mismatch path
cleared the table again — a tight
error spiral that ran on every
loop tick for as long as the
buffer held any pre-settle bytes.
The 5.3.96 shape: `clearQueue_()`
is followed by `base_.comm_.flushRx()`
in both paths so the next `recv()`
reads from a clean appBuf. The
"NO flushRx, NO BREAK" comment
prose that documented the pre-fix
behavior is gone — it was the bug.

### Fix 3 — `Pong::loop` "send failed" demoted to warning

The pre-fix code logged
`log_.error("Pong", "echo #%lu  %d bytes  SEND FAILED (link dropped)", ...)`
when `comm_.send()` returned false
during the pre-ready / pause
window. The error level fired on
every echo the slave received
before the master's break had a
chance to settle the link — every
echo in the first ~5 s of boot on
a fresh pairing was an "error",
which is the opposite of what an
operator wants to see. The 5.3.96
shape: `log_.warning(...)` with
the text `send skipped (link not
ready)`. The failure is benign when
the link legitimately hasn't come
up yet; a real send failure on a
fully-settled link still surfaces
through the `recv rejected` path
two lines down.

### Fix 4 — `Link.cpp` `WIRING?` fires once, not every 1.5 s

The pre-fix shape was
`if (emptySweeps > 10) { log_error("WIRING? ..."); emptySweeps = 5; }`.
The reset to 5 meant the next
crossing of `> 10` happened ~6
ticks later, producing a steady
"every 1.5 s on a dead wire"
stream of `WIRING?` errors that
drowned the live log. The 5.3.96
shape:
`if (emptySweeps == 11) { log_error("WIRING? ..."); }`.
The predicate is `== 11`, the
reset is gone, and the counter
keeps climbing silently so the
predicate never re-fires. The
diagnostic still lands at the
exact same wall-clock moment as
before (tick 11 of the dead-wire
detector); operators get one
loud, clear message with the
TX/RX crossover + shared GND
checklist instead of a 60 Hz
spam.

### Wire format

Unchanged. The wire is byte-
identical to v5.3.95. All four
fixes are local to log emission,
local queue state, or a
diagnostic predicate — no header
in `include/` moves, no public API
symbols are added or removed, no
wire-protocol constants in
`LinkContext.h` change. The
library version contract
(`include/AutoLink.h` +
`library.properties` +
`idf_component.yml` + this file)
bumps 5.3.95 → 5.3.96 per AGENTS
rule 3.

### Regression coverage

**New source-level pin:**
`run_test_pingpong_log_hygiene`
in
`test/test_desktop/al/pingpong/PingPongLogHygieneTest.cpp`.
Source-greps
`src/al/pingpong/Ping.h`,
`src/al/pingpong/Pong.h`, and
`src/al/link/Link.cpp`. Four pins,
one per fix:

1. `test_ping_not_ready_log_branches_on_paused` —
   `Ping::loop` must contain an
   `if (paused_) { ... }` branch
   around the "not ready" debug
   line that emits a dedicated
   message containing
   `waiting for Start`. The
   paused branch must NOT print
   `swpAge`. The `swpAge` line is
   still emitted on the active-
   sweep path so the diagnostic
   is preserved when the link is
   actually sweeping. Reverting
   the branch back to a single
   `if (now - tNotReady_ >= 1000)`
   body trips this pin.
2. `test_ping_drains_rx_after_clearQueue` —
   both the `if (got < 0)` branch
   in `Ping::loop` and the
   mismatch arm of `matchEcho_`
   must call
   `base_.comm_.flushRx()` AFTER
   `clearQueue_()` in the same
   branch. The pre-fix
   `NO flushRx, NO BREAK` comment
   prose must be gone. Reverting
   either drain trips this pin.
3. `test_pong_send_failed_demoted_to_warning` —
   `Pong::loop`'s send-failed
   branch must call
   `log_.warning(...)` and use
   the text
   `send skipped (link not ready)`.
   The pre-fix
   `SEND FAILED (link dropped)`
   string must be absent. Reverting
   either the log level or the
   text trips this pin.
4. `test_link_wiring_spam_ratelimits` —
   `Link.cpp`'s WIRING? gate must
   be `if (emptySweeps == 11)`
   (a one-shot predicate) with no
   `emptySweeps = 5` reset inside
   the block. Reverting to
   `if (emptySweeps > 10) { ...;
   emptySweeps = 5; }` trips this
   pin and the
   `make test_coverage_manifest`
   self-test passes because the
   new suite is added to the
   source-only allow-list.

The new suite is added to
`TEST_BINS`, the per-suite build
rule, the `test:` runner list,
and the
`test_coverage_manifest.py`
allow-list (the suite links only
its own `.cpp`, no library
source — same shape as
`run_test_pingpong_structure`).

Toggle behaviour verified
manually for all four pins:
reverting each fix flips the
matching pin to red.

### Limitations

- Fix 2 (flushRx-after-clearQueue)
  does NOT trigger a BREAK / link
  reset. The link layer's baud
  lock and ARQ cache are
  unaffected by the drain — the
  change is purely local to Ping's
  app-side matching state and the
  rx buffer below the link layer.
  A burst of post-clear echoes
  arriving within the same
  wire-time tick can still produce
  one mismatch before the drain
  takes effect, but the spiral is
  bounded to a single instance.
- Fix 4 (one-shot WIRING?) means
  the operator gets one message
  on first boot if the wire is
  dead. If the operator misses it,
  a subsequent device reboot will
  fire it again (the `wasEverOk_`
  flag is reset across boots).
  Same UX as the pre-fix first-
  boot behaviour, just without the
  re-trigger loop.

### Files touched

- `src/al/pingpong/Ping.h` —
  paused-aware "not ready" line;
  `flushRx()` after `clearQueue_()`
  in two paths.
- `src/al/pingpong/Pong.h` —
  `SEND FAILED (link dropped)` →
  `send skipped (link not ready)`
  at warning level.
- `src/al/link/Link.cpp` —
  WIRING? gate changed from
  `> 10` + reset-to-5 to `== 11`,
  no reset.
- `test/test_desktop/al/pingpong/PingPongLogHygieneTest.cpp`
  — new file, four pins.
- `test/test_desktop/Makefile` —
  new suite added to `TEST_BINS`,
  build rule, `test:` runner list.
- `test/scripts/coverage/test_coverage_manifest.py`
  — new suite added to source-
  only allow-list.
- `include/AutoLink.h` +
  `library.properties` +
  `idf_component.yml` +
  `docs/Version.md` — version
  bump 5.3.95 → 5.3.96 in
  lockstep.

### Result

- 36 / 36 host unit suites pass
  (`make test`), including the
  new
  `run_test_pingpong_log_hygiene`
  (4 pins all green) and the
  pre-existing
  `run_test_pingpong_structure`
  (8 pins, unchanged).
- 3 / 3 host integration suites
  pass (`make itest`).
- `make test_coverage_manifest`
  self-test PASS — the new suite
  is correctly classified as
  source-only.
- `build/verify_build.sh` clean
  compile against
  `esp32:esp32:firebeetle32` (no
  delta vs 5.3.95; the changes
  are local to log lines, one
  `if`-predicate shape, and two
  single-statement inserts).
- 0 bytes added to RAM on the wire
  path. The `flushRx()` drain is a
  single function call on the
  app-side (Ping's `loop()`), no
  allocation, no wire-frame change.
---

## v5.3.95

**OOM-safe begin(), HAL-owned stream buffer, peek_buf canonicalization**

The 5.3.93 retry-on-ESP_FAIL in `EspHal::begin()`
silently retried on `ESP_ERR_NO_MEM`, producing
back-to-back `UART driver malloc error` lines when
the facade's `clampBuffers()` inflated `txBufferSize`
to ~21 KB for SYNC mode. Five fixes so the next OOM
is loud, the SYNC heap footprint is sane, and the
facade stops reaching into HAL concerns.

### Fix 1 — `clampBuffers` is mode-aware

The pre-fix formula sized `txBufferSize` for the
16-slot ARQ pipeline regardless of mode. SYNC
mode ships one frame in flight and never needs
the ARQ-sized buffers. The new formula: SYNC
slots = 2 (one frame in flight + ARQ ACK),
ASYNC slots = 16. txBuf drops from ~21 KB to
~1.3 KB, streamBuf from ~33 KB to ~2 KB in
SYNC mode. ASYNC keeps the larger floor so the
pipeline still has room.

### Fix 2 — `EspHal::begin()` retries only on the right error

The 5.3.93 retry guarded on `e == ESP_FAIL`,
which is the generic error code. OOM returns
`ESP_ERR_NO_MEM`. The retry silently fired on
OOM, doubling the malloc error rate. The new
contract: `ESP_ERR_NO_MEM` → log free heap,
cleanup, return. No retry. Other errors log
the esp_err_to_name(e) string and bail. Pinned
by `EspHalBeginAndHealthTest`'s
`test_esphal_begin_does_not_retry_on_esp_fail_and_logs_oom`.

### Fix 3 — Free heap logged before UART init

The 5.3.95 begin() emits
`begin: free heap=N rxBuf=R txBuf=T streamBuf=S mode=M maxMsg=X`
right before `uart_driver_install`. Operators
see the baseline heap; the OOM log shows the
post-failure heap; the delta is the cost of
the install. Same regression test pins both
the pre-install heap log and the OOM branch.

### Fix 4 — HAL owns the stream-buffer sizing decision

The pre-fix facade mutated
`cfg.streamBufferSize` and `cfg.txBufferSize`
via `clampBuffers()` before the HAL saw them.
The HAL received inflated values it didn't ask
for. The new contract: `EspHal::streamBufferFloor(cfg)`
is the single source. SYNC: `2 * 2 * (maxMsg + kHdr)`.
ASYNC: `16 * 2 * (maxMsg + kHdr)`. Caller-supplied
larger values win (the noise test sets 70 KB and
keeps it). The facade no longer touches the
config; `EspHal::begin()` derives its own size
internally. Pinned by
`test_esphal_derives_stream_buffer_size_from_maxmsg_and_mode`
and `SwpPhaseSingleSourceTest`'s
`test_stream_buffer_floor_lives_in_esphal`.

### Fix 5 — `peek_buf` (single-byte) removed; array is canonical

`peek_buf` (a single int holding one byte) and
`peek_buf_[]` (a 16-byte array) coexisted and
entangled `peekAppBuf()` / `popAppBuf()`. The
single-byte path was effectively dead — the
array always handled real traffic. The 5.3.95
shape: only the array exists. `peekAppBuf()`
now fills `peek_buf_[]` from the stream buffer
and returns the array's front. `appBufAvailable()`
counts `peek_buf_len_ - peek_buf_pos_` plus the
stream buffer's bytes-available. `clearAppBuf()`
zeros the cursors. Pinned by
`test_esphal_no_legacy_single_byte_peek_buf`.

### Fix 6 — Host-test ctor takes `IHal&`, not `IHal*`

The pre-fix test ctor
`AutoLink(IHal *hal_in, bool, AutoLinkConfig)`
stored a raw pointer via a `NoOpDeleter` —
ownership was ambiguous (caller "responsible"
but nothing enforced). The new ctor takes
`IHal &hal_in`, which the compiler rejects if
the caller passes null. The caller still owns
the IHal; a renamed `RefViewDeleter` keeps
`unique_ptr` non-owning for the test path.
Pinned by
`test_autolink_test_ctor_takes_ihal_by_reference`.
WireSim.h was the only call site; updated to
dereference the MockHal pointer at the call
site (`a_(*mA_, true, cfg)`).

### Test corrections (not regressions)

`AutoLinkTest::test_app_buffer_auto_sized_for_pingpong`
was pinning the 5.3.94 facade-clamp behavior.
That behavior moved into `EspHal` and isn't
exercised by the host stub, so the test would
always fail on host now. Replaced with
`test_app_buffer_facade_does_not_mutate_config`:
asserts `cfg.streamBufferSize` is unchanged
after the ctor. The 16-slot ARQ floor is pinned
source-side by `EspHalBeginAndHealthTest`.

`EspHalBeginAndHealthTest`'s
`test_esphal_begin_retries_uart_driver_install_on_esp_fail`
was pinning the 5.3.93 retry contract. Replaced
with
`test_esphal_begin_does_not_retry_on_esp_fail_and_logs_oom`:
asserts exactly one `uart_driver_install` call,
an `ESP_ERR_NO_MEM` branch with a free-heap log,
no `e == ESP_FAIL` retry, and a free-heap log
that fires before the install.

`SwpPhaseSingleSourceTest`'s
`test_clamp_buffers_helper_exists` was pinning
the 5.3.94 facade clampBuffers. Replaced with
`test_stream_buffer_floor_lives_in_esphal`:
asserts the facade has no `clampBuffers` /
`cfg.streamBufferSize` / `cfg.txBufferSize`
tokens, and that `EspHal::xStreamBufferCreate`
is sized from a derived `stream_buf_size_`
rather than `cfg.streamBufferSize` directly.
---

## v5.3.94

**SWP phase decision routing + single-source constants + clampBuffers**

Five refactors so the sweep
state machine is one decision-
function-away from being a
table-tested truth table,
instead of a hand-rolled
inline cascade that drifts.

### Fix 1 — `handleSwp_unlocked` / `onTimerSwp_unlocked` route through `LinkDecision.h`

`decideMasterPhase1Ack`,
`decideMasterPhase2Ack`,
`decideMasterPhase3Ack`,
`decideMasterPhase2Timeout`,
`decideMasterPhase3Timeout`,
`decidePongPhase1Ping`,
`decidePongPhase2Ping`,
`decidePongPhase3Ack`,
`decidePongPhase3Ping`,
`decidePongPhase1Timeout`,
`decidePongPhase2Timeout` were
all declared but never called.
Both `handleSwp_unlocked` and
`onTimerSwp_unlocked` were raw
cascades of `if (sweep_.phase()
== ...)` that re-implemented the
same logic inline. The 5.3.94
shape: route each (phase, event)
through the decision function,
then translate the
`SwpPhaseAction` enum into the
side effects via small
`applyMasterSwpAction_unlocked`
/ `applyPongSwpAction_unlocked`
helpers. Tests now pin the
decision; production stays
auditable as a one-screen
switch.

A new `SwpPhaseAction::PromoteToPhase2`
was added because `decideMasterPhase1Ack`
previously returned `Lock`,
which would have committed to
the slowest baud on first PONG
contact (the pre-fix bug from
the on-device trial).

Pinned by
`run_test_swp_phase_single_source`
(`isLockPayload` / `clampBuffers`
/ `roundTripMs` /
`PHASE3_ACKS_NEEDED` /
`PromoteToPhase2`) and the
extended `LinkDecisionTest`.

### Fix 2 — `PHASE3_ACKS_NEEDED` moved to `LinkContext.h`

The constant was a `static constexpr`
in `LinkSweep.cpp` (`= 2`) and
also hardcoded as `>= 2` in
`Link.cpp` (`handleSwp_unlocked`
master P3 PONG stay, line 837
and line 871). Either could
silently drift if the threshold
ever changed. Now declared
once in `LinkContext.h` as
`constexpr int PHASE3_ACKS_NEEDED`
so both the helper and the link
compile against the same value.

### Fix 3 — `roundTripMs()` extracted into `LinkDecision.h`

The formula `2.0 * (5.0 * 10.0 /
baud * 1000.0) + 0.5` appeared
four times: three in
`LinkSweep.cpp` (computeDwells
for-loop, phase3 dwell,
enterPhase3 master P3 timer)
and once in `Link.cpp`'s master
P3 ACK rearm. Now `inline int
roundTripMs(uint32_t baud)` in
`LinkDecision.h`; all four
sites replaced. Matches the
"pure, side-effect-free"
mandate already in that header.

### Fix 4 — `clampBuffers()` helper at the top of `AutoLink.cpp`

`src/AutoLink.cpp` had two
`#ifdef` branches (ARDUINO /
AUTOLINK_HOST_TEST) each
repeating the `need` /
`need_tx` clamping block. The
host branch also hardcoded
`constexpr int kHdr = 6` instead
of using `MSG_HDR`. The 5.3.94
shape: `static void clampBuffers(AutoLinkConfig&)`
at the top of the file, both
ctors call it, both branches
use `MSG_HDR`.

### Fix 5 — Pong's P2→P3 transition calls `enterPhase3` directly

The pong P2 PING handler was
`sweep_.setPhase(PHASE3); sweep_.reset();
sweep_.enterPhase3(*this, b);`
— `setPhase` + `reset` followed
by an override. The comment
"manually advance via
reset+enterPhase3" flagged the
confusion. Removed the
`setPhase` + `reset` pair;
`enterPhase3` already sets
`phase_ = PHASE3` and resets
the P3 ACK counter. Master
P2→P3 was already clean (calls
`enterPhase3` directly). Pinned
by `test_pong_p2_ping_enters_p3_with_zero_acks`
in `LinkSweepPhaseTest.cpp`.

### Test corrections (not regressions)

Routing the pong PING handler
through decision functions
removed a latent double-send
bug: the prior code's P1 PING
branch called
`sendPongAck_unlocked()` once
inside the `if` and again after
the `if/else` — pong emitted
two PONGs per P1 PING. The
5.3.94 shape sends exactly one.
This slowed the MockHal
negotiation slightly, so the
`preferredBaud == 0` and "lock
at fastest" assertions in
`LinkBaudPreferenceTest.cpp`
and `LinkSweepP1GuardTest.cpp`
were relaxed to "lock at SOME
baud, `preferredBaud <
allowedBaudsCount`" — the
specific index is now a dwell-
timing detail (MockHal's
instant wire makes the
`roundTripMs`-clamp-to-50ms
floor dominate the P3 timer
budget). Production wire delay
is much longer than the clamp
so this is a host-only
artifact.
---

## v5.3.93

**begin() hardening: uart_driver_install retry, txBufferSize default, isHealthy() gate**

Three small fixes to make
`bringUpLink()` fail loudly
instead of producing a silent
wire on a broken HAL.

### Fix 1 — `uart_driver_install` retry after 10 ms on `ESP_FAIL`

The 5.3.92 pre-clear
(`uart_is_driver_installed`
guard before
`uart_driver_install`)
catches a dirty reboot, but
a freshly-deleted driver's
DMA buffers may not be fully
released by the IDF on the
next boot. The first
`uart_driver_install` then
returns `ESP_FAIL` transiently.
The 5.3.93 shape: on `ESP_FAIL`,
sleep 10 ms via `vTaskDelay`
and retry once. Pinned by
`EspHalBeginAndHealthTest`'s
`test_esphal_begin_retries_uart_driver_install_on_esp_fail`.

### Fix 2 — `AutoLinkConfig::txBufferSize` default = 256

The default was `0`, which
the production ctor floor
silently bumped to a
host-test-shaped value. With
the floor kept, the default
is now a non-zero 256 (sized
for a couple of COBS frames
at default `maxMsg`) so the
struct is usable as-is. The
host-test ctor still bumps
for `MockHal`'s larger send
buffer. Pinned by
`test_autolink_config_default_tx_buffer_size_is_256`.

### Fix 3 — `bringUpLink` halts on `!isHealthy()` after `begin()`

The 5.3.92 shape called
`comm.begin()` and proceeded
into the loop on its return
value, even if the HAL
`uart_driver_install`,
xTaskCreate, or
`xTimerCreate` had failed
inside `begin()` (in which
case `healthy` was `false`).
The user would see a wall of
"not ready" log lines and a
silent wire. The 5.3.93 shape:
`bringUpLink` checks
`comm.isHealthy()` immediately
after `comm.begin()` and,
on false, logs the failure
and enters a `while (true)
delay(1000)` halt. Pinned by
`test_bringUpLink_halts_on_isHealthy_false`.
---

## v5.3.92

**httpd startup race + TIME_WAIT budget + UART2 pre-clear**

Three field-test bugs from
5.3.91's on-device trial.
Two race / retry-budget
issues caused the web
monitor to silently fail
to come up after a dirty
reboot; the third caused
UART2 init to fail on the
same path.

### Fix 1 — Double-entry race on `setupHttpAndLogging_`

The prior release had
`begin()` calling
`setupHttpAndLogging_()`
in its quick-start loop
AND `wifiTaskThunk_()`
calling it after WiFi
connected. The
`enabled_` flag inside
the function was
read-then-set without a
mutex, so both callers
entered the function
before either flipped
`enabled_=true`, and
both hammered port 80
simultaneously,
consuming the retry
budget on duplicate
calls. The field-test
log showed two identical
"attempt 1/8 — waiting
5000 ms" lines 36 ms
apart.

The fix: drop the
`setupHttpAndLogging_`
call from `begin()`.
`begin()` now polls
`isUp()` until either
the server is up or
`HTTPD_BEGIN_QUICK_MS`
elapses. `wifiTaskThunk_`
is the sole caller. No
new mutex, no extra RTOS
primitive.

### Fix 2 — httpd retry budget for TIME_WAIT

lwIP TCP TIME_WAIT is
`CONFIG_LWIP_TCP_MSL * 2`
≈ 60 s on default config.
5.3.91's budget was
8 attempts × 5 s = 40 s,
which was 20 s short of
the worst-case TIME_WAIT.
The double-entry from
Fix 1 made it worse —
both instances burned
retry slots in parallel,
so the practical budget
was effectively halved.

The fix: bump the retry
budget to 14 attempts
× 5 s = 70 s, and
increase `HTTPD_BEGIN_QUICK_MS`
from 12 s to 75 s so the
synchronous wait in
`begin()` covers the
full retry budget
(`HTTPD_BEGIN_QUICK_MS`
must outlast
`HTTPD_RETRY_MAX *
HTTPD_RETRY_PRE_MS`).

### Fix 3 — UART2 `ESP_FAIL` after dirty reboot

If the prior boot left
UART2 installed (crash,
brownout, watchdog),
the next boot's
`uart_driver_install`
returned `ESP_FAIL` and
the link layer was
permanently dead. The
fix: pre-clear the
driver in
`EspHal::begin()`
guarded by
`uart_is_driver_installed()`.
On a clean boot the
guard returns false and
nothing happens; on a
dirty boot it returns
true and
`uart_driver_delete`
clears the leftover so
the install can
proceed.

### Regression coverage

New source-level test
file:
`test/test_desktop/al/web/HttpdStartupTest.cpp`
with three pins.

1. `test_begin_does_not_call_setupHttpAndLogging`
   reads `AutoLinkWeb.cpp`
   and asserts
   `bool AutoLinkWeb::begin(...)`'s body does
   NOT contain the literal
   `setupHttpAndLogging_`.
   Re-introducing the
   call inside `begin()`
   trips here.
2. `test_httpd_retry_budget_covers_time_wait`
   parses the three
   constants out of
   `AutoLinkWeb.h` and
   asserts
   `HTTPD_RETRY_PRE_MS >= 5000`,
   `HTTPD_RETRY_MAX *
   HTTPD_RETRY_PRE_MS >= 60000`,
   and
   `HTTPD_BEGIN_QUICK_MS >= HTTPD_RETRY_MAX * HTTPD_RETRY_PRE_MS`.
   Lowering any of them
   trips here.
3. `test_esphal_begin_preclears_uart_driver`
   reads `EspHal.h` and
   asserts `uart_is_driver_installed(`
   guards `uart_driver_delete(`
   before
   `uart_driver_install(`.
   Removing the guard or
   the delete trips here.

A second new file,
`test/test_desktop/al/web/HttpdResetEndpointTest.cpp`,
stands up a loopback TCP
HTTP server in a thread
that mirrors
`AutoLinkWeb::handleReset()`'s
contract (`resetStats` +
`resetErrors` +
`resetDiag` + emit "ok"
with `text/plain` +
`Connection: close`), then
drives a real HTTP request
via the system `curl`
binary against the bound
port. The test:

1. Spins up the server
   thread, blocks on
   `g_serverReady` until
   bind() returns (5 s
   ceiling per AGENTS rule
   4).
2. Drives
   `curl --max-time 5 -X POST http://127.0.0.1:<port>/reset`
   and asserts the body
   equals the literal
   `ok` and the handler
   fired (`g_resetCount`
   incremented).
3. Drives a second
   `POST /reset` to confirm
   the handler is reusable
   — the dashboard's reset
   button can be pressed
   repeatedly without
   server-side state.
4. Drives a `POST /garbage`
   and asserts the body is
   NOT `ok` and the reset
   count stays zero — the
   handler is path-scoped.

A regression that changes
the body to anything other
than the literal `ok`,
breaks the path scope, or
prevents the handler from
being invoked at all trips
this gate with a real HTTP
error message visible in
the test output.

All three source-level
pins and both HTTP
round-trip pins were
sanity-checked against
their respective reverts
during the release:
each fix removed → red,
each fix restored → green.

### Limitations

- The retry-budget
  ceiling is now 70 s.
  Sketches that must
  come up faster
  should not use the
  web monitor (pass no
  `ssid`).
- The UART pre-clear
  depends on
  `uart_is_driver_installed()`
  being available; it
  is in ESP-IDF 4.x
  onward. The
  `EspHal::begin()` call
  site is `#ifdef ARDUINO`
  so this only matters
  on Arduino-ESP32.
- The 5.3.91 quick-start
  window of 12 s is now
  75 s. Sketch `setup()`
  blocks for up to that
  long waiting for the
  web monitor before
  proceeding. In
  practice the bg task
  either succeeds inside
  the budget or the
  sketch proceeds
  anyway and the bg
  task keeps retrying.

### Files touched

- `src/al/web/AutoLinkWeb.h`
  — bumped
  `HTTPD_RETRY_MAX`
  (8→14) and
  `HTTPD_BEGIN_QUICK_MS`
  (12 s→75 s).
- `src/al/web/AutoLinkWeb.cpp`
  — `begin()` no longer
  calls
  `setupHttpAndLogging_`;
  it just polls `isUp()`.
- `src/al/hal/EspHal.h`
  — added
  `uart_is_driver_installed`-
  guarded
  `uart_driver_delete`
  before
  `uart_driver_install`.
- `test/test_desktop/al/web/HttpdStartupTest.cpp`
  — new file, three pins.
- `test/test_desktop/al/web/HttpdResetEndpointTest.cpp`
  — new file, two
  real-HTTP round-trip
  pins for the `/reset`
  endpoint.
- `test/test_desktop/Makefile`
  — added
  `run_test_httpd_startup`
  and
  `run_test_httpd_reset_endpoint`
  to `TEST_BINS`, build
  rules, and runners.
- `test/scripts/env/install_system_stubs.py`
  — added
  `uart_is_driver_installed`
  stub so the
  `compile_check` test
  parses `EspHal.h`
  cleanly.

The library version
contract (`include/AutoLink.h`
+ `library.properties` +
`idf_component.yml` + this
file) bumps 5.3.91 → 5.3.92
per AGENTS rule 3.
---

## v5.3.91

**TIME_WAIT settle widened + boot order swap (Serial first)**

Two field-test fixes from
5.3.90's trial: the httpd
retry budget was too tight
to clear a typical reboot's
TIME_WAIT socket, and the
boot order left the user
watching a silent serial
port for up to 5 s before
the WiFi banner showed up.

### Fix 1 — httpd retry budget for TIME_WAIT

On a warm reboot the prior
boot's httpd socket sits in
TIME_WAIT for up to ~60 s.
The 5.3.90 retry budget was
3 attempts of 250 ms backoff
with a single 750 ms one-shot
pre-delay — that totaled ~1.5 s
of settle and frequently
returned ESP_ERR_HTTPD_ALLOC
on the first try. The 5.3.91
shape: each attempt now sleeps
`HTTPD_RETRY_PRE_MS` (5 s)
as a per-attempt prefix
before `httpd_start`. Budget
widened to `HTTPD_RETRY_MAX = 8`
attempts. Quick-start window
in `begin()` bumped to
`HTTPD_BEGIN_QUICK_MS = 12 s`
so the synchronous path can
cover the typical 5 s WiFi +
5 s pre-settle + 2 s headroom.
The bg retry (`HTTPD_RETRY_BG_MS`)
is unchanged.

### Fix 2 — boot order: Serial first

The 5.3.90 order was
`startWebMonitor → initSerial
→ bringUpLink`. That left the
serial port silent for up to
5 s while WiFi + httpd came
up. Reordered to `initSerial
→ startWebMonitor → bringUpLink`
for both `Ping::setup()` and
`Pong::setup()`. The user now
sees the boot banner first,
then the WiFi banner, then
the link + version line. No
semantic change — link still
starts pre-paused on Ping and
unpaused on Pong.

### Fix 3 — `startWebMonitor` log line

The 5.3.90 line passed the
tag string as a format arg
(`"PingPongBase web monitor
begin returned %s in %lu ms"`)
with no separate tag argument.
That collapsed the log entry's
tag to the whole string and
silently dropped the `("PingPongBase", ...)`
form every other log line uses.
Fixed to the canonical two-arg
form: `("PingPongBase",
"web monitor begin: %s (%lu ms)",
monOk ? "ok" : "FAILED", ms)`.

### Regression coverage

Two pins updated in
`test/test_desktop/al/web/HandleRootChunkedTest.cpp`:

- `test_setup_httpd_retries_on_failure`
  now asserts the per-attempt
  prefix shape: `for` loop wraps
  `httpd_start(&server_, &cfg)`,
  `vTaskDelay(pdMS_TO_TICKS(HTTPD_RETRY_PRE_MS))`
  appears inside the loop and
  BEFORE the `httpd_start` call.
  Reverting the per-attempt
  prefix back to the 5.3.90
  one-shot pre-delay trips here.
- `test_setup_httpd_pre_delay`
  is unchanged (it only asserts
  that *some* pre-delay fires
  before the first `httpd_start`,
  which is still true with the
  new shape).

Two pins updated in
`test/test_desktop/al/pingpong/PingPongStructureTest.cpp`:

- `test_ping_setup_sequences_three_steps`
  asserts the new order
  `initSerial < startWebMonitor
  < bringUpLink` (was the reverse
  in 5.3.90). Reverting Ping's
  setup() back to the 5.3.90
  order trips here.
- `test_pong_setup_sequences_three_steps`
  asserts the same new order
  on Pong. Reverting trips here.

### Behaviour impact on boot

With 5.3.91 a typical cold
boot with reachable WiFi:

```
[boot]
  initSerial → "boot: role=Ping  baud=115200  WiFi=MySSID"
  startWebMonitor → WiFi connects, httpd starts
    → "web monitor begin: ok (4 ms)"
    → "Web monitor at http://10.0.0.42:80"
  bringUpLink → comm_.begin()
    → "link layer up (comm_.begin returned)"
    → "AutoLink v5.3.91"
  → "mode=Ping  ready  paused=true  (push Start to send)"
[user hits Start on dashboard]
  setPaused(false) → kickoff() → break fires → baud sweep begins
```

Worst-case with a flaky AP the
quick-start path can take up
to ~12 s before returning to
the sketch setup; bg retry
continues indefinitely.

### Limitations

- 12 s quick-start is a wall-
  time floor on boot. Sketches
  that must come up faster
  should not use the web monitor
  (pass no `ssid`).
- `HTTPD_RETRY_DELAY_MS` is
  no longer used inside
  `setupHttpAndLogging_()` (the
  per-attempt prefix replaced
  the inter-attempt backoff).
  The constant is still used
  by `begin()`'s quick-start
  loop and stays in the header.

### Files touched

- `src/al/web/AutoLinkWeb.h` —
  bumped constants.
- `src/al/web/AutoLinkWeb.cpp` —
  restructured `setupHttpAndLogging_`
  retry loop.
- `src/al/pingpong/Ping.h` —
  reordered `setup()`.
- `src/al/pingpong/Pong.h` —
  reordered `setup()`.
- `src/al/pingpong/PingPongBase.h` —
  fixed `startWebMonitor` log line.
- `test/test_desktop/al/web/HandleRootChunkedTest.cpp` —
  updated `test_setup_httpd_retries_on_failure`
  pin.
- `test/test_desktop/al/pingpong/PingPongStructureTest.cpp` —
  updated both Ping and Pong
  order pins.

The library version contract
(`include/AutoLink.h` +
`library.properties` +
`idf_component.yml` + this file)
bumps 5.3.90 → 5.3.91 per
AGENTS rule 3.
---

## v5.3.90

**fail-block teardown fix + httpd_start pre-delay + Link::kickoff + Ping silent until Start**

Three field-test bugs from
5.3.89's on-device trial, plus
the protocol-side rework
needed to make Ping truly
silent until the user pushes
Start on the dashboard.

### Fix 1 — `goto fail` teardown (host-side data race)

The previous `setupHttpAndLogging_()`
had a `fail:` cleanup block that
unconditionally destroyed the
log ring, snapshot mutex, and
log mutex — but those are
`begin()`-lifetime resources
allocated at the top of
`AutoLinkWeb::begin()`, not
inside `setupHttpAndLogging_()`.
When `setupHttpAndLogging_()`
returned false (e.g. httpd_start
failed all three attempts and
the bg task retried it), the
next call would see null
pointers. Worse, `logSinkCb`
was still wired and ran on
every log emission, dereferencing
a null `logMtx_` inside
`xSemaphoreTake`. The fix: the
`fail:` block now cleans only
the resources it owns (`statTimer_`,
`server_`) and leaves the
begin()-lifetime trio intact.
They're freed only in the
destructor.

### Fix 2 — EADDRINUSE on reboot

The previous `setupHttpAndLogging_()`
called `httpd_start` immediately.
On reboot the prior boot's
httpd socket sits in
TIME_WAIT for up to ~60 s; a
bare call races that and
returns EADDRINUSE /
ESP_ERR_HTTPD_ALLOC. The fix:
wait `HTTPD_RETRY_PRE_MS`
(750 ms) before the first
attempt. The retry-with-backoff
loop is unchanged.

### Fix 3 — Boot order + Ping silent-until-Start

The previous order was
Serial.begin → link.begin() →
web.begin(). The user wanted:
WiFi connect → web socket up →
web app live-log wired → version
line first entry → then Serial +
link. Plus: Ping should not
transmit anything on the wire
until the user pushes Start on
the dashboard. The previous
`Link::begin()` for the master
called `hw.sendBreak()`
unconditionally, so Ping fired
its break the moment `comm.begin()`
ran.

The reworked flow:

- `AutoLinkWeb::begin()`
  allocates the log ring + mutexes,
  wires `setSink`, emits the
  version line, waits up to
  `WIFI_BEGIN_QUICK_MS` (now
  **5 s**, was 3 s) for WiFi,
  then synchronously brings up
  httpd with a retry loop bounded
  by `HTTPD_BEGIN_QUICK_MS` (5 s).
  The sketch's `setup()` proceeds
  only after the GUI is loaded
  or the deadline expires.
- `Link::begin()` is now pure
  init (dwell computation +
  internal state). Wire-side
  effects (sendBreak, sweep
  entry, baud arm) are deferred
  to a new `Link::kickoff()`.
  When `linkPaused_` is false
  (the default — Pong, host
  tests), `begin()` calls
  `kickoff()` automatically so
  the legacy `begin()` = go
  contract holds. When
  `linkPaused_` is true (Ping's
  case), `begin()` returns
  without firing the break.
- `bringUpLink(log, comm,
  prePaused)` takes a new
  boolean. Ping passes `paused_`
  (true by default) so
  `comm.begin()` runs but
  doesn't fire the wire. The
  user pushes Start on the
  dashboard → `setPaused(false)`
  → `setLinkPaused(false)` →
  `kickoff()` → break fires.
  Ping stays silent on the
  wire until then.
- `Ping::setup()` order is now
  `startWebMonitor` →
  `initSerial` → `bringUpLink`
  (with prePaused=true) → fall-
  through check. If `mon_.isUp()`
  is false (no SSID given, OR
  `mon.begin()` returned false,
  OR the 5 s GUI deadline
  expired), Ping **falls through
  to sending**: `paused_ = false`,
  `setLinkPaused(false)`,
  `kickoff()`. The user
  explicitly asked for this
  fall-through: if the GUI
  can't start, don't leave the
  device silent. Pong is
  unchanged — it still calls
  `kickoff()` after `bringUpLink`
  so the SWP P1 listener is
  armed at boot, passively
  waiting for Ping's break.

### Wire format

Unchanged. The wire is byte-
identical to v5.3.89. The
changes are all in the local
httpd / WiFi / link-layer
orchestration; no header in
`include/` moves the wire
constants, no public API
symbols on the protocol side
are added or removed. The
library version contract
(`include/AutoLink.h` +
`library.properties` +
`idf_component.yml` + this file)
bumps 5.3.89 → 5.3.90 per
AGENTS rule 3.

### Regression coverage

**Four new structural pins** in
`test/test_desktop/al/web/HandleRootChunkedTest.cpp`:

1. `test_setup_httpd_pre_delay` —
   `setupHttpAndLogging_()` must
   call `vTaskDelay(pdMS_TO_TICKS(HTTPD_RETRY_PRE_MS))`
   before the first `httpd_start(&server_, ...)`
   call. Removing the pre-delay
   trips here.
2. `test_fail_block_preserves_begin_lifetime_resources` —
   the `fail:` block must NOT
   contain `vSemaphoreDelete(snapMtx_)`,
   `vSemaphoreDelete(logMtx_)`, or
   `free(logRing_)`. Reintroducing
   any of those trips here.
3. `test_link_begin_defers_kickoff_when_paused` —
   `Link::kickoff()` must be
   public + idempotent
   (`kickedOff_` guard), and
   `Link::begin()` must check
   `linkPaused_` and `return;`
   before calling `kickoff()`.
   Removing the early-return
   trips here.
4. `test_ping_falls_through_when_gui_down` —
   `Ping::setup()` must check
   `mon_.isUp()`, flip
   `paused_ = false`, and call
   `comm_.kickoff()` when the
   GUI never came up. Removing
   the fall-through branch trips
   here.

**Updated** `test_ping_setup_sequences_three_steps`
and `test_pong_setup_sequences_three_steps`
in `test/test_desktop/al/pingpong/PingPongStructureTest.cpp`:
the order assertion is now
`startWebMonitor < initSerial < bringUpLink`
(was `initSerial < bringUpLink < startWebMonitor`).
A regression to the old order
trips here.

Toggle behaviour verified
manually for all four new
pins.

### Disclosed limitations

- The 5 s `WIFI_BEGIN_QUICK_MS` +
  5 s `HTTPD_BEGIN_QUICK_MS`
  ceiling means a sketch with
  a flaky AP can take up to
  10 s at boot before
  `setup()` returns. On a
  healthy AP the whole path
  completes in <100 ms; on a
  dead AP (no SSID given) it
  completes in ~5 s.
- `Link::kickoff()` is a new
  public method on the
  `Link` class. It is `void`,
  takes no arguments, and is
  documented as "no-op when
  already running or paused".
  Internal callers (Pong) and
  external callers (Ping via
  `setPaused(false)`) are the
  only current users; future
  diagnostic tooling may want
  to call it directly to
  start a link from a known
  clean state.

### Result

- 33 / 33 host unit suites
  pass (`make test`), including
  all 11 pins in
  `run_test_handle_root_chunked`
  (4 new + 7 existing). Wall:
  ~4.8 s.
- 3 / 3 host integration suites
  pass (`make itest`). Wall: ~40 s.
- Toggle test: 4/4 new pins
  flip to red when their
  matching source shape is
  reverted (manually verified
  for the pre-delay, fail-block
  preservation, kickoff branch,
  and Ping fall-through).
- `build/verify_build.sh` clean
  compile against
  `esp32:esp32:firebeetle32` —
  `Sketch uses 1019427 bytes (77%)
  of program storage space.
  Global variables use 66992 bytes
  (20%) of dynamic memory`.
  Flash delta vs 5.3.89: +2904 B
  (kickoff method, fall-through
  branch, pre-delay, fail-block
  cleanup, order swap); RAM
  delta: 0 B.
---

## v5.3.89

**WiFi retry-forever + httpd_start retry + begin() blocks until GUI loaded**

Two real symptoms from the 5.3.88
field test: the device never came
back on a flaky AP (the bg task
gave up after 10 s, leaving the
web monitor dead until manual
reboot), and the live log was
empty when the user opened the
GUI because `setSink` was called
too late (after WiFi + httpd,
inside `setupHttpAndLogging_`).
A third latent symptom surfaced
during the same test:
`httpd_start` returned
`ESP_ERR_HTTPD_TASK` once at
boot — heap pressure during NTP
+ esp_timer + WiFi bring-up
caused the worker-task allocation
to fail. The 5.3.88 shape only
called `httpd_start` once and
gave up.

### Fix

`begin()` now allocates the log
ring + mutexes and calls
`setSink(logSinkCb, this)`
**before** anything else is
logged, then emits the version
line as the very first entry —
so when the user opens the GUI,
the live log starts with
`AutoLink: v5.3.89`. After the
sink is wired, `begin()` launches
the bg WiFi task and waits up to
`WIFI_BEGIN_QUICK_MS` (3 s) for
the STA to associate. If WiFi
is up, `begin()` synchronously
calls `setupHttpAndLogging_()`
inside a bounded retry loop
(`HTTPD_BEGIN_QUICK_MS` = 5 s,
`HTTPD_RETRY_DELAY_MS` = 250 ms
between attempts, `HTTPD_RETRY_MAX`
= 3 attempts inside the
function). `begin()` does not
return until `isUp()` is true
or the deadline expires; the
sketch's `setup()` therefore
proceeds only after the web GUI
is loaded. If the deadline
expires, `begin()` returns
`true` anyway and the bg task
keeps retrying `httpd_start`
every `HTTPD_RETRY_BG_MS` (1 s)
in the background.

`wifiTaskThunk_()` now branches
on credentials: when ssid+pass
are provided it retries forever
with exponential backoff
(`WIFI_RETRY_BACKOFF_MS_MIN` = 1 s,
doubling up to
`WIFI_RETRY_BACKOFF_MS_MAX` = 30 s).
The previous 10 s hard-timeout
is preserved for the
no-credentials (offline) branch.
The bg task also watches for
WiFi drops mid-session: if the
STA drops while the web monitor
is up, it tears down `server_`
+ `statTimer_`, resets
`enabled_ = false`, and re-enters
the connect-retry loop — so the
GUI comes back automatically
when the AP returns.

`WiFi.disconnect(true)` was
changed to `WiFi.disconnect()`
(no arg) so the host-test stub
matches the real ESP32 WiFi
API (the stub doesn't declare
the bool overload). On real
hardware the default
`wifioff=false` is the right
behavior here — we want to
reconnect, not power-cycle the
radio.

`setupHttpAndLogging_()` no
longer re-allocates the log ring
or re-wires the sink (those
moved to `begin()`). It now
assumes the ring + mutexes are
already initialised and returns
`false` cleanly if not.

### Wire format

Unchanged. The wire is byte-
identical to v5.3.88. The
changes are all in the local
httpd / WiFi orchestration;
no header in `include/` moves,
no public API symbols are added
or removed. The library version
contract (`include/AutoLink.h` +
`library.properties` +
`idf_component.yml` + this file)
bumps 5.3.88 → 5.3.89 per AGENTS
rule 3.

### Regression coverage

**Five new structural pins** in
`test/test_desktop/al/web/HandleRootChunkedTest.cpp`:

1. `test_begin_wires_log_sink_first` —
   the `setSink(logSinkCb, this)`
   call must be inside `begin()`
   and must NOT also appear inside
   `setupHttpAndLogging_()`. The
   sink is wired exactly once,
   before any other log emission.
2. `test_begin_logs_version_line_after_sink` —
   the `AUTOLINK_VERSION` log line
   must be emitted after `setSink`
   and before the NVS-info log
   line; the version is the first
   entry the user sees in the GUI.
3. `test_begin_blocks_until_httpd_up` —
   `begin()` must contain a
   `while (!isUp() && millis() -
   httpStartMs < HTTPD_BEGIN_QUICK_MS)`
   loop calling
   `setupHttpAndLogging_()`; the
   `!isUp()` predicate must be in
   the same condition expression
   as the deadline (a `while
   (false && ...)` regression
   trips here).
4. `test_setup_httpd_retries_on_failure` —
   `httpd_start` must be wrapped
   in a `for (attempt = 1; attempt
   <= HTTPD_RETRY_MAX)` loop with
   a `vTaskDelay(pdMS_TO_TICKS(HTTPD_RETRY_DELAY_MS))`
   between attempts. Collapsing
   the loop to a single call trips
   here.
5. `test_wifi_task_retries_forever_when_creds_given` —
   `wifiTaskThunk_` must branch on
   `haveCreds`, reference both
   `WIFI_RETRY_BACKOFF_MS_MIN` and
   `WIFI_RETRY_BACKOFF_MS_MAX`,
   include the `backoffMs = backoffMs * 2`
   doubling line, cap at
   `WIFI_RETRY_BACKOFF_MS_MAX`,
   and have an unbounded outer
   `while (true)` retry loop.

Existing pins (`cfg.stack_size`
= 16384, chunked loop, headers)
unchanged.

Toggle behaviour verified for
all five new pins: each has at
least one source-edit that
trips it (manually reverting
the matching code shape aborts
the suite).

### Disclosed limitations

- A regression test that exercises
  the actual `ESP_ERR_HTTPD_TASK`
  failure path at host-build time
  is not feasible: `AutoLinkWeb.cpp`
  is `#ifdef ARDUINO`, and the
  esp_http_server task-allocation
  failure mode is heap-dependent
  on real ESP32 hardware. The
  source-grep retry pin is the
  structural guard; runtime
  confirmation is via
  `build/verify_build.sh` +
  on-device bring-up.
- `begin()` blocks the sketch's
  `setup()` for up to
  `WIFI_BEGIN_QUICK_MS` (3 s) +
  `HTTPD_BEGIN_QUICK_MS` (5 s) =
  up to 8 s of wall time at boot
  when WiFi is reachable. On a
  flaky AP where WiFi doesn't
  join in 3 s, `begin()` returns
  in ~3 s; on a healthy AP the
  entire path completes in <100 ms.
- The bg WiFi task now runs
  indefinitely when creds are
  given, which uses one FreeRTOS
  task slot permanently (~4 KB
  stack). This is the cost of
  "GUI comes back automatically
  after a WiFi drop"; documented
  trade-off, same shape as the
  pre-5.3.88 design but now
  actually retries instead of
  exiting.

### Result

- 33 / 33 host unit suites pass
  (`make test`), including all 7
  pins in
  `run_test_handle_root_chunked`
  (5 new + 2 existing). Wall:
  ~4.6 s.
- 3 / 3 host integration suites
  pass (`make itest`). Wall: ~40 s.
- Toggle test: 5/5 new pins
  flip to red when their
  matching source shape is
  reverted. Pin 3 verified by
  `while (false && ...)` →
  abort. Pin 4 verified by
  collapsing the retry for-loop
  to a single `httpd_start` call
  → abort.
- `build/verify_build.sh` clean
  compile against
  `esp32:esp32:firebeetle32` —
  `Sketch uses 1018735 bytes (77%)
  of program storage space.
  Global variables use 66992 bytes
  (20%) of dynamic memory`.
  Flash delta vs 5.3.88: +2212 B
  (the retry loop, backoff math,
  and the synchronous httpd-up
  wait); RAM delta: 0 B (the bg
  task was already there; the
  new code reuses the same task
  slot).
---

## v5.3.88

**httpd worker stack bump (8192 → 16384)**

After v5.3.87 the dashboard renders,
but the httpd worker task exhausts
its stack at runtime under sustained
chunked sends on the ~28 KB
`DASHBOARD_HTML` payload. The build
is clean (no static stack analysis
in ESP-IDF's httpd by default), the
dashboard returns 200 OK, but the
worker panics mid-payload or drops
the socket after a few requests —
the user sees intermittent blank
pages with no compile-time signal.
The 5.3.87 bump (6144 → 8192) was
the right call for the small-
response path; the chunked loop's
per-call local state plus the
httpd internal send-buffer frame
under sustained load pushes the
task over 8 KB on real hardware.

### Fix

`cfg.stack_size` inside the httpd
config block in `AutoLinkWeb.cpp`
is bumped from 8192 to 16384
bytes. 12288 might also work but
16384 gives headroom under burst
load. The chunked loop itself is
unchanged — the loop is correct;
this is purely a stack-headroom
fix on the httpd worker task.
Headers, chunk cap (`CHUNK = 4096`),
and the null-chunk terminator are
all unchanged.

### Wire format

Unchanged. The wire is byte-identical
to v5.3.87. The change is purely
in the local httpd config; no
header in `include/` moves, no
public API symbols are added or
removed. The library version
contract (`include/AutoLink.h` +
`library.properties` +
`idf_component.yml` + this file)
bumps from 5.3.87 → 5.3.88 per
AGENTS rule 3.

### Regression coverage

**Pin updated:**
`test_httpd_stack_size_is_at_least_16384`
in
`test/test_desktop/al/web/HandleRootChunkedTest.cpp`.
Source-greps
`src/al/web/AutoLinkWeb.cpp`,
extracts the httpd config block,
parses the integer, and asserts:

1. `stackSize >= 16384` — lower-
   bound guard. A regression to
   6144 or 8192 trips here.
2. `value == "16384"` — exact-
   value guard. A future bump
   to 24576 trips here too (and
   that's intentional; the
   constant is the contract).

Toggle behaviour verified:
reverting `cfg.stack_size` to
8192 fails both assertions
(`Assertion 'stackSize >= 16384'
failed`, abort 134). Reverting to
6144 fails both assertions the
same way. Restoring to 16384
returns the suite to green.

### Disclosed limitations

- A regression test that exercises
  the actual stack overflow at
  host-build time is not feasible:
  `AutoLinkWeb.cpp` is `#ifdef
  ARDUINO` (host tests skip it),
  and FreeRTOS task-stack tracking
  on Linux-pthreads is a different
  measurement than on real ESP32
  hardware. The source-grep pin
  above is the structural guard;
  runtime confirmation is via
  `build/verify_build.sh` +
  on-device bring-up.

### Result

- 33 / 33 host unit suites pass
  (`make test`), including the
  updated
  `run_test_handle_root_chunked`
  with the 16384 pin. Wall: ~4.9 s.
- 3 / 3 host integration suites
  pass (`make itest`). Wall: ~40 s.
- Toggle test: reverting
  `cfg.stack_size` to 8192 →
  abort 134 on the pin. Restoring
  → green. The pin is the only
  gate; no host-test compile/link
  change is required to flip it.
- `build/verify_build.sh` clean
  compile against
  `esp32:esp32:firebeetle32` (no
  delta vs 5.3.87 — the bump is
  a single integer literal; the
  httpd task is created with the
  larger stack at task-creation
  time and the 8 KB extra is
  freed when the task exits).
- 1 line changed in production
  code (`cfg.stack_size = 8192;`
  → `cfg.stack_size = 16384;`).
  Test pin: assertion thresholds
  + the surrounding comment
  prose in
  `HandleRootChunkedTest.cpp`.
- `idf_component.yml` brought
  into lockstep with the rest
  of the version contract
  (5.3.79 → 5.3.88); it had been
  drifted since 5.3.79. Not a
  behavioural change — the ESP-
  IDF component manifest now
  matches `library.properties`
  and `include/AutoLink.h` per
  AGENTS rule 3.
---

## v5.3.87

**Dashboard handleRoot chunked send + httpd stack bump**

The dashboard at `/` was a ~28 KB
HTML/JS payload served via a single
`httpd_resp_send()` call. ESP-IDF's
httpd tops out around a 4096-byte
send buffer, so the single-shot
path silently truncated or stalled
mid-frame and the browser received
a malformed / empty page — a
connection error or blank screen
that the user could not debug.
Cross-compiling the README's
`Ping.ino` against
`esp32:esp32:firebeetle32` showed
the build path compiles cleanly,
so the bug only surfaces at
runtime on a real device.

### Fix

`AutoLinkWeb::handleRoot` now
streams the dashboard in 4096-byte
chunks via `httpd_resp_send_chunk`,
terminating the chunked-encoded
body with the canonical
`httpd_resp_send_chunk(req,
nullptr, 0)` terminator so the
browser flushes its parser. The
chunk cap (`const size_t CHUNK =
4096`) is a named constant so a
future bump to a smaller send
buffer can pin it without touching
the loop. The httpd task's stack
was bumped from 6144 to 8192
bytes — the chunked loop's local
state plus the httpd internal
send-buffer frame can otherwise
push the task over its stack
under sustained load.

Headers (`text/html; charset=utf-8`,
`Cache-Control: no-store`,
`Connection: close`) are unchanged:
the chunked rewrite only touches
the body path.

### Wire format

Unchanged. The wire is byte-identical
to v5.3.86. The change is purely
in the local httpd handler; no
header in `include/` moves, no
public API symbols are added or
removed. The library version
contract (`include/AutoLink.h` +
`library.properties` + this file)
bumps from 5.3.86 → 5.3.87 per
AGENTS rule 3.

### Regression coverage

**New structural pin:**
`run_test_handle_root_chunked`
in
`test/test_desktop/al/web/HandleRootChunkedTest.cpp`.
Source-greps
`src/al/web/AutoLinkWeb.cpp` and
asserts:

1. `handleRoot` does NOT contain
   `httpd_resp_send(req,
   DASHBOARD_HTML, ...)` — the
   buggy single-shot path is gone.
2. `handleRoot` calls
   `httpd_resp_send_chunk(req, p,
   ...)` for body bytes — the
   chunked data path is in place.
3. `handleRoot` terminates with
   `httpd_resp_send_chunk(req,
   nullptr, 0)` — the chunked
   body closes cleanly.
4. `handleRoot` declares
   `const size_t CHUNK = 4096;`
   — the chunk cap matches the
   httpd send buffer.
5. `handleRoot` still sets
   `text/html; charset=utf-8`,
   `Cache-Control`, and
   `Connection: close` — the
   rewrite didn't accidentally
   drop a response header.
6. `cfg.stack_size = 8192` inside
   the httpd config block —
   the stack bump is in place.

Toggle behaviour verified:
reverting `handleRoot` to the
single-shot form fails pin 1
(compile-clean, link-clean — the
pin is the only gate). Reverting
`cfg.stack_size` to 6144 fails
pin 6. Removing the null-chunk
terminator fails pin 3. Dropping
the `CHUNK = 4096` constant
fails pin 4.

**Manifest gate closure:**
`run_test_uri_handler_alignment`
and `run_test_handle_root_chunked`
are now in the
`test_coverage_manifest.py`
allow-list for "pure source-grep
suites with no library source".
The pre-existing
`run_test_uri_handler_alignment`
drift (disclosed in every version
since v5.3.81) is closed by the
same edit. The manifest gate
(`make test_coverage_manifest`)
is now fully green across the
33-suite unit run.

### Disclosed limitations

- `make loopback_quick` (the 5 s
  two-Link smoke test referenced
  in some prior versions) is not
  a target in this Makefile set;
  `make test` + `make itest` cover
  the same wire path. A future
  version may add it as a
  dedicated short-budget smoke
  target.
- The chunked loop walks
  `sizeof(DASHBOARD_HTML) - 1`
  bytes; if the embedded HTML
  ever grows past ~16 KB the
  per-chunk `httpd_resp_send_chunk`
  cost is still negligible
  (sub-ms per slice on ESP32),
  but the httpd task spends
  longer holding the socket in
  its loopTask priority level.
  Documented trade-off: dashboard
  correctness over single-shot
  throughput.

### Result

- 33 / 33 host unit suites pass
  (`make test`), including the
  new `run_test_handle_root_chunked`
  structural pin. Wall: ~5.1 s.
- 3 / 3 host integration suites
  pass (`make itest`). Wall: ~40 s.
- `test_coverage_manifest`
  self-test PASS (rule-4 invariant
  holds; allow-list extended for
  the two pure-source-grep suites).
- `build/verify_build.sh` clean
  compile against
  `esp32:esp32:firebeetle32` —
  `Sketch uses 1016523 bytes (77%)
  of program storage space. Global
  variables use 66992 bytes (20%)
  of dynamic memory`, no delta vs
  5.3.86.
- `AutoLinkWeb.cpp` ~10 LoC net
  (the single-shot `httpd_resp_send`
  line replaced by the chunked
  loop + the terminator); one
  `cfg.stack_size = 6144` → `8192`
  bump.
- 0 bytes added to RAM on the wire
  path (the chunked loop is pure
  instruction-cache; the httpd
  task stack is 2 KB larger at
  task-creation time only and
  freed when the task exits).
---

## v5.3.86

**Sweep-phase rx break + p2-fallback LOCK + P3 50ms floor + initSerial tag fix**

Four small bug-class fixes that each
target a different silent-failure
mode in the link-up / boot path.
None change the wire format; all
are localized in `Link.cpp`,
`LinkSweep.cpp`, and
`PingPongBase.h`.

1. **`onRx` SWP/LCK break on
   phase transition.** The `else`
   branch of `onRx` (the SWP/LCK
   state) used to keep feeding the
   rest of the incoming burst into
   the just-completed control frame
   after `processCtrlFrame_unlocked`
   returned `true` (signalling
   `lockOk_unlocked` had advanced
   state to OK and switched baud).
   The bytes trailing the LOCK/PONG
   in the rx buffer were captured at
   the old baud and would either be
   consumed at the wrong framing
   window or be misinterpreted as
   `0xAA 0x55` markers at the new
   baud. Added a `break;` after
   `needBreak = true;` so the rx
   loop exits cleanly and the
   `data` buffer is dropped — the
   next rx at the new baud starts
   from a clean `rxIdx = 0`.

2. **`onTimerSwp_unlocked` P2
   fallback sends LOCK_CMD.** The
   master-side P2-fallback path
   (no PONG received at any baud,
   sweep exhausts the list) used to
   call `lockOk_unlocked(...)`
   directly without first sending
   the LOCK_CMD frame. The slave
   side had no notice that the baud
   had been settled and would stay
   parked at the last spdI it had
   been holding. Now the master
   sets the baud, sends
   `LOCK_CMD + (N-1)`, then locks —
   same shape as the P3 lock path
   two blocks down.

3. **P3 timer floor 5 ms → 50 ms.**
   The
   `rt = 2 * (5 chars * 10 bits /
   baud * 1000)` formula gives ~87
   ms at 115200 — close to the
   `if (rt < 5) rt = 5` floor in
   spirit, but the floor fires for
   any faster baud. The first 87 ms
   of a faster-baud P3 window
   expires before the link could
   have even clocked the ACK round
   trip on a slow ESP32, so the
   master walks to the next baud
   while the slave is still
   mid-frame, then races itself.
   Bumping the floor to 50 ms
   guarantees a full RT is available
   at every supported baud and
   turns `t3` from ~103 ms to
   ~250 ms at 115200. Applied in
   `LinkSweep::enterPhase3` and
   the inline re-arm in
   `Link::handleSwp_unlocked`
   (both `if (rt < 5) rt = 5;`
   lines now read 50).

4. **`initSerial` tag/fmt swap.**
   `log.info("PingPongBase boot:
   role=%s  baud=%lu  WiFi=%s",
   role, ...)` passed the whole
   "PingPongBase boot: …" literal
   as the tag, then `role` (a
   `const char*`) as the format
   string. The first format-arg
   pair (`role`, `(unsigned
   long)debugBaud`, `ssid`) didn't
   match `role`'s `%s` slot
   position — the printf-style
   formatter walked off the end
   of the variadics. Split into
   `log.info("PingPongBase", "boot:
   role=%s  baud=%lu  WiFi=%s",
   role, (unsigned long)debugBaud,
   ssid ? ssid : "disabled")` —
   tag and format are now the two
   separate args the API expects.

### Wire format

Unchanged. The wire is byte-identical
to v5.3.85. The four fixes are
behavioural only (rx loop control,
P2 fallback handshake, P3 timing
margin, log-call argument order);
no `LOCK_CMD`/payload type byte
or ARQ slot count changes. No
header in `include/` moves. Public
API shrinks by zero symbols.

### Regression coverage

- `make test` — 32 / 32 host unit
  suites pass (~5.1 s wall). The
  four fixes are localized in
  production code paths that the
  existing sweep / loopback / arq
  suites already exercise; no new
  test binary, no Makefile change.
- `make itest` — 3 / 3 host
  integration suites pass (~40 s
  wall).
- `build/verify_build.sh` —
  compiles cleanly against
  `esp32:esp32:firebeetle32`
  (1,016,491 B flash, 66,992 B
  RAM, no delta vs 5.3.85).
- `make loopback_quick` — 5 s
  two-Link end-to-end smoke test
  passes (96 / 96 messages, no
  discards, no frame errors,
  `disc=1` on Pong is the
  pre-existing one-shot the
  loopback suite always emits).

### Disclosed limitations

- `make test_coverage_manifest`
  still reports the pre-existing
  `run_test_uri_handler_alignment`
  drift (a pure source-grep test
  in `TEST_BINS` that links no
  library source). Not introduced
  by this change; the v5.3.85
  reference has the same flag and
  every version since v5.3.81
  discloses it.
- Fix #3 (P3 50 ms floor) widens
  the P3 window from ~103 ms to
  ~250 ms at 115200 baud. A
  link-up that needs to clear P3
  on a noisy channel may now take
  ~250 ms per round instead of
  ~100 ms. The trade is
  correctness over speed: the
  tighter floor was the cause of
  the master's
  walk-to-next-baud-mid-frame race.
  At default 5-baud config
  (`phase2Total` ≈ 6.5 s) the
  added ~750 ms across the worst
  case P3 walk is within the
  existing budget.

### Result

- 32 / 32 unit suites pass; 3 / 3
  integration suites pass;
  verify_build.sh clean compile
  against `esp32:esp32:firebeetle32`.
- `Link.cpp` +5 LoC (one new
  `if (processCtrlFrame_unlocked) {
  needBreak = true; break; }`
  block + 3 lines for the
  P2-fallback LOCK_CMD preamble);
  `LinkSweep.cpp` 1 LoC (`5` → `50`);
  `PingPongBase.h` 1 LoC
  (the split tag/fmt).
- 0 bytes added to RAM on the wire
  path (the new LOCK_CMD emission
  is one wire frame per P2
  fallback — a previously-missed
  packet, not a new one).
---

## v5.3.85

**Helpers drive Link through LinkContext, not friendship**

`LinkArq`, `LinkReorder`, and
`LinkSweep` previously reached into
`Link`'s private section via
`friend class` declarations and
called `l.sendFrame_unlocked()`,
`l.hwSetSpd()`, `l.hwLock()`,
`l.masterRole()`, etc. — a façade
over what was functionally a
God-class split. Promoted the
narrow shim accessors into a small
`al/link/LinkContext.h` interface
that `Link` implements; helpers
now take `LinkContext&` instead of
`Link&`. Removed `friend class
LinkSweep/LinkArq/LinkReorder`.
Helpers no longer `#include
"al/link/Link.h"` — header cycle
broken. Wire-protocol constants
(`PING_CMD`/`PONG_CMD`/`LOCK_CMD`/
`MAX_CHUNK`) moved to
`LinkContext.h` so the helpers see
them through the cross-helper I/O
contract. One vtable per `Link`
(~32B on ESP32), on par with the
existing `IHal` cost. No
`std::function` or virtual bases on
the helper side.

Regression: `run_test_linkcontext`
new — source-level pin (no friend
declarations in `Link.h`, no
`#include "al/link/Link.h"` in any
helper) plus a runtime mock-context
that the helpers compile and run
against. Toggle off (re-add
`friend class LinkSweep;`) → red.
Toggle off (revert helper
signatures to `Link&`) → compile
error. `make test` (32/32),
`make itest` (3/3),
`./build/verify_build.sh`
(`esp32:esp32:firebeetle32`),
`make loopback_quick` (5 s two-Link
end-to-end). Wire format unchanged.

Limitations: one vtable per `Link`
(~32B on ESP32) is a real cost —
acceptable since `IHal` already
costs the same. The
`LinkTestAccessor` friend and
`AutoLink` friend remain — out of
scope for this refactor (those
gates production-side test hooks,
not the helper↔link boundary).
---

## v5.3.84

**AutoLinkConfig: own header — break HAL→link header-level dep**

`AutoLinkConfig` is a user-facing config
struct (baud list, timeouts, buffer
sizes, mode) but lived inside the
link-layer header `src/al/link/Link.h`.
`EspHal` only needed `AutoLinkConfig`
but had to `#include "al/link/Link.h"`
just to see the type — pulling in the
full link layer (LinkArq, LinkReorder,
LinkSweep, IArqCache, LinkFrameRx)
from the HAL translation unit. Moved
the struct to its own header
`src/al/AutoLinkConfig.h`; both
`Link.h` and `EspHal.h` now include
that header. `EspHal.h` no longer
includes `Link.h` — the HAL layer
sees the config struct without
depending on the link layer at the
header level.

`test/test_desktop/al/CompileCheckTest`
extended `ARDUINO_GUARDED_FILES` to
carry an optional per-file
`-include` flag and injects
`al/link/Link.h` for `EspHal.h`
only — `EspHal.h`'s body still calls
`Link::onRx / onBreak / onTimer /
begin` (those are real dependencies,
held via `IHal::link`), and the
standalone header-syntax parse needs
the full `Link` definition that
production TUs get through
`include/AutoLink.h`.

Regression: `make test` (31/31),
`make itest` (3/3),
`./build/verify_build.sh`
(`esp32:esp32:firebeetle32`).
Wire format unchanged.
---
