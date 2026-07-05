# 📅 AutoLink Version History

All releases, most recent first.
## v6.0.40

**Fix three OK-timer wedges (double unlock, pause kills the tick, idleTimeoutMs<=0 kills ARQ), make MockHal one-shot-faithful, strip historical comments**

Cleanup pass that turned into a bug hunt. The FreeRTOS link timer is one-shot (`xTimerCreate(..., pdFALSE, ...)`), so every OK-tick exit must re-arm — and two early returns in `onTimerOk_unlocked` didn't. (1) `linkPaused_` returned without re-arming: a dashboard Pause during OK killed the tick permanently, and Resume only flipped the flag — after any pause/resume cycle the link ran with no ARQ retransmit sweep, no reorder expiry, and no health watchdogs. (2) `cfg.idleTimeoutMs <= 0` returned before everything: disabling the idle watchdog also disabled retransmission entirely. Both now re-arm; idleTimeoutMs<=0 skips only the `decideHealth` verdict while the reorder/ARQ machinery keeps running. (3) Every watchdog drop path did `hw.unlock(); hw.sendBreak(); return;` inside a callee that `onTimer` unlocks again after — a double unlock (UB on the host `std::mutex`, a failed give on the device mutex). `onTimerOk_unlocked`/`onTimerLck_unlocked` now return a break-needed bool and `onTimer` owns the single unlock + post-unlock `sendBreak`. The host suite never caught any of this because `MockHal::pumpClock` re-fired a stale one-shot arm up to 16× per pump — effectively a broken auto-reload timer — so a callee that failed to re-arm still ticked on the host while dying on the device; the mock now consumes the arm on fire exactly like the hardware timer, and the whole suite stays green under the faithful semantics. Separately, the widened version-free source gate (majors 5–9) flushed out twelve hard-coded release references in test comments/banners; all scrubbed. The comment strip removed historical anchors ("pre-fix shape", "this release", step narration) across the link core, HAL, pingpong roles, facade, and web handlers per the developer-doc comment policy, keeping only why/wire-side/pin rationale — e.g. `Ping.h` 297 → 83 comment lines, `EspHal.h` 135 → 94 — verified comment-only by a strip-and-compare diff on every touched file except the three with intended code changes.

### What moved

- `src/al/link/LinkTimers.cpp` — `onTimerOk_unlocked`/`onTimerLck_unlocked` return `bool` (break-needed); paused tick re-arms; `idleTimeoutMs<=0` no longer skips the ARQ sweep; drop paths no longer unlock/break inline; `onTimer` single-unlock + post-unlock BREAK; dead `cur` locals removed; comments condensed.
- `src/al/link/Link.h` — the two signature changes; comment trims.
- `test/common/MockHal.h` — `pumpClock` consumes the one-shot arm on fire (was: stale re-fire masking rearm bugs).
- `test/test_desktop/al/link/LinkTimerRearmTest.cpp` (new) + Makefile registration (`run_test_timer_rearm`) — pins pause-rearm and idle-disabled-tick-alive; toggle-off → red (verified).
- `test/test_desktop/al/link/LinkAckBytesTest.cpp` — Pin 3 source-grep updated to the health-monitor shape (`bool onTimerOk_unlocked`, `decideHealth`, `reset_unlocked` → `return true`).
- Twelve version-string scrubs across six test files (version-free gate green).
- Comment strips: `LinkRx.cpp`, `LinkSweep.cpp`, `LinkTx.cpp`, `LinkContext.h`, `Ping.h`, `Pong.h`, `PingGap.h`, `EspHal.h`, `include/AutoLink.h`, `AutoLinkWebHandlers.cpp` (comment-only, machine-verified).
- `include/AutoLink.h` / `library.properties` / `idf_component.yml` — `6.0.39 → 6.0.40` lockstep.

### Wire format

Unchanged. Same framer, COBS encap, `MAX_CHUNK = 250`, `MSG_HDR = 6`, `POOL_SIZE = 64`, window 32, seq-space budget as 6.0.39. Behavior changes are timer-lifecycle only: the OK tick now survives pause/resume and idleTimeoutMs<=0, and BREAK is sent after the lock is released rather than between a double unlock.

### Regression tests

- `LinkTimerRearmTest` — Pin 1: paused OK tick re-arms and resume keeps ticking; Pin 2: idleTimeoutMs=0 keeps the tick alive and holds a healthy link. Toggle-off (restore either early return) → red (verified).
- MockHal one-shot fidelity is itself a suite-wide hardening: any future exit path that forgets to re-arm now fails on the host instead of only on the bench.
- All prior pins green through the new dispatch: 70/70 unit, 4/4 itest.

### Disclosed limitations

- `build/verify_build.sh` not run in-sandbox (no `arduino-cli`/network); source touched — cross-compile carry-over stands, last cleared end-to-end at 6.0.32 (todo Open 1).
- `clang-format` unavailable; `pretty_print.py` merge pass ran, formatting skipped.
- The double unlock was almost certainly benign on-device (FreeRTOS rejects a give by a non-holder) but UB on the host; fixed for both.

### Result

- `make test` 70/70 unit (+`run_test_timer_rearm`), `make itest` 4/4 (~45 s), `test_coverage_manifest` PASS, version-free gate green, `version.py check` green (20 entries).
- Comment-only verification: `gcc -fpreprocessed` strip-and-diff clean on every comment-strip file; code deltas confined to `LinkTimers.cpp`, `Link.h`, `MockHal.h`, and the two test files.
---

## v6.0.39

**Extract the unified link-health monitor: one pure decideHealth replaces the scattered OK-timer watchdog branches**

Closes todo Open 2 (and subsumes Open 5). The 6.0.32/33/35 bug family shared one shape: keep/drop decisions lived as separate, mode-gated branches inside `onTimerOk_unlocked` (`LinkTimers.cpp`), so every fix added a branch and every branch another place to be silently gated out of a mode — the SYNC stall wedge existed precisely because the tx-backpressure watchdog sat inside an `if (mode != SYNC)` block. This release moves all four OK-state watchdog decisions — tx-backpressure stall (both modes), asymmetric idle, symmetric idle, and ARQ pool exhaustion (ASYNC only) — into one pure free function `decideHealth(HealthState, nowMs, idleTimeoutMs)` in the new header-only `src/al/link/LinkHealth.h` (the `LinkDecision.h` pattern: no state, no I/O, enum out, exhaustively table-tested per AGENTS rule 21). `onTimerOk_unlocked` now snapshots one `HealthState` (reject streak, rx/tx ages, ARQ pending, frameErrs, pool room, mode) and dispatches on the returned action; the four log lines and the single drop path (`reset_unlocked(true)` → BREAK) are unchanged, so wire behaviour is byte-identical — the decision logic just can't fragment again. `FAST_IDLE_RX_MS`/`FAST_IDLE_TX_MS`/`POOL_EXHAUST_DROP_PENDING` moved into the health module; `LinkTimers.cpp` shrank 477 → 338 lines and now holds only timer plumbing, which is what Open 5's split was for. Behavior-preserving: check priority (stall → asym → sym-idle → pool → retx sweep), all thresholds, and the SYNC gating are exactly the pre-refactor semantics, and every existing watchdog pin stays green. Wire format unchanged.

### What moved

- `src/al/link/LinkHealth.h` (new) — `HealthState`, `HealthAction`, pure `decideHealth()`, threshold accessors; rationale comments consolidated here.
- `src/al/link/LinkTimers.cpp` — the tx-stall / asym-idle / sym-idle / pool-exhaust branch blocks replaced by one snapshot + `decideHealth` dispatch with per-reason logging; module-local watchdog constants removed (owned by LinkHealth.h); 477 → 338 lines.
- `test/test_desktop/al/link/LinkHealthTest.cpp` (new) — 19-row truth table.
- `test/test_desktop/Makefile` — `run_test_health` in `TEST_BINS` + build rule (header-only, links no library source).
- `test/scripts/coverage/test_coverage_manifest.py` — `run_test_health` added to the source-grep-only exempt list.
- `todo.md` — Open 2 closed, Open 5 subsumed; remaining items renumbered (bench/verify loop; SYNC instant resync; ASYNC delivery floor).
- `include/AutoLink.h` / `library.properties` / `idf_component.yml` — `6.0.38 → 6.0.39` lockstep bump (AGENTS rule 3).

### Wire format

Unchanged. Decision relocation only; no new frames, same framer, COBS encap, `MAX_CHUNK = 250`, `MSG_HDR = 6`, `POOL_SIZE = 64`, window 32, seq-space budget as 6.0.38.

### Regression tests

- `LinkHealthTest` — 19 rows spanning all four drop reasons, both modes, the clean-quiet-link false-positive guard, stale-streak hold, and check priority; toggle-off (re-gate the stall check behind `!sync`) → SYNC-streak row red (verified).
- `LinkSyncStallWatchdogTest` / `LinkTxStallWatchdogTest` and the idle/asym pins — unchanged, still green through the new dispatch; the same toggle-off also turns `LinkSyncStallWatchdogTest` red end-to-end (verified), proving the integration path runs through `decideHealth`.

### Disclosed limitations

- `build/verify_build.sh` could not run in-sandbox (no `arduino-cli`, no network). Source WAS touched (`LinkTimers.cpp`, host-tested link core + one new header) — the cross-compile carry-over stands; still last cleared end-to-end at 6.0.32. Head of todo Open 1.
- `clang-format` unavailable in-sandbox; `build/pretty_print.py` ran its merge pass but skipped formatting on the touched files. Re-run with clang-format available.
- The monitor centralizes decisions made on the OK-timer tick; event-time signals still feed it via the existing members (`noteTxReject_unlocked`, `lastRxMs`/`lastTxMs`, ARQ counters). Migrating ownership of those counters into `HealthState` is deliberately out of scope — no behavior would change.

### Result

- `make test` 69/69 unit (~6.7 s wall; +`run_test_health`, toggle-off → red verified), `make itest` 4/4 (~45.1 s wall).
- `make test_coverage_manifest` PASS; `python3 build/version.py check` green (20 entries; this entry pushed the oldest off the tail).
- `build/verify_build.sh` not run — see Disclosed limitations.
---

## v6.0.38

**todo.md architecture-hardening reorg + Version.md tail trim (docs only)**

Documentation-only housekeeping in the 6.0.23/6.0.28/6.0.31 shape — no `.cpp`/`.h`/`.ino` touched, protocol and build surface byte-identical to 6.0.37. The trigger: weeks of instability whose fixes (6.0.32–6.0.37) each patched one wedge but left the underlying architecture exposed, so `todo.md` is rebuilt as the minimum plan that makes the link bulletproof rather than a single bench note. The failure history reads as three structural causes and one process failure, and the new Open list maps onto them one-to-one. (1) Keep/drop/resweep decisions are scattered mode-gated branches across `LinkTimers.cpp`/`LinkApi.cpp` — the 6.0.32/33/35 bug family was, every time, "a watchdog branch that didn't run in some mode / didn't stamp the streak"; Open 2 extracts one pure, table-tested `decideHealth` fed by one `HealthState` so a missed branch becomes structurally impossible. (2) SYNC's length-prefixed framer wedges on a lost mid-message ACK and recovery waits out `idleTimeoutMs` (the 6.0.35 disclosed limitation); Open 3 makes the resync fire on the failure itself (immediate BREAK on a mid-message `waitForAck` timeout). (3) ASYNC delivery tops out at ~74 % under 1 % frame loss (the 6.0.34 result) because the fixed `reorderHoldMs` expires before retransmission closes the gap; Open 4 derives the hold from measured RTO × `maxRetx` and pins per-loss-rate delivery floors with a loss-sweep itest. The process failure is the cross-compile gate: `verify_build.sh` last cleared at 6.0.32, five source-touching releases shipped device-path-unverified, and 6.0.37 exists only because the host stubs cannot catch Arduino-only breakage — so Open 1 is now "restore the verification loop first", bundling the standing FireBeetle bench items (re-lock cadence, walk-down, ASYNC-flood wedge, stall watchdog end-to-end, heap-cap log, both OTA uploads). Open 5 is the follow-through readability split of `LinkTimers.cpp` once Open 2 lands. No completed items were moved out of `todo.md` — the prior file held only the bench item and it remains open inside Open 1; this pass adds what was missing, orders by priority, and keeps nothing done.

### What moved

- `todo.md` — rebuilt: five Open items, priority-ordered (verify loop + bench backlog; unified health monitor; SYNC instant resync; ASYNC delivery floor; `LinkTimers.cpp` split), each with symptom, file anchors, fix direction, and pin recipe. Verify footer retained.
- `docs/Version.md` — this entry; `add` + `trim --keep 20` dropped the oldest entry (v6.0.17) off the tail.
- `include/AutoLink.h` / `library.properties` / `idf_component.yml` — `6.0.37 → 6.0.38` bump in lockstep (AGENTS rule 3).

### Wire format

Unchanged. No source touched. Same framer shape, COBS encap, `MAX_CHUNK = 250`, `MSG_HDR = 6`, `POOL_SIZE = 64`, window 32, and seq-space budget as 6.0.37.

### Regression test

None added — docs-only, no behavior to pin. The host suite is the gate, unchanged from 6.0.37, re-run this session (below).

### Disclosed limitations

- Standing cross-compile carry-over: `build/verify_build.sh` not run in-sandbox (no `arduino-cli`, no network). No source delta on top of 6.0.37, so risk stays bounded to the already-disclosed surface; last cleared end-to-end at 6.0.32. Now the head of todo Open 1.
- All five Open items are open — this release plans the work, it does not perform any of it.

### Result

- No source touched; host suite re-run this session: `make test` 68/68 unit (~7.0 s wall), `make itest` 4/4 (~45.2 s wall), combined ~52.2 s.
- `python3 build/version.py check` green (20 entries, --keep=20; this entry pushed v6.0.17 off the tail).
- `build/verify_build.sh` not run in-sandbox — see Disclosed limitations.
---

## v6.0.37

**Fix `OtaCore::NAME_MAX` ↔ `<limits.h>` macro collision + coverage-manifest exempt-list drift**

Build-only release — the 6.0.36 OTA path failed to cross-compile on Arduino-ESP32 because `OtaZipStream::NAME_MAX = 96` clashed with `NAME_MAX` from `<limits.h>` (pulled in transitively by `<freertos/FreeRTOS.h>` → `portmacro.h` → `limits.h`); the preprocessor substituted the macro numeric constant ahead of the `constexpr` declarator and produced `error: expected unqualified-id before numeric constant`. `NAME_MAX` is a `<limits.h>` macro on every POSIX-y libc the Arduino toolchain ships, so the collision is guaranteed on the device path. `OtaCoreTest` (host compile, no `freertos/`) never triggered it. Fix: rename to `kNameMax` in `OtaCore.h` and the two call sites in `OtaCore.cpp` (`OtaZipStream::kNameMax` for the size guard, bare `kNameMax` inside the class); a comment records why. Companion: `run_test_heap_cap_floor` and `run_test_ota_core` were missing from the `test_coverage_manifest.py` exempt list (they link only their own test .cpp / the host-side `OtaCore.cpp`), tripping AGENTS rule 4 (`make test_coverage_manifest` red); both added. No protocol, framer, or surface-area change.

### What moved

- `src/al/web/OtaCore.h` — `static constexpr size_t NAME_MAX = 96;` → `kNameMax` (with comment recording the limits.h collision).
- `src/al/web/OtaCore.cpp` — two references updated to the new name; one is qualified (`OtaZipStream::kNameMax`) for the public guard, one bare (`kNameMax`) for the in-class length check.
- `test/scripts/coverage/test_coverage_manifest.py` — exempt list extended with `run_test_heap_cap_floor` and `run_test_ota_core` (per the existing source-grep-only exemption pattern; no new `TEST_BINS` rules).
- `include/AutoLink.h` / `library.properties` / `idf_component.yml` — `6.0.36 → 6.0.37` lockstep bump (AGENTS rule 3).

### Wire format

Unchanged. Rename + manifest exemption only; no new frames, same framer, COBS encap, `MAX_CHUNK = 250`, `MSG_HDR = 6`, `POOL_SIZE = 64`, window 32, seq-space budget as 6.0.36.

### Regression tests

- `OtaCoreTest` — unchanged in body; the rename is exercised by every name-sanitizing case (the `OtaZipStream::NAME_MAX` reference inside the guard) and by the zip-stream "name too long" reject (`nameLen_ > kNameMax`). Re-linking passes (the host tree never pulled `<limits.h>` via the test path, so this is a coverage-floor pin — the assertion that the new name is the only one referenced).
- `test_coverage_manifest.py` — `test_real_makefile_covers_every_test_bin` now exempts the two new source-grep-only bins; toggle-off (drop the exemptions) → red.

### Disclosed limitations

- `build/verify_build.sh` still could not run in-sandbox (no `arduino-cli`, no network — the 6.0.36 disclosed limitation carried forward). The whole reason this release exists is the cross-compile failure `verify_build.sh` would have caught — confirmed the host gate misses it by construction (the host stub set substitutes `FreeRTOS.h` but does not transitively pull `<limits.h>`), so the **fix is verified by the host rename + a manual review of the preprocessor name** but the Arduino-side compile is still unverified. Re-run `verify_build.sh` against `esp32:esp32@3.3.5` before flashing.
- The 6.0.36 todo `Open 1` (re-lock cadence + walk-down + stall-watchdog bench confirm) remains the sole open item — FireBeetle-pair-only. No item moved.

### Result

- `make test` 68/68 unit (re-run, ~8.8 s wall); `make itest` 4/4 (re-run, ~45.2 s wall); `make test_coverage_manifest` PASS (re-run).
- `python3 build/version.py check` green (20 entries; `add` + `trim --keep 20` dropped v6.0.16 off the tail).
- `build/verify_build.sh` not run — see Disclosed limitations.
---

## v6.0.36

**Close Open 1 + Open 3 (heap-aware buffer floors; real OTA firmware + GUI upload) and add the multi-chunk ASYNC-under-loss itest**

Three deliverables. (1) **Open 1, ASYNC heap headroom**: `EspHal::begin()` now caps its two big asks — the RX stream buffer and the ASYNC UART rx buffer — against `esp_get_free_heap_size()` via the new pure helper `capFloorByHeap(want, minFloor, freeHeap, reserve)`, leaving `cfg.heapReserveBytes` (default 16 KB) untouched for WiFi/httpd/OTA. The stream buffer never shrinks below one full coalesced message (`maxMsg + 6`) and the rx buffer never below its SYNC baseline (`cfg.rxBufferSize`) — better to eat the reserve than come up broken; a shrink is logged. Heap 0 (host stub) and reserve 0 disable the cap. (2) **Open 3, OTA**: the 6.0.22-era 503 stubs at `/ota/fw` and `/ota/gui` are now real. `/ota/fw` streams the raw body via `httpd_req_recv` (chunk buffer sized by `otaRecvChunkBytes`, unblocked by (1)) into the inactive app slot with `esp_ota_begin/write/end`, sets the boot partition, replies, and reboots; the first web-monitor boot afterwards marks the image valid (rollback-cancel). `/ota/gui` unpacks a **STORED** zip (`zip -0`) through the new streaming parser `OtaZipStream` into LittleFS under `/web/`, and `handleRoot` serves `/web/index.html` from FS when present, else the baked-in `AutoLinkWebHtml.h`. Every error path drains the unread body first (`drainBody_`), closing the half-read-socket limitation the stub carried since 6.0.22. All OTA decision logic (slot policy, chunk sizing, entry-name traversal guard, zip state machine including feeds split at every byte boundary) lives in the host-linkable `src/al/web/OtaCore.{h,cpp}`; the esp_ota_*/LittleFS I/O is cross-compile-only, and the host stub set now parses both web TUs (CompileCheckTest gate extended). Requires a dual-app-slot + LittleFS partition table — documented in `docs/WebMonitor.md`. (3) **6.0.34 follow-up itest**: `run_loopback_multichunk` sends random 300-3000 B (2-13 chunk) ASYNC payloads under 1 % frame loss with byte-keyed content verification — the exact coverage gap that let the retransmit throttle ship. Open 2 (re-lock cadence symmetry + walk-down + stall-watchdog bench confirm) remains the sole open item: it is FireBeetle-pair-only and its dwell relationships are pinned by existing guard tests, so no blind cadence change was made. Wire format unchanged.

### What moved

- `src/al/AutoLinkConfig.h` — `heapReserveBytes` field; pure `capFloorByHeap()`.
- `src/al/hal/EspHal.h` — heap-aware cap applied in `begin()` (stream buf first, then rx buf, subtracting each grant), warning log on shrink.
- `src/al/web/OtaCore.h` / `OtaCore.cpp` (new) — `otaInactiveSlot`, `otaRecvChunkBytes`, `otaSafeEntryName`, `OtaZipStream` (streaming STORED-only zip; rejects compressed / data-descriptor entries).
- `src/al/web/AutoLinkWebHandlers.cpp` — real `handleOtaFw` / `handleOtaGui` replace the 503 stubs; `drainBody_`; `handleRoot` LittleFS-first serving.
- `src/al/web/AutoLinkWeb.h` / `AutoLinkWeb.cpp` — `fsOk_` + `mountFs_()`; LittleFS mount + OTA rollback-valid mark in `setupHttpAndLogging_`.
- `test/scripts/env/install_system_stubs.py` — `esp_ota_ops.h` + `LittleFS.h` stubs; httpd stub gains `content_len`, `httpd_req_recv`, `HTTPD_SOCK_ERR_TIMEOUT`; `CompileCheckTest` expected-symbol list extended to pin them.
- `test/test_desktop/al/hal/HeapCapFloorTest.cpp` (new), `test/test_desktop/al/web/OtaCoreTest.cpp` (new), `test/itest/test_desktop/al/link/loopback_multichunk_test.cpp` (new) + the two Makefile registrations.
- `docs/WebMonitor.md` (OTA endpoints, partition requirements, curl examples), `docs/Tests.md` (itest table).
- `include/AutoLink.h` / `library.properties` / `idf_component.yml` — `6.0.35 → 6.0.36` lockstep bump (AGENTS rule 3).

### Wire format

Unchanged. Buffer sizing and HTTP surface only; no new frames, same framer, COBS encap, `MAX_CHUNK = 250`, `MSG_HDR = 6`, `POOL_SIZE = 64`, window 32, seq-space budget as 6.0.35.

### Regression tests

- `HeapCapFloorTest` — 9-row truth table for `capFloorByHeap`; toggle-off (return `want` unconditionally) → tight-heap rows red (verified).
- `OtaCoreTest` — slot policy, chunk sizing, name traversal guard, zip stream incl. split feeds at every byte boundary and deflate / data-descriptor / bad-signature rejects; toggle-off (drop the method reject) → red (verified).
- `run_loopback_multichunk` — delivery floor 30 % + zero-corruption + at-least-one-multi-chunk pins; toggle-off (retx no-op) collapses delivery to ~2 % (the documented pre-6.0.34 shape) → red (verified).

### Disclosed limitations

- `build/verify_build.sh` could not run in-sandbox (no network). Source WAS touched (`EspHal.h`, web TUs, new `OtaCore.cpp`) and the OTA I/O paths (`esp_ota_*`, LittleFS) only compile in the Arduino build — the cross-compile carry-over re-opens and is **mandatory** before flashing. Both web TUs parse cleanly under the extended host stub set (CompileCheckTest), which checks syntax/types but not the real IDF signatures. Last cleared end-to-end at 6.0.32.
- The OTA paths are host-pinned at the decision layer only; the stream-to-flash and LittleFS writes need on-device verification (upload a real image, power-cycle, confirm rollback-cancel; upload a `zip -0` GUI, confirm `/web/index.html` serves).
- GUI zips must be STORED (`zip -0`); deflate is rejected by design (no streaming inflate budget on a ~39 KB-heap target).
- Open 2 (re-lock cadence + walk-down + stall-watchdog bench confirm) remains open — FireBeetle-pair-only.

### Result

- `make test` 68/68 unit (+`run_test_heap_cap_floor`, +`run_test_ota_core`; both toggle-off → red verified), `make itest` 4/4 (~45.2 s, +`run_loopback_multichunk` ~5 s).
- `python3 build/version.py check` green (20 entries; this entry pushed the oldest off the tail).
- `build/verify_build.sh` not run — see Disclosed limitations.
---

## v6.0.35

**Fix SYNC self-healing: a peer-desync wedge (lost mid-message ACK) now drops + resweeps instead of hanging forever**

SYNC mode had no self-healing at all. The entire idle / asymmetric / tx-backpressure watchdog block in `onTimerOk_unlocked` was gated behind `if (mode != SYNC)`, on the assumption that SYNC's inline `waitForAck` timeout was "its own recovery path" — but that only fails one send; it never drops or resweeps the link. Failure shape on the FireBeetle pair (random size, 500 ms delay, SYNC): a CRC/desync burst costs one mid-message ACK, so the sender abandons that message partway. The receiver's length-prefixed framer is left waiting for a message tail that never comes, stops delivering/ACKing, and the sender's every subsequent `sendMsg` times out — `tx` falls to 0, `arqPending=0`, `frameErrs=0`, `disc=0`, and the link hangs forever (`send failed (backpressure) … arqPending=0` every ~1.5 s with no resweep). This is the same blind spot 6.0.33 closed for ASYNC, but SYNC was excluded because the reject-streak was only stamped on ASYNC reject branches. Fix: (1) a failed SYNC `sendMsg` now stamps the tx-reject streak (`noteTxReject_unlocked`), and (2) the tx-backpressure stall watchdog is hoisted out of the `mode != SYNC` gate so it runs in both modes. A SYNC sender whose sends stay un-ACKed past `idleTimeoutMs` (streak still live) now drops → BREAK → resweeps, which realigns both framers and resumes traffic. ASYNC behaviour is unchanged (same streak, same watchdog, just relocated ahead of the ASYNC-only idle checks). Recovery latency is `idleTimeoutMs` (default 10 s); lower `cfg.idleTimeoutMs` for a shorter SYNC stall-to-resweep window. Wire format unchanged.

### What moved

- `src/al/link/LinkApi.cpp` — `sendMsg` stamps `noteTxReject_unlocked()` on a SYNC failure (`else if (sync)` alongside the existing success-clears-streak line); ASYNC reject branches already stamped.
- `src/al/link/LinkTimers.cpp` — the tx-backpressure stall watchdog moved out of the `if (mode != SYNC)` block to run mode-independently, right after the reorder-expiry step in `onTimerOk_unlocked`; the ASYNC-only idle/asymmetric checks stay gated.
- `test/common/LinkTestAccessor.h` — `noteTxReject()` shim so the regression can drive the streak without spinning a real `waitForAck` timeout on the injected clock.
- `test/test_desktop/al/link/LinkSyncStallWatchdogTest.cpp` (new) + `test/test_desktop/Makefile` (`run_test_sync_stall_watchdog` in `TEST_BINS` + build rule).
- `include/AutoLink.h` / `library.properties` / `idf_component.yml` — `6.0.34 → 6.0.35` lockstep bump (AGENTS rule 3).

### Wire format

Unchanged. Watchdog scheduling only; no new frames, same framer, COBS encap, `MAX_CHUNK = 250`, `MSG_HDR = 6`, `POOL_SIZE = 64`, window 32, seq-space budget as 6.0.34. The recovery path (drop → BREAK → resweep) is the existing one.

### Regression test

`LinkSyncStallWatchdogTest` — two pins. Pin 1: a live tx-reject streak older than `idleTimeoutMs` drops a SYNC link (state → SWP, `discCount==1`) on the OK-timer tick; toggle-off (re-gate the watchdog to `mode != SYNC`) → the link stays OK → assertion fails (verified red). Pin 2: a single stale reject (no follow-up) does not drop a quiet SYNC link — the still-live gate holds.

### Disclosed limitations

- `build/verify_build.sh` could not run in-sandbox (no network). Source WAS touched (`LinkApi.cpp` / `LinkTimers.cpp`, host-tested link core only) — cross-compile carry-over re-opens; re-run before the next source-touching tag. Last cleared end-to-end at 6.0.32.
- The fix recovers the SYNC desync; it does not prevent it. SYNC's length-prefixed framing is inherently fragile to a lost mid-message ACK (the sender abandons the message partway and the stream desyncs). Recovery now takes up to `idleTimeoutMs`; a follow-up worth considering is an immediate BREAK on a mid-message `waitForAck` timeout so the resync is near-instant rather than watchdog-driven. For hard, low-latency delivery under lossy wires, ASYNC (with 6.0.34's retransmit fix) remains the more robust mode.

### Result

- `make test` 66/66 unit (new `run_test_sync_stall_watchdog`; toggle-off → red verified), `make itest` 3/3 (~40 s).
- `python3 build/version.py check` green (20 entries; this entry pushed the oldest off the tail).
- `build/verify_build.sh` not run — see Disclosed limitations.
---

## v6.0.34

**Fix ASYNC retransmit throttle: NAK-driven fast retransmit now fires inline, and the OK-timer sweep resends every RTO-expired frame per tick**

ASYNC multi-chunk delivery collapsed under any real wire loss: a dropped chunk was effectively never recovered, so random/multi-chunk traffic degraded to ~0 delivery while the link churned resweeps. SYNC was unaffected (it self-throttles inline via `waitForAck`, so no retransmit backlog ever forms) — which is exactly why sequential ran better than random. Root cause was retransmit throttling, not framing or CRC. Both loss signals fed a single deferred slot: `onNak` only stamped `pendingRetxBase_`/`hasPendingRetx_` for the next OK-timer tick (and each fresh NAK overwrote the prior base), and the ACK-timeout sweep resent one frame then `break`ed. At line rate the sender emits hundreds of frames per `okTickMs`; recovering one chunk per tick could never keep pace, the receiver's reorder window expired (`reorderHoldMs`) before the gap closed, multi-chunk messages never reassembled, and the ARQ eventually hit `maxRetx`/pool-exhaust → forced resweep, dropping everything in flight. The README already advertised "NAK-driven fast retransmit"; it was not fast. Fix: retransmit inline, not deferred. A NAK for a live pending seq now resends that cached frame immediately inside `onNak` (under the RX lock, the same lock `onPayload`'s ACK/NAK sends already take), and the OK-timer sweep now resends *every* RTO-expired pending frame in the pass instead of one-then-break. The single-slot `hasPendingRetx_`/`pendingRetxBase_` mechanism is removed; both call sites funnel through one `retxSeq_unlocked(seq)` helper. Host stress (random 1–1200 B ASYNC, byte-keyed integrity check) at 1 % per-frame loss: delivery ~2 % → ~74 % with **zero** silent corruption (every non-delivery is a CRC-safe `recvMsg` -1, as documented). Wire format unchanged.

### What moved

- `src/al/link/LinkTimers.cpp` — new `retxSeq_unlocked(seq)` (the cache-lookup + verbatim resend logic, lifted out of the old deferred tail in `onTimer`); the OK-timer ACK-timeout sweep loops all pending slots and resends each RTO-expired one inline (no `break`); `onTimer`'s post-unlock single-slot resend block deleted.
- `src/al/link/LinkRx.cpp` — `onNak` retransmits immediately via `retxSeq_unlocked(missingCobsSeq)` after `arq_.onNaked` restamps the RTO; the `pendingRetxBase_`/`hasPendingRetx_` stamping is gone.
- `src/al/link/Link.h` — `retxSeq_unlocked` declared; `hasPendingRetx_` / `pendingRetxBase_` fields removed.
- `src/al/link/LinkCore.cpp` — the two field clears dropped from `reset_unlocked`.
- `test/test_desktop/al/link/LinkFastRetxTest.cpp` (new) + `test/test_desktop/Makefile` (`run_test_fast_retx` in `TEST_BINS` + build rule).
- `include/AutoLink.h` / `library.properties` / `idf_component.yml` — `6.0.33 → 6.0.34` lockstep bump (AGENTS rule 3).

### Wire format

Unchanged. Sender-local retransmit scheduling only; no new frames, same framer, COBS encap, `MAX_CHUNK = 250`, `MSG_HDR = 6`, `POOL_SIZE = 64`, window 32, seq-space budget as 6.0.33. Retransmits are the existing verbatim cached frames; the receiver's Stale/dup handling already deduped them.

### Regression test

`LinkFastRetxTest` — three pins. Pin 1: after a multi-chunk ASYNC send, injecting a NAK for the pending base seq puts a retransmit on the wire *within the `onNak` call, before any timer tick*; toggle-off (defer to timer) → wire stays empty → assertion fails (verified red). Pin 2: a NAK for a non-pending seq emits nothing (pending guard intact). Pin 3: the retransmitted frame carries the NAKed seq.

### Disclosed limitations

- `build/verify_build.sh` could not run in-sandbox (no network; the arduino-cli toolchain download fails). Source WAS touched this release (`LinkTimers.cpp` / `LinkRx.cpp` / `Link.h` / `LinkCore.cpp`, all host-tested link core — no `AutoLinkWeb.cpp` or Arduino-only surface), so the cross-compile carry-over re-opens; re-run before the next source-touching tag. Last cleared end-to-end at 6.0.32.
- Integration coverage gap that let this ship: `loopback_noise_test.cpp` only sends fixed 64-byte (single-frame) payloads, so the multi-chunk ASYNC-under-loss path had no itest. The new unit pin plus host stress cover the fix; adding a random multi-chunk loss itest is recommended follow-up (kept out of this release to avoid growing itest wall time under the sandbox ceiling).
- Delivery under sustained heavy loss (≳5 % per frame on multi-chunk messages) is still low by nature — most messages lose at least one chunk faster than any retransmit budget; that is best-effort behaviour, not a regression. SYNC remains the choice for hard-delivery needs.

### Result

- `make test` 65/65 unit (new `run_test_fast_retx`, ~2 ms; toggle-off → red verified), `make itest` 3/3 (~40.1 s). Combined ~47 s.
- `python3 build/version.py check` green (20 entries; this entry pushed the oldest off the tail).
- `build/verify_build.sh` not run — see Disclosed limitations.
---

## v6.0.33

**Fix Open 1: TX-backpressure stall watchdog (silent ASYNC hang now drops + resweeps)**

Closes todo Open 1, the successor to the 6.0.27 disc-cascade fix: an ASYNC master wedged upstream of the ARQ pool rejected every `sendMsg` (`send failed (backpressure) n=… arqPending=0` every ~1.5 s) while `pendingCount()==0` and `frameErrs==0` — both existing watchdog branches were blind. The asymmetric stuck-send check (`LinkTimers.cpp`) gates on `pendingCount() > 0`; the symmetric idle check no-ops on `pendingCount()==0 && frameErrs==0`. A sender trying to send but unable looked identical to a clean idle link, so `tx` fell to 0, echos froze on both peers, `disc` held at 0, and the link never resweeped. Fix: the watchdog gets a third input — a consecutive `sendMsg` backpressure-rejection streak. `sendMsg` stamps first/latest reject times (`txRejFirstMs_`/`txRejLastMs_`) on every ASYNC reject branch (seq-space exhausted, cache full at entry, mid-message pool exhaustion) and clears the streak on any successful send; `reset_unlocked` clears it too. The OK-state timer drops + BREAKs + resweeps when the streak has outlived `idleTimeoutMs` **and** is still live (latest reject within `idleTimeoutMs`) — the still-live gate keeps a single stale transient reject from bouncing a healthy quiet link.

### What moved

- `src/al/link/Link.h` — `txRejFirstMs_` / `txRejLastMs_` streak stamps + `noteTxReject_unlocked()` inline helper, documented against the wedge shape.
- `src/al/link/LinkApi.cpp` — `noteTxReject_unlocked()` on the three ASYNC reject branches in `sendMsg`; streak cleared on successful return.
- `src/al/link/LinkTimers.cpp` — third watchdog branch in `onTimerOk_unlocked` (non-SYNC block, after the symmetric idle check): live streak > `idleTimeoutMs` → warn `tx backpressure stall -> drop`, `reset_unlocked(true)`, BREAK.
- `src/al/link/LinkCore.cpp` — streak cleared in `reset_unlocked` so a fresh lock starts clean.
- `test/test_desktop/al/link/LinkTxStallWatchdogTest.cpp` (new) + `test/test_desktop/Makefile` (`run_test_tx_stall_watchdog` in `TEST_BINS` + build rule; coverage manifest picks it up from `TEST_BINS`).
- `include/AutoLink.h` / `library.properties` / `idf_component.yml` — `6.0.32 → 6.0.33` lockstep bump (AGENTS rule 3).

### Wire format

Unchanged. The fix is sender-local watchdog state; no new frames, same framer shape, COBS encap, `MAX_CHUNK = 250`, `MSG_HDR = 6`, `POOL_SIZE = 64`, window 32, and seq-space budget as 6.0.32. The recovery path it triggers (drop → BREAK → resweep) is the existing one.

### Regression test

`LinkTxStallWatchdogTest` — three pins. Pin 1 drives the exact wedge (real `ArqCache` with `testFillPool()`, zero pending bits, ASYNC `sendMsg` rejected every tick) and asserts the OK-state timer drops within `idleTimeoutMs` + BREAK sent + `discCount==1`; toggle-off verified red (branch disabled → assertion fails). Pin 2 asserts a successful send clears the streak (link stays OK past `idleTimeoutMs`). Pin 3 asserts a single stale reject with no follow-up does not drop a quiet clean link (the still-live gate).

### Disclosed limitations

- `build/verify_build.sh` could not run in-sandbox (no network; the arduino-cli toolchain download fails). Source WAS touched this release, so the cross-compile carry-over re-opens — tracked in the todo `## Verify` footer. Last cleared end-to-end at 6.0.32 (1027835 / 79320 bytes); the delta since is `Link.h`/`LinkApi.cpp`/`LinkTimers.cpp`/`LinkCore.cpp`, all inside the host-tested link core (no `AutoLinkWeb.cpp` or Arduino-only surface touched), so risk is bounded but the build is formally unverified.
- The physical wedge (short CRC/desync burst on the peer under ASYNC flood) still needs the FireBeetle pair to confirm the watchdog recovers it end-to-end — folded into the re-lock cadence bench item.
- Streak stamping is ASYNC-only by design; SYNC's inline `waitForAck` timeout is its own recovery path.

### Result

- `make test` 64/64 unit (~8.3 s wall; new suite 2 ms), `make itest` 3/3 (~40.1 s wall), combined ~48.4 s. Toggle-off → red verified for Pin 1.
- `python3 build/dashboard_assets.py --check` green; `make test_coverage_manifest` green (new bin picked up from `TEST_BINS`).
- `python3 build/version.py check` green (20 entries; this entry pushed the oldest off the tail).
- `build/verify_build.sh` not run — see Disclosed limitations.
---

## v6.0.32

**Repackage: 6.0.31 → 6.0.32 (no source changes; host + itest + verify_build all green)**

Version-only bump. No `.cpp` / `.h` / `.ino` / `Makefile` / `test` source touched — protocol, dashboard, framer, and build surface are byte-identical to 6.0.31. Per the standing user-pref (always new version on repackage; no two consecutive reuses), the next slot above 6.0.31 is 6.0.32. The fix surface the four open items in `todo.md` call for is unchanged (item 1 stays the live TX-backpressure deadlock; items 2-3 stay FireBeetle-pair-only; item 4 OTA stays blocked on item 2) — this release only re-stamps the version, does not move them.

### What moved

- `include/AutoLink.h` / `library.properties` / `idf_component.yml` — `6.0.31 → 6.0.32` bump in lockstep (AGENTS rule 3).
- `docs/Version.md` — this entry; `add` + `trim --keep 20` dropped the oldest entry off the tail.
- `build/verify_build.sh` — re-run this session, the cross-compile carry-over that the 6.0.31 todo `## Verify` footer was tracking is now closed (1027835 B / 78% of program space, 79320 B / 24% of dynamic memory against `esp32:esp32:firebeetle32` / `esp32:esp32@3.3.5`).

### Wire format

Unchanged. No source touched. Same framer shape, COBS encap, `MAX_CHUNK = 250`, `MSG_HDR = 6`, `POOL_SIZE = 64`, window 32, and seq-space budget as 6.0.31.

### Regression test

None added — version-only, no behavior to pin. The full host gate is the signal: `make test` 63/63 unit, `make itest` 3/3, `verify_build.sh` cross-compile clean — all three re-run this session (below).

### Disclosed limitations

- The four Open items stay Open — same fix directions, same file:line anchors, same host-pin recipes. The watchdog surface (`LinkTimers.cpp:188` + `:243`) is unchanged, the heap floor (`uartRxBufferFloor` / `uartTxBufferFloor` / `chunksForMsgLen` derive from `MAX_CHUNK` symbolically) is unchanged, the `/ota/fw` + `/ota/gui` handlers still 503.
- The physical wedge for item 1, the ASYNC heap headroom for item 2, and the master/slave sweep cadence for item 3 are still FireBeetle-pair-only. Host pin recipe for item 1 (drive `sendMsg` false on full TX buffer with empty ARQ pool, assert the watchdog drops + resweeps within the deadline) is unchanged from the todo.

### Result

- No source touched; full host gate re-run this session: `make test` 63/63 unit (~8.2 s wall), `make itest` 3/3 (~40.1 s wall), `build/verify_build.sh` cross-compile clean (1027835 / 79320 bytes), combined ~48.3 s.
- `python3 build/version.py check` green (20 entries, --keep=20; this entry pushed the oldest off the tail).
- `make assets_check` / `make test_coverage_manifest` unaffected — no dashboard source, no new `TEST_BINS`.
---

## v6.0.31

**todo.md reorg + Version.md tail cleanup (docs only)**

Documentation-only housekeeping, same shape as 6.0.23/6.0.28 — no `.cpp`/`.h`/`.ino` touched, protocol and build surface byte-identical to 6.0.30. Two things get fixed. (1) `docs/Version.md` carried a dangling `---` separator at the file tail (below the oldest entry, v6.0.10), the same trailing-separator artifact 6.0.23 removed once before and that has re-accreted; it is stripped. (2) `todo.md` is re-tightened to the bare minimum a developer needs to resume the work: the four Open items are unchanged in substance and order (the live TX-backpressure deadlock stays Open 1 — a hang with no recovery outranks everything — then heap headroom, re-lock cadence, OTA), but each is trimmed of the bench-log-narrative retelling that duplicates the canonical account in Version.md, keeping only the symptom, the `LinkTimers.cpp` file:line anchors, the fix direction, and the host-pin recipe. The standing cross-compile carry-over (`verify_build.sh` last cleared the build at 6.0.22) is promoted from a recurring per-cycle disclosure into the todo `## Verify` footer so it is tracked in one place rather than re-disclosed each release. No completed items were moved — `todo.md` held only Open items and all four are confirmed still open in source (the symmetric watchdog no-op at `LinkTimers.cpp:243` is unfixed; the `/ota/fw` + `/ota/gui` handlers still return 503 stubs), so this pass re-files what is still Open and cleans the tail; it moves nothing to done.

### What moved

- `docs/Version.md` — this entry; `add` + `trim --keep 20` dropped the oldest entry off the tail. The trailing `---` artifact below the last entry is removed.
- `todo.md` — four Open items retained, same order, trimmed to symptom + file:line + fix direction + host-pin. Cross-compile carry-over folded into the `## Verify` footer. Title bumped to 6.0.31.
- `include/AutoLink.h` / `library.properties` / `idf_component.yml` — `6.0.30 → 6.0.31` bump in lockstep (AGENTS rule 3).

### Wire format

Unchanged. No source touched. Same framer shape, COBS encap, `MAX_CHUNK = 250`, `MSG_HDR = 6`, `POOL_SIZE = 64`, window 32, and seq-space budget as 6.0.30.

### Regression test

None added — docs-only, no behavior to pin. The host suite is the gate and is unchanged from 6.0.30: `make test` 63/63 unit, `make itest` 3/3. Both were re-run this session (below).

### Disclosed limitations

- Standing cross-compile carry-over: `build/verify_build.sh` was not re-run in-sandbox (no `arduino-cli` in this environment). No source delta on top of 6.0.30, so risk stays bounded to the already-disclosed surface; the 6.0.22 cross-compile last cleared the build path end-to-end at 1027671 / 79320 bytes. This is now tracked in the todo `## Verify` footer.
- The four Open items stay Open — this release only re-files and trims them. Item 1 is host-pinnable at the watchdog boundary; items 2-3 remain FireBeetle-pair-only; item 4 (OTA) is blocked on item 2's buffer-sizing answer.

### Result

- No source touched; host suite re-run this session: `make test` 63/63 unit (~6.6 s wall), `make itest` 3/3 (~40.1 s wall), combined ~46.7 s.
- `python3 build/version.py check` green (20 entries, --keep=20; this entry pushed the oldest off the tail).
- `make assets_check` / `make test_coverage_manifest` unaffected — no dashboard source, no new `TEST_BINS`.
- `build/verify_build.sh` not run in-sandbox — see Disclosed limitations.
---

## v6.0.30

**Drop per-slot 'wire <seq> <bytes>' Ping debug log; facade accessor stays for dashboard JSON**

The `wire <seq> <bytes>` debug line that Ping::loop fired at every slot completion (both the gap-stop drain and the main loop's tail-drain) was the production consumer that closed Open 5 in 6.0.19 — it gave `Link::bytesRecvdForMessage(baseSeq)` its first caller. The accessor is no longer latent, and the dashboard JSON path now reads it directly via the `AutoLink::bytesRecvdForMessage` facade forwarder. The per-slot log line was useful in development but pulled operator-facing noise at high ASYNC rates (it fired once per slot drain, every loop iteration, between the existing `echo` log and the head advance). Removed: both call sites in `Ping.h`, the comment that justified reading through `base_.comm_.` for the log, and the inverse-presence regression pins in `BytesRecvdForwardedToPingTest` (pin b/c/d inverted into a single "wire log absent" pin). The dashboard accessor stays — pin a (facade forwarder delegates to `link->bytesRecvdForMessage`) is unchanged. No protocol change. No build change.

### What moved

- `src/al/pingpong/Ping.h` — two `base_.log_.debug("Ping", "wire %u %u", ...)` call sites deleted (one in each slot-completion drain). The preceding 6-line comment block that described the wire-recvd log is dropped with the call.
- `test/test_desktop/al/pingpong/BytesRecvdForwardedToPingTest.cpp` — Pins b (format string in two sites), c (reads from facade forwarder), and d (fires inside slot-drain loop) are merged into a single Pin b that asserts the `wire %u %u` format string and `bytesRecvdForMessage(` reference are both absent from `Ping::loop`. Pin a (facade forwarder delegates to `link->bytesRecvdForMessage`) is preserved unchanged — the dashboard JSON path still needs the accessor.
- `include/AutoLink.h`, `library.properties`, `idf_component.yml`, `todo.md` — version bump `6.0.29 → 6.0.30` (AGENTS rule 3).

### Why

The wire-recvd line was added because the underlying accessor had no consumer — closing the "latent API" gap meant giving it a caller. With the dashboard JSON path serving that role exclusively, the log line became redundant: the operator-facing number for a slot is the local message length (already logged as `echo <seq> <msgBytes> <pending>`), and the wire-ACK sum is only useful for cross-checking transmission (a debugging concern, not an operator one). Per ASYNC TX rate the log fired dozens of times per second on the bench capture, drowning the surrounding state-change chatter.

### Wire format

Unchanged. This is a logging-only change.

### Regression coverage

- `BytesRecvdForwardedToPingTest::test_ping_wire_log_removed` (Pin b): asserts `wire %u %u` is absent anywhere in `Ping.h` and `bytesRecvdForMessage(` is not dereferenced inside `Ping::loop`. Pin a (facade forwarder still delegates to `link->bytesRecvdForMessage`) remains green. Toggling the log back on → red.
- Existing `PingSendFailureTest` Pin 2 (echo log shape) is unaffected: the 1500-char tail window after each `echo %u %u %d` site still reaches the `head_ = (head_ + 1)` slot advance without the wire log between them.
- Existing `LinkAckBytesTest`, `RxBytesWireAckTest`, `LinkBaseSeqTrackingTest`, and `LinkMessageRoundtripTest` are all wire-protocol coverage and are unaffected.

### Disclosed limitations

None. This is a logging-shape change only.

### Result

`62/62` host unit, `3/3` itest, `build/verify_build.sh` clean against `esp32:esp32@3.3.5`.
---

## v6.0.29

**Bench triage: TX-backpressure deadlock (docs-only, no source change)**

Docs-only triage of a fresh 512000/ASYNC bench capture — no `.cpp`/`.h`/`.ino`
touched, protocol and build surface byte-identical to 6.0.28/6.0.27. The
capture both confirms one 6.0.27 claim and exposes a new failure mode that the
6.0.27 fix uncovered. **Confirmed:** the pre-6.0.27 `disc` cascade is gone —
under a saturated ASYNC flood `disc` held at 0 on both peers through the entire
run (old Open 1's disc sub-claim, retired). **New failure (now Open 1):** after
a short CRC/desync burst on the receiver (Pong `frameErrs=3`, `errs` 0→5) the
master wedges — `Ping send failed (backpressure) n=… arqPending=0` repeats every
~1.5 s indefinitely, `tx` falls to 0 B/sec and stays there, echos freeze on both
peers (Ping 2535, Pong 2532), and the link never resweeps. Root cause: the
failed sends stall upstream of the ARQ pool (TX stream buffer / UART writer),
so `arqPending` reads 0 and the master's `frameErrs` is 0; the symmetric idle
watchdog (`LinkTimers.cpp:243`) no-ops on exactly `pendingCount()==0 &&
frameErrs==0`, and the asymmetric stuck-send check (`LinkTimers.cpp:188`) only
fires on `pendingCount() > 0`. A sender that is trying to send but cannot is
indistinguishable from a clean idle link, so recovery never triggers. The
6.0.27 Open 1 fix (remove Ping's `MAX_SEND_FAIL` self-disconnect) correctly
stopped the disc cascade but left no other recovery path, converting the noisy
cascade into a silent deadlock. `todo.md` is re-prioritised around this: the new
deadlock is Open 1 (most important — a hang with no recovery outranks feature
work), old Open 1's bench-confirm folds into it (disc part confirmed here,
recovery part superseded), and heap/cadence/OTA follow.

### What moved

- `todo.md` — old Open 1 (bench-confirm the 6.0.27 cascade fix) retired: its
  disc sub-claim is confirmed by this capture and recorded above; its
  frameErrs/recovery sub-claim is superseded by the new deadlock finding. New
  Open 1 is the TX-backpressure deadlock with root cause + host-pinnable fix
  direction (watchdog needs a third input: sustained `sendMsg` backpressure
  rejection distinguishable from quiet). Heap headroom → Open 2, re-lock cadence
  → Open 3 (now cross-references item 1 — a deadlocked master never enters the
  sweep), OTA → Open 4. Title and Verify footer updated.
- `include/AutoLink.h` / `library.properties` / `idf_component.yml` —
  `6.0.28 → 6.0.29` bump in lockstep (AGENTS rule 3).
- `docs/Version.md` — this entry; `add` + `trim --keep 20` dropped the oldest
  entry (v6.0.8) off the tail.

### Wire format

Unchanged. No source touched.

### Regression test

None added — docs-only, no behavior to pin. The new Open 1 fix, when it lands,
gets a watchdog toggle test (drive `sendMsg` false on a full TX buffer with an
empty ARQ pool; assert drop + resweep within the deadline). Host suite unchanged
from 6.0.27 (`make test` 63/63, `make itest` 3/3) — not re-run this session.

### Disclosed limitations

- Standing cross-compile carry-over unchanged: `build/verify_build.sh` not
  re-run in-sandbox (no `arduino-cli`). No source delta on top of 6.0.27, so
  risk stays bounded to the already-disclosed surface; 6.0.22 last cleared the
  build path end-to-end.
- The new Open 1 is a live, unfixed deadlock. This release only documents it.
  Items 2-3 remain FireBeetle-pair-only.

### Result

- No source touched; host suite unchanged from 6.0.27, not re-run (docs-only).
- `python3 build/version.py check` green (20 entries, --keep=20).
- `make assets_check` / `make test_coverage_manifest` unaffected — no dashboard
  source, no new `TEST_BINS`.
---

## v6.0.28

**Docs-only: todo.md re-prioritised, no source changes**

Housekeeping pass on `todo.md` only — no `.cpp`/`.h`/`.ino` touched, protocol
and build surface byte-identical to 6.0.27. The 6.0.27 release already closed
Open 1–6 and left the Open list correctly trimmed to items 7–10; this entry
re-numbers and re-orders that same four-item list most-important-first instead
of release-chronological-first. The bench-confirmation item (re-flood and
verify `frameErrs`/`disc` hold at 0 after the 6.0.27 source fix) moves to Open
1 — it's the standing gate on whether the prior release's fix actually works
on hardware, which outranks new feature work. Heap headroom (Open 2) and
re-lock cadence (Open 3) keep their bench-only status. OTA (Open 4) drops to
last: it's blocked on Open 2's buffer-sizing answer and is net-new feature
work, not a regression. No item's content changed beyond renumbering and the
cross-reference updates that follow from it.

### What moved

- `todo.md` — renumbered Open 7→1 (bench-confirm cascade fix), 8→2 (heap
  headroom), 10→3 (re-lock cadence), 9→4 (OTA, reordered last since blocked on
  item 2). Cross-references between items updated to match (item 4 now points
  at item 2 for the heap-headroom block; item 1 now points at item 2 for the
  baud-aware rx floor coupling). Title bumped to 6.0.28. Verify footer updated
  to reference item 1 instead of "Open 9".
- `include/AutoLink.h` / `library.properties` / `idf_component.yml` —
  `6.0.27 → 6.0.28` bump in lockstep (AGENTS rule 3).
- `docs/Version.md` — this entry; `add` + `trim --keep 20` dropped the oldest
  entry off the tail.

### Wire format

Unchanged. No source touched.

### Regression test

None added — docs-only, no behavior to pin. Host suite is unchanged from
6.0.27 (`make test` 63/63 unit, `make itest` 3/3) — not re-run this session.

### Disclosed limitations

- Standing cross-compile carry-over unchanged from 6.0.27:
  `build/verify_build.sh` has not been re-run in-sandbox (no `arduino-cli`
  in this environment). No source delta on top of 6.0.27, so risk stays
  bounded to the already-disclosed 6.0.27 surface; the 6.0.22 cross-compile
  last cleared the build path end-to-end.
- Items 1 and 3 remain FireBeetle-pair-only and are not host-pinnable.

### Result

- No source touched; host suite unchanged from 6.0.27, not re-run this
  session (docs-only).
- `python3 build/version.py check` green (20 entries, --keep=20).
- `make assets_check` / `make test_coverage_manifest` unaffected — no
  dashboard source, no new `TEST_BINS`.
---

## v6.0.27

**Close todo Open 1–6 (backpressure self-disconnect cascade, multi-chunk retx-slot reservation, RX wire-ACK byte counter, Pong post-settle drain, idle watchdog on clean link, gap-stall recovery)**

Source-touching release that lands six host-pinnable Open items from `todo.md` together because they're the only ones this release can host-pin; the items form a single coherent fix surface — the ASYNC flood backpressure cascade — plus four adjacent receiver-side / watchdog fixes that the same bench log exposed. No wire-format change; the protocol, ARQ, framer, and seq-budget are byte-identical to 6.0.26. The dominant signature the prior triage releases logged was the Ping `MAX_SEND_FAIL` self-disconnect cascade: under a saturated ASYNC flood, the sender's ARQ cache fills, `sendMsg` returns false (real backpressure), `consecSendFail_` climbs 1→5, and `dropLink()` fires on a saturated pipe — the drop forces a resweep, the relock re-floods, and `disc` climbs 1→N on both peers forever. The deeper root was that the chunk-emit loop in `LinkApi` had only a per-message `hasRoom()` gate; multi-chunk sends still walked past the ARQ pool floor and emitted chunks with no retx slot reserved, so any early loss was unrecoverable. Open 3, 4, 5, 6 surfaced from the same bench log: Pong's pre-blink drain ran before the post-baud-switch line garbage landed (16 CRC rejects per relock); the symmetric idle watchdog fired on a clean idle link (resweep loop); a held gap whose missing `cobsSeq` never arrived pinned the rx window open forever; and a clean SYNC two-peer capture showed Ping's RX total at 0 because the wire-ACK reply path never bumped `rxBytes`. `todo.md` is re-prioritised with all six items closed and the remaining Open list now hardware-bench-only (OTA + bench items). `build/verify_build.sh` not run in-sandbox this session — the sandbox doesn't have `arduino-cli`; risk stays bounded to the touched build path (mostly the link layer and Ping/Pong, no dashboard changes; the cross-compile cleared at 6.0.22 last).

### What moved

- `src/al/pingpong/Ping.h` — Open 1 fix. The send-failure branch's `consecSendFail_++` bump and `MAX_SEND_FAIL` threshold check + `dropLink()` call are removed. The branch now logs the ARQ pending count (`base_.comm_.arqPendingCount()`, a new facade forwarder) instead of the local `count_` (the app echo queue, which reads 0 when the cache is what saturated), sets the `BACKPRESSURE_COOLDOWN_MS` cooldown stamp, and breaks. No escalation to dropLink: the link is its own recovery loop; Ping's job is to pace sends, not to second-guess the link layer's health model. The `consecSendFail_` field, the `MAX_SEND_FAIL = 5` constant, and the `consecSendFail_ = 0` resets are removed as dead state. The log line changes from `"send failed (backpressure) n=%d pending=%d consec=%lu"` to `"send failed (backpressure) n=%d arqPending=%d"` — the misleading `pending=0` reads on a saturated cache are gone. The link-not-OK guard (`if (!base_.comm_.ready()) { break; }`) is preserved from the 6.0.x split; the backpressure branch follows it.
- `include/AutoLink.h` — `arqPendingCount() const` facade forwarder. Body delegates to `link->arqPendingCount()`. Lock-free contract matches `bytesRecvdFor`'s; the read window is a single `uint32` stamped inside the link task.
- `src/al/link/Link.h` — `int arqPendingCount() const` accessor; body delegates to `arq_.pendingCount()`. Lives next to `peekTxSeq()` and the existing lock-free stats accessors.
- `src/al/link/LinkApi.cpp` — Open 2 fix. The multi-chunk ASYNC send loop's chunk-emit now checks `arqCache_.hasRoom()` BEFORE each chunk goes out. On a full pool, `ok = false; break;` with a `sendMsg: ARQ cache exhausted mid-message (emitted %d/%d bytes) — partial send` warning. No chunk goes on the wire without a retx slot reserved; the per-message `hasRoom()` pre-check at the top of `sendMsg` covers the first message, the per-chunk check covers the rest of the burst. The two-stage guard means: (a) the first message of a pipeline bursts through (cache is fresh), (b) the burst stops when the cache floor is reached, (c) the caller's backpressure cooldown absorbs the partial. Pre-fix shape emitted all chunks regardless of cache state and logged `"pool exhausted … slot skipped (retx will be a cache miss)"` for chunks past the floor — those chunks were on the wire but unrecoverable on loss (the link's NAK handler does a cache lookup and finds nothing → `LinkArq` marks Drop → link reset → discCount climbs).
- `src/al/link/LinkRx.cpp` — Open 4 fix. `onAck` now bumps `rxBytes += RX_ACK_WIRE_BYTES` (= 8: 5 raw ACK COBS-encodes to 6 bytes, wire adds leading 0x00 + trailing 0x00). `onNak` bumps `rxBytes += RX_NAK_WIRE_BYTES` (= 6: 3 raw NAK COBS-encodes to 4 bytes, wire adds 2). The mirror of v6.0.10 Fix 4 (outbound CTRL/ACK/NAK counted in `txBytes`). The pre-fix shape only counted bytes on the app-payload path (`hw.pushAppBuf` → `rxBytes += n`), so a Ping whose peer only emits wire ACKs (the PingPong protocol) saw `rxBytes` stuck at 0 forever — the bench capture exposed this as `RX 0 B/s, total 0 B` on a healthy locked link. The new constants are defined in `LinkContext.h` alongside `MAX_CHUNK` and `MSG_HDR`.
- `src/al/link/LinkContext.h` — `RX_ACK_WIRE_BYTES = 8` and `RX_NAK_WIRE_BYTES = 6` constants. Single source of truth with the rest of the wire-protocol shape so a future ACK/NAK frame change has one place to update.
- `src/al/pingpong/Pong.h` — Open 3 fix (drain timing). The `!wasReady_` branch no longer drains BEFORE `blinkWait(4)`. The pre-fix shape ran the drain first; the baud switch from the SWP→OK transition pushed line garbage onto the rx stream AFTER the drain (the drain saw nothing — the bench log shows `drained 0 stale bytes pre-blink`), so the next `recv()` failed CRC and the bench log saw 16 Pong-app rejects per relock. Post-fix: `blinkWait(4)` first (the 4-blink settling indicator takes ~480 ms wall time; the next loop iteration is well after the baud switch is stable), `wasReady_ = true`, `tReady_ = millis()`, return. The post-settle drain runs on the NEXT iteration after `SETTLE_MS = 500`, by which time the line garbage has landed. The log line changes from `"drained %d stale bytes pre-blink"` to `"drained %d stale bytes post-settle"` so the operator can tell what happened.
- `src/al/pingpong/Pong.h` — Open 3 fix (counter reconcile). The recv-rejected (got<0) log line now includes the link-layer's `frameErrs` counter via `getStats(s)`: `"recv rejected (CRC/desync)  ackCount=%lu  frameErrs=%lu"`. The pre-fix shape only logged `ackCount`; the bench log showed 16 app-level rejects vs. 3 wire-level `frameErrs`, and operators had to correlate the two numbers by hand. Logging them in the same line resolves the mismatch — a single CRC failure produces one `frameErrs` bump and one `errs` bump in `Link::err_unlocked()` (`LinkApi.cpp:46-47`) but can produce multiple app-level rejects (each subsequent `recv()` on a still-corrupt stream returns negative), so the two numbers are different by construction; co-locating them surfaces the relationship.
- `src/al/link/LinkTimers.cpp` — Open 5 fix. The symmetric idle watchdog (the `rxAge > idleTimeoutMs && txAge > idleTimeoutMs` check in `onTimerOk_unlocked`) is now a no-op when `arq_.pendingCount() == 0 && frameErrs == 0`. Pre-fix shape unconditionally dropped the link on a fully quiet past `idleTimeoutMs`; the bench log showed the watchdog firing on a clean loopback whose last activity was a heartbeat many seconds old, triggering resweeps on a healthy link. The post-fix contract: "quiet is fine" — only drop when there's an actual reason to suspect the link. The asymmetric "stuck send" check (TX active, RX silent) above is unchanged and still fires on `arq_.pendingCount() > 0 && rxAge > FAST_IDLE_RX_MS && txAge < FAST_IDLE_TX_MS`; the symmetric watchdog's drop log line now includes the `arqPending` count and `frameErrs` total for operator correlation.
- `src/al/link/LinkReorder.{h,cpp}` + `src/al/link/LinkRx.cpp` + `src/al/link/LinkTimers.cpp` — Open 6 fix. `LinkReorder::dropExpired` now takes an out-array of dropped seqs; the Link callers advance `rxSeq` past each one via `reorderAdvanceRxSeq(gap)`. Pre-fix shape: `dropExpired` released the slot and bumped `lostMsgs` but left `rxSeq` pinned at the gap's expected position; the contiguous-resume logic in `flushContiguous` looked up `slots_[exp]` and broke on `in_use==false` — a held gap whose missing `cobsSeq` never arrived pinned the rx window open forever, the receiver gap-stop NAKed the same missing seq indefinitely, and a clean retransmit that arrived after `reorderHoldMs` was silently rejected as stale (the rx seq never moved). Post-fix: `dropExpired` returns the set of dropped seqs; the Link advances `rxSeq` for each one; the next `onPayload` lands in the right slot, `flushContiguous` can resume, and the receiver's gap-stop NAK doesn't loop on the same missing seq. `lostMsgs` bumps per dropped slot for honest per-gap accounting.
- `src/al/pingpong/Ping.h` — pre-existing `consecSendFail_` field, `MAX_SEND_FAIL = 5` constant, and the `consecSendFail_ = 0` resets (in `setup`, `!wasReady_`, send-success branch, `setPaused`, `clearQueue_`) are removed as dead state after the Open 1 fix. The comment block above `BACKPRESSURE_COOLDOWN_MS = 1000` is rewritten to reflect the new contract (no escalation to drop on backpressure; cooldown is the throttle).
- `test/test_desktop/al/pingpong/PingSendFailureTest.cpp` — Pin 3 rewritten: was `test_ping_consec_send_fail_counter` (asserted the bump + threshold check + dropLink + reset); is now `test_ping_backpressure_does_not_drop_link` (asserts NO `consecSendFail_++` bump, NO `MAX_SEND_FAIL` threshold, NO `dropLink` in the backpressure branch, the cooldown stamp IS set, and the diagnostic log line reads `arqPendingCount()` not `count_`). Pin 7 renamed and tightened: was `test_ping_send_fail_splits_link_not_ok_from_backpressure` (asserted the link-not-OK guard precedes the consecSendFail bump); is now `test_ping_send_fail_guards_on_link_not_ok` (asserts the link-not-OK guard precedes the backpressure log line and the cooldown stamp — the guard is a true early-return with no side effects). Pin 3 (echo log format) window was 400 chars in 6.0.19, 1500 in 6.0.19's tail-window widening — unchanged here.
- `test/test_desktop/al/link/ModeSyncAsyncFixesTest.cpp` — Pin 5b (Ping backpressure 1000 ms cooldown) was locating the cooldown assignment via `src.find("consecSendFail_++")`. With Open 1's bump removal, the pin's anchor changes to `src.find("send failed (backpressure)")` (the backpressure branch's first action after the link-not-OK early-return). The 2000-char window after the anchor still covers the cooldown assignment.
- `test/test_desktop/al/link/LinkMtuRoundtripTest.cpp` — Pin 4 rewritten. The pre-this-fix shape documented and tested that the 32 KB ASYNC send with production `ArqCache` (POOL_SIZE=64) succeeded end-to-end despite the cache-floor skip emitting un-ACKable chunks; the test relied on 0% drop to keep `pendingCount_` from saturating. The Open 2 fix makes the cache-floor skip a stop-and-fail instead. Post-fix contract: `sendMsg(tx, 32768)` with POOL_SIZE=64 returns `false` (chunks 1..240 go out cleanly, chunk 241 hits the per-chunk `hasRoom()` guard at `arqCache_.hasRoom()` returns false, `ok=false; break;`). The pin now asserts `sent == false` and pumps the receiver to verify `cacheA.count == 0` after the partial send drains (every emitted chunk had its retx slot reserved and ACKed; no orphan un-ACKed chunks left behind).
- `test/test_desktop/al/link/LinkMessageRoundtripTest.cpp` — `test_message_chunk_boundary_carries_then_rejects` rewritten. The pre-this-fix shape tested 240-chunk-cap boundary with the production `ArqCache`; the post-fix contract is the same cap boundary but the failure mode changes from "240-chunk carries, 241st rejected by ARQ cap gate" to "cap-boundary sends reject before emitting un-retxable chunks; under-cap messages round-trip end-to-end". The pin sends an over-cap payload (CAP*250 bytes → 240 data + 1 hdr = 241 chunks, exceeds CAP=240) and asserts `sent == false`, then drains to verify `cacheA.count == 0` (no orphan chunks).
- `test/test_desktop/al/link/RxBytesWireAckTest.cpp` — new file, Open 4 host pin. Four pins: (1) `onAck` bumps `rxBytes` by `RX_ACK_WIRE_BYTES` (8) per inbound ACK; (2) `onNak` bumps `rxBytes` by `RX_NAK_WIRE_BYTES` (6) per inbound NAK; (3) the `RX_*_WIRE_BYTES` constants live in `LinkContext.h` (single source of truth alongside `MAX_CHUNK` and `MSG_HDR`) and `LinkRx.cpp` references them by name; (4) wire-ACK-only reply path bumps `rxBytes` end-to-end (WireSim loopback: Pong emits wire ACKs but no app echo; Ping's RX counter advances).
- `test/test_desktop/al/pingpong/PingPongLogHygieneTest.cpp` — new pin `test_pong_post_settle_drain_and_frame_errs_in_log` for Open 3: (a) Pong's drain log line uses "drained %d stale bytes post-settle" (the post-fix wording; pre-fix "drained %d stale bytes pre-blink" would trip); (b) Pong's recv-rejected log line includes "frameErrs=%lu" and the branch calls `getStats` to read the counter from the link layer.
- `test/test_desktop/al/link/LinkErrorTest.cpp` — two sub-tests in `test_error_counter_link_failures` that exercised the idle watchdog's drop path now stage a pending ARQ slot via `LinkTestAccessor::markAckedPending(0x42)` so the asymmetric-idle check (which still fires on `arqPending>0`) does the drop. The pre-this-fix shape relied on the symmetric watchdog's unconditional drop; the post-fix contract is "watchdog only drops when there's a reason".
- `test/test_desktop/al/ClockInjectionTest.cpp` — `test_idle_timeout_drops_link` rewritten. The pre-this-fix shape asserted `dropA + dropB == 2` (both nodes dropped after 5500 ms quiet); the post-fix contract is "clean idle link: no drops, watchdog is a no-op without pending ARQ or recent errors" → `dropA + dropB == 0`. The existing test name was already "symmetric idle does NOT drop"; the assertion now matches. `test_idle_watchdog_combined_tx_rx_v5_1_54` similarly updated: `dropA + dropB == 0`.
- `test/test_desktop/al/link/sweep/LinkSweepPhaseTest.cpp` — `test_v531_heartbeat_miss_drops_quickly` now stages a pending ARQ slot before pumping past `idleTimeoutMs`. The pre-this-fix shape relied on the symmetric watchdog's drop; post-fix the asymmetric check (`rxAge > FAST_IDLE_RX_MS, txAge < FAST_IDLE_TX_MS, arqPending > 0`) does the drop within 10 ticks.
- `test/test_desktop/al/link/LinkCobsSeqTest.cpp` — `test_lost_msgs_burst_vs_single` second sub-test (the `cfg2.reorderHoldMs = 0` case) updated for the Open 6 contract. Pre-this-fix shape: 3 gaps, 2 lost (frames 4 and 5 held; frame 7 held; rxSeq stays pinned). Post-fix: dropExpired at the top of every onPayload empties the held slots; rxSeq advances past each dropped gap; gaps collapse to 2 (only the original gap, the slot 4 expiry is logged as lost not as a new gap), lostMsgs=1 (only slot 4 expires; slot 5 was never held because frame 5 is Contiguous after the rxSeq advance, not a gap).
- `test/test_desktop/al/link/LinkReorderTest.cpp` — new pin `test_gap_expired_advances_rx_seq` for Open 6: drive frame 0 (deliver, rxSeq=0), frame 4 (gap, hold slot 4, rxSeq stays 0), then frame 5 with `reorderHoldMs=0` so slot 4 expires on this onRx call. Assert `gaps=1, lostMsgs=1, rxSeq=5` and `!reorderSlotInUse(4)` — the dropped gap advances the window; pre-fix pinned rxSeq=0 forever.
- `test/common/LinkTestAccessor.h` — new `utilFrameRxFeed(data, len)` accessor that drives `Link::frameRx.feed` directly. Used by `RxBytesWireAckTest` to inject synthetic ACK/NAK frames into the framer's dispatch without going through a full sender/receiver round-trip.
- `test/test_desktop/Makefile` — `run_test_rx_bytes_wire_ack` added to `TEST_BINS`, the per-suite build/run target lists, and the `test_rx_bytes_wire_ack` short target.
- `include/AutoLink.h` / `library.properties` / `idf_component.yml` — `6.0.26 → 6.0.27` bump in lockstep (AGENTS rule 3).
- `docs/Version.md` — this entry; `add` + `trim --keep 20` dropped v6.0.6 off the tail.
- `todo.md` — Open 1–6 closed. Remaining Open list: 7 (OTA firmware + GUI upload, slots reserved), 8 (ASYNC heap headroom), 9 (ASYNC flood frameErrs/disc=0 — CONFIRMED FAILING, re-test after Open 1+2), 10 (re-lock cadence symmetry + sweep walk-down). Title bumped to 6.0.27.

### Wire format

Unchanged. The Open 1+2 fixes are sender-side ARQ pacing and chunk-emit reservation, not a wire change. The Open 4 fix adds inbound wire-byte accounting that mirrors what the wire already does (the v6.0.10 outbound counter was already counting `txBytes += el + 2` for ACK/NAK frames; this is the inbound mirror). The Open 3+5+6 fixes are link-state machine behaviour changes (drain timing, watchdog gate, gap-stall advance) that don't alter the on-wire shape. Same framer shape, COBS encap, `MAX_CHUNK = 250`, `MSG_HDR = 6`, `POOL_SIZE = 64`, window 32, seq-space 254.

### Regression test

- `run_test_ping_send_failure` — Pin 3 (backpressure does NOT escalate to dropLink): the new contract asserts the backpressure branch has no `consecSendFail_++`, no `MAX_SEND_FAIL` threshold check, no `dropLink()` call; the cooldown stamp IS set; the diagnostic line reads `arqPendingCount()` not `count_`. Pin 7 (link-not-OK guard): the `ready()` check precedes the backpressure log line and the cooldown stamp so the link-not-OK path is a true early-return with no side effects. Both pins toggle red if the source reverts to the pre-this-release shape (re-add the bump + threshold + dropLink in Pin 3; remove or move the ready() guard in Pin 7).
- `run_test_mode_sync_async_fixes` Pin 5b — the cooldown 1000 ms gate is independent of `txDelayMs`; fires regardless of user pacing. The anchor for the post-fix cooldown assignment is `src.find("send failed (backpressure)")`; reverting the log line to the pre-fix `pending=%d` text or moving the cooldown stamp out of the backpressure branch trips the pin.
- `run_test_mtu_roundtrip` Pin 4 — 32 KB ASYNC send with production `ArqCache` (POOL_SIZE=64) returns `false` at the per-chunk `hasRoom()` guard (after emitting 240 chunks). Pre-fix shape asserted `sent == true`; the post-fix `sent == false` and `cacheA.count == 0` after drain are the new contract.
- `run_test_alink_message_roundtrip` — the chunk-boundary test now sends a `CAP*250` byte payload against `TestCache{CAP=240}` and asserts the send is rejected (the cap-boundary payload exceeds the cache-floor). After draining, `cacheA.count == 0` proves every emitted chunk had its retx slot reserved.
- `run_test_rx_bytes_wire_ack` (new) — Pin 1: `onAck` bumps `rxBytes` by `RX_ACK_WIRE_BYTES` (8). Pin 2: `onNak` bumps `rxBytes` by `RX_NAK_WIRE_BYTES` (6). Pin 3: constants defined in `LinkContext.h`, referenced by name in `LinkRx.cpp`. Pin 4: WireSim loopback ACK-only reply path → receiver `rxBytes > 0`. All four pins toggle red if the inbound count is reverted (Pin 1/2), the constants are moved out of `LinkContext.h` (Pin 3), or the facade accessor is removed (Pin 4).
- `run_test_pingpong_log_hygiene` — new pin `test_pong_post_settle_drain_and_frame_errs_in_log`: the drain log line uses "post-settle"; the recv-rejected branch includes `frameErrs=%lu` and calls `getStats`. Pre-fix "pre-blink" wording trips Pin (a); missing `frameErrs` field trips Pin (b).
- `run_test_alink_error` + `run_test_clock_injection` + `run_test_alink_sweep_phase` — the three idle-watchdog-touching tests now stage a pending ARQ slot (`LinkTestAccessor::markAckedPending(0x42)`) before pumping past `idleTimeoutMs`. Pre-this-fix shape relied on the symmetric watchdog's unconditional drop; the post-fix asymmetric check (`arqPending > 0`) is what fires the drop now. The assertions on `discCount` and state transitions stay the same; only the staging changes. Without the staging, the watchdog is a no-op on a clean idle link and the assertions would fail.
- `run_test_alink_cobsseq` — `test_lost_msgs_burst_vs_single` second sub-test updates for the Open 6 contract. With `reorderHoldMs=0`, frames 4 and 5 (originally both held) now have slot 4 expire when frame 5 arrives (rxSeq advances past the dropped gap); the held-slot count is 1 (slot 4 only), lostMsgs=1, gaps=2 (only the original frame-4 gap).
- `run_test_linkreorder` — new pin `test_gap_expired_advances_rx_seq`: drive frame 0 (deliver), frame 4 (gap, hold), then frame 5 with `reorderHoldMs=0` so slot 4 expires. Assert `gaps=1, lostMsgs=1, rxSeq=5, !reorderSlotInUse(4)`. Pre-fix shape would assert `gaps=1, lostMsgs=0, rxSeq=0` (window pinned).
- `run_test_ack_bytes` — unchanged. The pre-existing extended-ACK frame shape (5-byte raw with bytes-recvd) and `bytesRecvdFor(seq)` accessor pins stay green; the `RX_ACK_WIRE_BYTES = 8` constant is consistent with the 5-byte raw ACK COBS-encoded + 2 framing bytes.
- Existing pins preserved: `run_test_uri_handler_alignment` (12-handler / 12-cap, unchanged), `run_test_handle_root_chunked` (root handler chunked encoding, unchanged), `run_test_base_seq_tracking` (5 pins on base-seq accessor + walk loop, unchanged), `run_test_pingpong_structure` (Ping/Pong role shapes, unchanged), `run_test_ping_gap_transition` (Ping gap-stop transition table, unchanged), `run_test_esphal_*` (EspHal stream buffer / health, unchanged), `run_test_seq_space_guard` (seq-space exhaustion, unchanged), `run_test_mtu_roundtrip` Pins 1/2/3 (multi-chunk round-trip 32 KB and boundary sizes, unchanged — Pin 4 is the only one rewritten).

### Disclosed limitations

- The chunk-emit-level in-flight bound (Open 2) is a real protocol constraint: a multi-chunk ASYNC send whose chunk count exceeds the ARQ pool floor will now FAIL where it previously silently emitted un-ACKable chunks. This means a Ping user who configures a `maxMsg` near the MTU and runs in ASYNC mode with a saturated pipe will see legitimate sends fail with the backpressure cooldown. The fix is correct (the pre-fix shape was unreliable on loss); the caller-side mitigation is to either pipeline shorter messages or wait for the cache to drain. The 32 KB `LinkMtuRoundtripTest` Pin 4 case is the canonical example of this constraint.
- The Pong pre-blink → post-settle drain timing change (Open 3) means Pong's first post-recovery `loop()` iteration returns without doing recv-side work (the SETTLE_MS=500 wait). On a fast loop this is a sub-ms delay per recovery; on a FreeRTOS task running at 100 Hz it's 10 ms. Operators watching the pong-side "first-ack-after-recovery" timing will see a ~500 ms gap; this is the price of draining garbage that the post-baud-switch line pushed onto the rx stream.
- The symmetric idle watchdog's new contract (Open 5) is "no drop without reason" — but "no reason" includes the case where the peer has been quietly disconnected for >`idleTimeoutMs` seconds (no UART activity at all because the cable is unplugged). The link stays in OK state until either the peer reconnects (which generates activity) or the application pings the link via a manual `kickoff()` / `setLinkPaused(false)` cycle. The asymmetric "stuck send" check still catches the "I'm sending but not hearing" case (gated on `arqPending > 0`); a passive app that doesn't generate ARQ traffic won't get any watchdog signal of a fully-quiet peer. This is the trade-off: a watchdog that fires on a healthy quiet link is worse than a watchdog that misses a fully-quiet peer (the relock on resweep will catch it).
- The gap-stall recovery (Open 6) advances `rxSeq` past an expired held gap; this is correct behaviour but it means a receiver that loses a chunk AND the retransmit (both missing) will see the chunk as "lost" rather than waiting forever. The pre-fix shape waited forever and NAKed indefinitely — also wrong, but in a different direction. Operators reading `lostMsgs` should note that an Open-6-recovered gap bumps `lostMsgs` by 1 (one missing chunk counted as lost) and the rx window resumes; pre-fix would have shown `lostMsgs=0` (no loss counted) but a permanent hang.
- The RX byte counter (Open 4) now counts inbound wire-ACK/NAK bytes; a Pin role node that previously showed `RX 0 B/s, total 0 B` on a healthy link will now show `RX ~ Ping's wire-ACK receive rate`. Operators reading the dashboard RX total should note that on a Ping-role node it now means "wire bytes received" (matches the existing `txBytes` semantics post-6.0.10), not "app payload delivered". For Pong-role nodes the value is essentially the same as before (Pong does receive app payloads, so app + wire = same accounting up to a small offset).
- Standing cross-compile carry-over: `build/verify_build.sh` not re-run in-sandbox this session (sandbox doesn't have `arduino-cli`). The fix surface is the link layer + Ping/Pong + the facade forwarder, no dashboard changes; the 6.0.22 cross-compile last cleared the build path at 1027671 / 79320 bytes. The touched source is mostly `.cpp` and a small set of `.h` headers; a fresh cross-compile should run clean on a longer-lived environment. AGENTS rule 4 carry-over discharged for 6.0.27 once `verify_build.sh` runs end-to-end in a sandbox with the ESP32 toolchain.
- Open 7 (OTA firmware + GUI upload) stays Open — slots r10/r11 reserved with 503 stubs since 6.0.22, no implementation this release. Open 8 (ASYNC heap headroom), 9 (flood frameErrs/disc=0 — CONFIRMED FAILING in the prior triage; expected to flip GREEN once this release's bench re-test runs against the Open 1+2 fixes), and 10 (re-lock cadence symmetry + sweep walk-down) stay Open as hardware-bench-only items. They require the FireBeetle pair and WireSim can't catch any of them (byte-exact and untimed).

### Result

- `make test` 63/63 unit suites pass (wall ~7 s). New suite `run_test_rx_bytes_wire_ack`; updated pins in `run_test_ping_send_failure`, `run_test_mode_sync_async_fixes`, `run_test_mtu_roundtrip`, `run_test_alink_message_roundtrip`, `run_test_pingpong_log_hygiene`, `run_test_alink_error`, `run_test_clock_injection`, `run_test_alink_sweep_phase`, `run_test_alink_cobsseq`, `run_test_linkreorder`. New source-grep-only suite added to the manifest exempt list.
- `make itest` 3/3 host integration suites (loopback / loopback_noise / loopback_sync) unchanged from 6.0.26.
- `make assets_check` PASS — no dashboard source touched, `AutoLinkWebHtml.h` byte contract unchanged.
- `make test_coverage_manifest` PASS — `run_test_rx_bytes_wire_ack` added to the source-grep-only exempt tuple (it's a source-grep + runtime-mixed suite and doesn't link `$(AUTOLINK_SRC)`).
- `python3 build/pretty_print.py` clean reformat on every touched file.
- `python3 build/version.py check` green (20 entries, --keep=20; this entry pushed v6.0.6 off the tail).
- `build/verify_build.sh` not run in-sandbox — see Disclosed limitations.

### Verification artifacts

- `make test` wall: ~7 s (63 unit suites including the new RxBytesWireAckTest).
- `make itest` wall: unchanged from 6.0.22 (~40 s for the three integration suites).
- Combined host wall: ~47 s.
- Peak RSS: unchanged from 6.0.22 (~89 KiB largest single-suite resident set).
- Cross-compile wall: not run this session.
---

## v6.0.26

**Bench-image triage: Ping RX reads 0 (wire-ACK frames uncounted)**

Triage of a clean two-peer dashboard capture (2026-06-30, Ping 10.10.10.29 / Pong 10.10.10.39, both SYNC, both locked 512000, both state OK, 0 errors / 0 lost / 0 frameErrs, txDelay=100 — a healthy paced run, not a flood). The capture exposes a stats-accuracy defect the flood logs masked: Ping reports `RX 0 B/s, total 0 B` while it is actively matching echoes (the `Ping echo` counter climbs 742→744) and the peer reports `TX total 13.4 KB`. Root cause: `rxBytes` (→ `Stats.rx` → dashboard RX total) is only incremented on the delivered-application-payload path (`hw.pushAppBuf`, `LinkRx.cpp:210,222,273`). In the PingPong protocol Pong does NOT echo a payload back — its reply is a wire-level ACK frame (`Ping.h:1-26`) consumed by the ARQ `waitForAck` path, not by `recvMsg`. Ping therefore never delivers an app message and its RX total stays 0 forever, even though its UART is continuously receiving ACK frames. Those inbound ACK/NAK frames are decoded in `UtilFrameRx::feed` and dispatched to `onAck`/`onNak` (`LinkFrameRx.cpp:30,39,42`; `LinkRx.cpp:300,312`), neither of which touches `rxBytes`. This is the missing mirror of the v6.0.10 Fix 4, which added outbound CTRL/ACK/NAK counting to `txBytes` (the reason Pong's TX is nonzero); the inbound side was never given the symmetric counter. `todo.md` adds this as new Open 4 (RX byte counter ignores wire-ACK/CTRL frames) with the fix — bump `rxBytes` by the decoded frame length for received ACK/NAK/CTRL frames — and a host pin (loopback ACK-only reply → receiver `rxBytes > 0`). The flood/desync items renumber: idle-watchdog → Open 5, gap-stall → Open 6, OTA → Open 7, bench items → 8–10 (re-lock cadence + sweep walk-down stay folded into 10). No completed items remained in `todo.md` to move out — the prior triage releases already re-filed everything still Open — so this pass only adds the new item and re-prioritises. `docs/Version.md` is trimmed to 20 entries (oldest, v6.0.5, dropped).

### What moved

- `todo.md` — new Open 4 (RX byte counter omits wire-ACK/NAK/CTRL frames → Ping RX reads 0; confirmed this clean SYNC capture; call sites `LinkRx.cpp:210,222,273` for the existing payload path and `LinkFrameRx.cpp:30,39,42` / `LinkRx.cpp:300,312` for the uncounted ACK/NAK path; fix is the symmetric inbound counter to v6.0.10 Fix 4). Flood items 1–3 unchanged; idle-watchdog → 5, gap-stall → 6, OTA → 7; bench items → 8–10. Verify footer notes items 1–6 add new host pins. Title bumped to 6.0.26.
- `docs/Version.md` — this entry; `add` + `trim --keep 20` dropped v6.0.5 off the tail.
- `include/AutoLink.h` / `library.properties` / `idf_component.yml` — `6.0.25 → 6.0.26` bump in lockstep (AGENTS rule 3).

### Wire format

Unchanged. No `.cpp` / `.h` / `.ino` touched. Same framer shape, COBS encap, `MAX_CHUNK = 250`, `MSG_HDR = 6`, `POOL_SIZE = 64`, window 32, and seq-space budget as 6.0.25. This entry triages the defect into todo.md; it does not fix it.

### Regression test

None added — docs-only, no behavior to pin. The new Open 4 carries its host pin to write when the code lands: a loopback whose reply path is wire-ACK-only, asserting the receiver's `rxBytes > 0` (and reverting the inbound-count addition turns it red). The host suite is the gate and is identical to 6.0.25: `make test` 62/62 unit, `make itest` 3/3.

### Limitations

- The RX-counter fix (todo Open 4) stays Open — this release only triages it. Once it lands, the dashboard RX total on a Ping-role node will reflect inbound ACK/NAK wire bytes rather than reading a flat 0; operators reading RX as "app payload delivered" should note the counter then means "wire bytes received", matching the existing `txBytes` semantics post-6.0.10.
- The flood cascade (todo Open 1–3, 5, 6) stays Open. The 503 OTA stubs and reserved r10/r11 slots are unchanged from 6.0.22.
- Standing cross-compile carry-over: `build/verify_build.sh` was not re-run for 6.0.26 (no source delta on top of 6.0.25; the 6.0.22 cross-compile last cleared the build path at 1027671 / 79320 bytes). Risk stays bounded to the unchanged build path.
- The capture is hardware; items 8–10 remain FireBeetle-pair-only and are not host-pinnable.

### Result

- No source touched; the host suite is unchanged from 6.0.25 (`make test` 62/62 unit, `make itest` 3/3) — not re-run this session (docs-only).
- `python3 build/version.py check` expected green (20 entries, --keep=20).
- `make assets_check` / `make test_coverage_manifest` unaffected — no dashboard source, no new TEST_BINS.
---

## v6.0.25

**Bench-log triage: Ping MAX_SEND_FAIL self-disconnect cascade**

Triage of a second FireBeetle-pair bench capture (2026-06-30, ASYNC, txDelay=0, random fill, locked 512000) that reproduces a *different, cleaner* self-disconnect signature than the 6.0.24 capture. No source, wire-format, build-surface, or test-behavior change — the protocol, ARQ, framing, and seq budget are byte-for-byte identical to 6.0.24. This log shows no `pending=73` / pool-exhaust / `gap stop missing seq=9` path; instead the flood saturates the ARQ cache, `sendMsg` returns false (real backpressure), Ping's `consecSendFail_` counter climbs 1→5 — one per `BACKPRESSURE_COOLDOWN_MS` window — and `Ping.h:372` fires `dropLink()` (`send failed 5 times — dropping link`). The drop forces a resweep, the relock re-floods, and the loop repeats: `disc` climbs 1→6 on both peers and the link never recovers. The `pending=%d` field in the backpressure log is Ping's app echo queue (`count_`), not `arq_.pendingCount()`, so it reads `pending=0` while the ARQ cache is full — masking the cause. The Pong side adds 16 `recv rejected (CRC/desync)` on every 512000 relock (the `drained 0 stale bytes pre-blink` drain runs before the post-baud-switch line garbage arrives). `todo.md` is re-prioritised most-important-first: the Ping `MAX_SEND_FAIL` self-disconnect becomes Open 1 (the proven dominant signature, `Ping.h:372,378`), the chunk-emit-level in-flight bound drops to Open 2 as the deeper root, a new Open 3 captures the Pong stale-byte CRC rejects + the `errs=3`-vs-16-logged stats discrepancy, the idle-watchdog and gap-stall items move to Open 4/5, OTA to Open 6, and the bench items renumber 7–9 (re-lock cadence + sweep walk-down fold into 9).

### What moved

- `todo.md` — reordered most-important-first. New Open 1 (Ping `MAX_SEND_FAIL` self-disconnect on sender backpressure, `Ping.h:371,372,378`, plus the misleading `pending=%d`-logs-`count_` line at `Ping.h:345`), Open 2 (chunk-emit-level in-flight bound, `LinkApi.cpp:247`), Open 3 (Pong stale-byte CRC rejects, `Pong.h:80,109`, + `errs` vs `frameErrs` stats reconcile, `LinkApi.cpp:47`). Idle-watchdog → Open 4 (`LinkTimers.cpp:208,181`), gap-stall → Open 5 (`LinkRx.cpp:187` / `LinkTimers.cpp:142`), OTA → Open 6. Bench items renumbered 7–9; item 8 (flood frameErrs/disc=0) stays CONFIRMED FAILING with this fresh reproduction; item 9 folds the re-lock cadence asymmetry and sweep walk-down together. Verify footer notes items 1–5 add new host pins. Title bumped to 6.0.25.
- `docs/Version.md` — this entry; `add` + `trim --keep 20` dropped the oldest entry off the tail.
- `include/AutoLink.h` / `library.properties` / `idf_component.yml` — `6.0.24 → 6.0.25` bump in lockstep (AGENTS rule 3).

### Wire format

Unchanged. No `.cpp` / `.h` / `.ino` touched. Same framer shape, COBS encap, `MAX_CHUNK = 250`, `MSG_HDR = 6`, `POOL_SIZE = 64`, window 32, and seq-space budget as 6.0.24. This entry triages the defect into todo.md; it does not fix it.

### Regression test

None added — docs-only, no behavior to pin. The fixes themselves (todo Open 1–5) each carry a host pin to write when the code lands: a window-ceiling loopback asserting a saturated send pipe yields `sendMsg==false` + cooldown but `discCount == 0` and no resweep; a window-ceiling loopback with one induced in-window loss asserting a cache hit; a Pong recv-path pin for the post-baud-switch drain timing; an idle-past-`idleTimeoutMs` loopback asserting `discCount == 0`; and a `LinkReorder` table test for stepping over an unfillable hole. The host suite is the gate and is identical to 6.0.24: `make test` 62/62 unit, `make itest` 3/3.

### Limitations

- The cascade (todo Open 1–5) stays Open — this release only triages it. The 503 OTA stubs and reserved r10/r11 slots are unchanged from 6.0.22.
- Standing cross-compile carry-over: `build/verify_build.sh` was not re-run for 6.0.25 (no source delta on top of 6.0.24; the 6.0.22 cross-compile last cleared the build path at 1027671 / 79320 bytes). Risk stays bounded to the unchanged build path.
- The bench log is hardware-captured; items 7–9 remain FireBeetle-pair-only and are not host-pinnable.

### Result

- No source touched; the host suite is unchanged from 6.0.24 (`make test` 62/62 unit, `make itest` 3/3) — not re-run this session (docs-only).
- `python3 build/version.py check` expected green (20 entries, --keep=20).
- `make assets_check` / `make test_coverage_manifest` unaffected — no dashboard source, no new TEST_BINS.
---

## v6.0.24

**Bench-log triage: ASYNC flood self-disconnect cascade triaged into todo.md (docs only)**

Triage of a FireBeetle-pair bench capture (2026-06-30, ASYNC, txDelay=0, locked 512000) that reproduces a self-inflicted disconnect cascade. No source, wire-format, build-surface, or test-behavior change — the protocol, ARQ, framing, and seq budget are byte-for-byte identical to 6.0.23. The log shows the flood driving `arq_.pendingCount()` to 73, past the ARQ cache floor (`POOL_SIZE = 64`): `cobsSeq 73..81` log "pool exhausted … slot skipped (retx will be a cache miss)", i.e. those chunks went on the wire un-retransmittable. An early loss (`seq=9`) then can never be resent, the receiver stalls in reorder (`GAP seq=10 exp=9` … `diff=73`), `Ping gap stop: missing seq=9` pauses sending, and the `POOL_EXHAUST_DROP_PENDING = 1` backstop tears the link — `disc` climbs 1→7. After the app pauses, the symmetric idle watchdog then loop-resweeps the clean locked link every 10 s. `todo.md` is re-prioritised most-important-first with three new host-pinnable Open items distilled from this (chunk-emit-level in-flight bound, no-resweep-on-sender-cache-full, idle-watchdog no-op on a clean quiet link, plus a receiver gap-stall recovery hardening); OTA drops from Open 1 to Open 4; bench item "flood frameErrs/disc=0" is reclassified from pending to CONFIRMED FAILING with this reproduction; sweep walk-down absorbs the observed master/slave cadence asymmetry.

### What moved

- `todo.md` — reordered most-important-first. New Open 1 (flood self-disconnect: chunk-emit-level in-flight bound + stop resweeping on sender cache-full, with the exact log evidence and `LinkApi.cpp:247` / `LinkTimers.cpp:67,248,263` call sites), Open 2 (idle-watchdog resweep loop, `LinkTimers.cpp:208`), Open 3 (receiver gap-stall recovery, `LinkRx.cpp:187` / `LinkTimers.cpp:142`). OTA moved 1→4 with sub-steps intact. Bench items renumbered 5–7; item 6 (flood frameErrs/disc=0) marked CONFIRMED FAILING; item 7 (sweep walk-down) folded in the observed cadence asymmetry. Verify footer notes items 1–3 add new host pins. Title bumped to 6.0.24.
- `docs/Version.md` — this entry; `add` + `trim --keep 20` dropped the oldest entry off the tail.
- `include/AutoLink.h` / `library.properties` / `idf_component.yml` — `6.0.23 → 6.0.24` bump in lockstep (AGENTS rule 3).

### Wire format

Unchanged. No `.cpp` / `.h` / `.ino` touched. Same framer shape, COBS encap, `MAX_CHUNK = 250`, `MSG_HDR = 6`, `POOL_SIZE = 64`, window 32, and seq-space budget as 6.0.23. This entry triages the defect into todo.md; it does not fix it.

### Regression test

None added — docs-only, no behavior to pin. The fixes themselves (todo Open 1–3) each carry a host pin to write when the code lands: a window-ceiling loopback with one induced in-window loss asserting a cache hit and `discCount == 0`; an idle-past-`idleTimeoutMs` loopback with no pending asserting `discCount == 0`; and a `LinkReorder` table test for stepping over an unfillable hole after `reorderHoldMs`. The host suite is the gate and is identical to 6.0.23: `make test` 62/62 unit, `make itest` 3/3.

### Limitations

- The flood cascade, idle-watchdog loop, and gap-stall (todo Open 1–3) stay Open — this release only triages them. The 503 OTA stubs and reserved r10/r11 slots are unchanged from 6.0.22.
- Standing cross-compile carry-over: `build/verify_build.sh` was not re-run for 6.0.24 (no source delta; the 6.0.22 cross-compile last cleared the build path at 1027671 / 79320 bytes). No source on top of 6.0.23, so the risk stays bounded to the unchanged build path.
- The bench log is hardware-captured; items 5–7 remain FireBeetle-pair-only and are not host-pinnable.

### Result

- No source touched; the host suite is unchanged from 6.0.23 (`make test` 62/62 unit, `make itest` 3/3) — not re-run this session (docs-only).
- `python3 build/version.py check` expected green (20 entries, --keep=20).
- `make assets_check` / `make test_coverage_manifest` unaffected — no dashboard source, no new TEST_BINS.
---

## v6.0.23

**todo.md reorg + Version.md hygiene (docs only)**

Documentation-only housekeeping, same shape as 6.0.20. No source, wire-format, build-surface, or test-behavior change — the protocol, ARQ, framing, and seq budget are byte-for-byte identical to 6.0.22. `todo.md` is reordered most-important-first and trimmed to the bare minimum a developer needs to pick the work back up: OTA is the single headline Open item with its firmware/gui/partition sub-steps inlined, and the previously-disclosed "OTA stub 503s without draining the request body (half-read socket)" limitation is promoted from a recurring Version.md footnote into the OTA item's first concrete sub-step instead of being re-disclosed each cycle. No completed items remain in `todo.md` — Open 1 and Open 2 were closed in 6.0.22, so this pass moves nothing out, it only re-files what is still Open. `docs/Version.md` stays the single source of truth for release detail; the dangling `---` separator at the file tail is removed and `trim --keep 20` drops v6.0.2.

### What moved

- `todo.md` — reordered most-important-first; OTA collapsed to one Open item (1) with the firmware / gui / partition sub-steps inlined; the half-read-body drain promoted into sub-step 1; hardware-bench items (2–4) and the Verify footer carried over unchanged; title bumped to 6.0.23.
- `docs/Version.md` — this entry; `trim --keep 20` dropped v6.0.2; trailing `---` separator artifact removed.
- `include/AutoLink.h` / `library.properties` / `idf_component.yml` — `6.0.22 → 6.0.23` bump in lockstep (AGENTS rule 3).

### Wire format

Unchanged. No `.cpp` / `.h` / `.ino` touched. Same framer shape, COBS encap, `MAX_CHUNK = 250`, `MSG_HDR = 6`, and seq-space budget as 6.0.22.

### Regression test

None added — docs-only, no behavior to pin. The host suite is the gate and is identical to 6.0.22: `make test` 62/62 unit, `make itest` 3/3.

### Limitations

- Standing cross-compile carry-over: `build/verify_build.sh` was not re-run for 6.0.23 (no source delta; the 6.0.22 cross-compile already cleared the build path at 1027671 / 79320 bytes). No source on top of 6.0.22, so the risk stays bounded to the unchanged build path.
- The OTA work itself (todo item 1) stays Open; this release only re-files it, it does not implement it. The 503 stubs and the reserved r10/r11 slots are unchanged from 6.0.22.

### Result

- No source touched; the host suite is unchanged from 6.0.22 (`make test` 62/62 unit, `make itest` 3/3).
- `python3 build/version.py check` green (20 entries, --keep=20; this entry pushed v6.0.2 off the tail).
- `make assets_check` / `make test_coverage_manifest` unaffected — no dashboard source, no new TEST_BINS.
---

## v6.0.22

**Fix stale dashboard byte-count test + reserve OTA URI-handler slots (closes Open 1 + 2)**

Two todo.md items landed together because they're the only two this release can host-pin: the dashboard-byte-count literal went stale against the regenerated header and is now computed from the parts; the OTA URI-handler cap was 10-of-10 saturated, blocking the `/ota/fw` and `/ota/gui` endpoints that Open 3 needs. The release reserves those two slots (handlers return 503 with a clear message until the stream-to-flash + LittleFS paths land) and bumps `cfg.max_uri_handlers` from 10 to 12 in lockstep with `URIS[]` / `PATHS[]`. No wire-format, ARQ, framer, or seq-budget change — the protocol is byte-identical to 6.0.20. Source-touching release, so the cross-compile carry-over that 6.0.14–6.0.20 carried (no source delta) is live again: this is the first source-touching tag since 6.0.13 where `AutoLinkWeb.cpp` / `AutoLinkWebHandlers.cpp` actually change. `build/verify_build.sh` ran in-sandbox against `esp32:esp32:firebeetle32` (`arduino-cli` + esp32 core installed fresh this session), 1027671 bytes / 79320 bytes RAM — a 548-byte program-space delta vs. 6.0.19 baseline, consistent with the two stub handler bodies and the +2 handler-table entries. The cross-compile gate is green.

### What moved

- `build/dashboard_assets-test.py` — Open 1 fix. The pin that previously asserted `runtime == 31222` (a hardcoded snapshot from an early release) is replaced with `expected = sum(sizes.values()) + 1`, computed from the per-part decomposition in `_runtime_size_from_header`. Each part now has a positivity check (`part_size > 0`) so an empty part silently sneaking into the header trips the gate. The byte-count line is structurally honest: it asserts "the runtime equals what we just summed", which is the only contract the chunked-send loop has.
- `src/al/web/AutoLinkWeb.cpp` — Open 2. `cfg.max_uri_handlers = 10` becomes `12` (the +2 headroom for OTA). Two new handler slots: `const httpd_uri_t r10 = { "/ota/fw", HTTP_POST, handleOtaFw, this };` and `const httpd_uri_t r11 = { "/ota/gui", HTTP_POST, handleOtaGui, this };`. `URIS[]` and `PATHS[]` grow in lockstep, preserving the index-aligned `PATHS[i] / URIS[i]` contract that the source-grep alignment test pins.
- `src/al/web/AutoLinkWeb.h` — two new `static esp_err_t` declarations: `handleOtaFw(httpd_req_t*)` and `handleOtaGui(httpd_req_t*)`.
- `src/al/web/AutoLinkWebHandlers.cpp` — two stub implementations. Both return `503 Service Unavailable` with a body of `"OTA firmware/gui upload not yet implemented"` and a `warning`-level log line so a user probing the device sees the reservation rather than a 404 (which would mask that the slot is taken). The actual `esp_ota_*` / LittleFS wire-up lands in Open 3.
- `test/test_desktop/al/web/UriHandlerAlignmentTest.cpp` — `10 → 12` everywhere the count is asserted (`n == 10`, `uris.size() == 10`, `paths.size() == 10`, `seen.size() == 10`, `value == "10"`), plus `/ota/fw` and `/ota/gui` added to the `mustHave[]` route roster so the part-of-PATHS pin catches a future refactor that drops either OTA route. The function names `test_max_uri_handlers_is_10` / `test_paths_array_contains_all_ten_routes` become `_is_12` / `_all_twelve_routes` to match.
- `todo.md` — Open 1 and Open 2 closed; the OTA work itself (Open 3, firmware + GUI OTA, partition table) stays Open and unchanged. Verified-done gains a 6.0.22 pointer (one-liner; detail lives here).
- `include/AutoLink.h` / `library.properties` / `idf_component.yml` — `6.0.20 → 6.0.22` bump in lockstep (AGENTS rule 3).

### Wire format

Unchanged. The OTA routes are HTTP, not AutoLink-wire. No `Link.cpp` / framer / seq-budget / ARQ-cache delta. `MAX_CHUNK = 250`, `MSG_HDR = 6`, the COBS encap, and the `cobsSeq` space are byte-identical to 6.0.20.

### Regression test

- `run_test_uri_handler_alignment` updated to pin the new 12-handler / 12-cap contract across all four sub-tests: (1) `cfg.max_uri_handlers = 12` in the httpd config block; (2) exactly 12 `r<N>` declarations match the cap; (3) `PATHS[12]` parallel to `URIS[12]`, every `PATHS[i]` matches the path declared in `r<N>`; (4) `PATHS[]` contains all 12 unique routes including `/ota/fw` and `/ota/gui`. Reverting either OTA slot (or any of the existing 10) trips the alignment pin.
- `run_test_esp_idf_error_etiquette` — the `httpd_register_uri_handler` word-boundary match in the source-grep pin is unchanged; the new code paths (`Log::log().warning(...)`, the 503/200 response shapes) are within the 15-line window the test checks for the call site.
- `run_test_compile_check` — `AutoLinkWeb.cpp` and `AutoLinkWebHandlers.cpp` both parse cleanly under `-DARDUINO=10607 -DAUTOLINK_USE_ESP_TIMER` against the host stubs. The OTA stubs only call ESP-IDF functions already declared by the stub `esp_http_server.h` (`httpd_resp_set_status`, `httpd_resp_set_type`, `httpd_resp_set_hdr`, `httpd_resp_send`).
- `run_test_version_free_source` — the dashboard_assets-test.py comment rewrite drops the prior `5.4.3` / `6.0.18` / `31222` / `32094` version-literal anchors (rule 12). The test no longer references any specific version or byte-count snapshot.
- `build/dashboard_assets-test.py` — the byte-count pin is now `expected = sum(sizes.values()) + 1` (computed) plus a per-part positivity check (`part_size > 0`). Idempotency, `{{VERSION}}`-marker, AUTOLINK_VERSION token-count, dashboard.js content, and dashboard.css version-marker checks are unchanged.

### Disclosed limitations

- The OTA stubs return 503 but do not consume the request body. A `POST /ota/fw` with a real firmware blob leaves the connection half-read; the httpd layer eventually times out and closes. Open 3 will wire up `httpd_req_recv` (or a chunked receive) into `esp_ota_write` for the firmware path; for the gui path it streams to LittleFS. The 503-without-body-consumption shape is acceptable as a pre-Open-3 placeholder because the routes are reserved, not implemented.
- `cfg.max_uri_handlers = 12` is a permanent cap; bumping it past 12 in a future release (say 14 for `/ota/fw/rollback` + `/ota/gui/clear`) requires a new entry in `URIS[]` / `PATHS[]` and a new pin in `mustHave[]`. The test invariants make the next bump mechanical but not free.
- `run_test_uri_handler_alignment` is a source-grep suite (the AutoLinkWeb TUs are `#ifdef ARDUINO` and can't run on host). It pins the count + order + completeness; a runtime regression (e.g., a `httpd_register_uri_handler` that returns `HANDLERS_FULL` silently) is caught by the alignment test's count invariant only if it surfaces in the source (a 12-of-12 race wouldn't). Out of scope for this release.
- The cross-compile gate ran in-sandbox this session against a freshly-installed `arduino-cli 1.5.1` + `esp32:esp32@3.3.5` (`arduino-cli core install esp32:esp32@3.3.5` completed before the verify). The 6.0.14–6.0.20 cross-compile carry-over is closed; 6.0.22 is the first source-touching release since 6.0.13 where the gate actually ran end-to-end in the sandbox.
- Open 3 (the OTA stream-to-flash + LittleFS wire-up) stays Open. The hardware bench items (renumbered 2–4 in `todo.md` after this release: ASYNC heap headroom, flood frameErrs=0, sweep walk-down) are unchanged — they require the FireBeetle pair and aren't host-pinnable.

### Result

- `make test` 62 / 62 unit suites pass (wall ~6.4 s). The `run_test_uri_handler_alignment` suite was the only source-grep pin that needed updating; it now asserts the 12-handler / 12-cap contract.
- `make itest` 3 / 3 host integration suites pass (wall ~40 s). Loopback / loopback_noise / loopback_sync unchanged from 6.0.20.
- `make assets_check` PASS — `AutoLinkWebHtml.h` byte contract unchanged (no dashboard source touched; the byte-count fix is in the test only).
- `make test_coverage_manifest` PASS — no new TEST_BINS entries; the two new stub functions live in `AutoLinkWebHandlers.cpp` which is excluded from the host suite's link set (same shape as the existing handlers). No manifest changes.
- `python3 build/pretty_print.py` — the touched files (`AutoLinkWeb.h`, `AutoLinkWeb.cpp`, `AutoLinkWebHandlers.cpp`, `UriHandlerAlignmentTest.cpp`, `dashboard_assets-test.py` for source-style check) format cleanly.
- `python3 build/dashboard_assets-test.py` PASS — `runtime=32094B`, sha256 unchanged from 6.0.20, sizes dict unchanged.
- `python3 build/version.py check` green (20 entries, --keep=20; this entry pushed v5.4.4 off the tail).
- `build/verify_build.sh` PASS — ran in-sandbox this session against `esp32:esp32:firebeetle32` (`arduino-cli 1.5.1` + `esp32:esp32@3.3.5` installed fresh). `Sketch uses 1027671 bytes (78%) of program storage space; Global variables use 79320 bytes (24%)`. Vs. the 6.0.19 baseline (1027123 / 79312) the deltas are +548 bytes program-space and +8 bytes RAM, consistent with the two stub handler bodies + the +2 handler-table entries.

### Verification artifacts

- `make test` wall: 6363 ms (62 unit suites).
- `make itest` wall: 40096 ms (3 integration suites).
- Combined wall: 46.5 s.
- Peak RSS: 88968 KiB (largest single-suite resident set; unchanged from 6.0.20).
- Cross-compile wall: ~3 min on a fresh `arduino-cli` + `esp32:esp32@3.3.5` toolchain install; ~30 s on a cached install.
- Program-space delta vs. 6.0.19 baseline: +548 bytes (two stub bodies + the +2 handler-table entries).
---

## v6.0.20

**todo.md reorg + Version.md cleanup; track stale dashboard-asset byte count (docs only)**

Documentation-only housekeeping. No source, wire-format, build-surface, or test-behavior change — the protocol, ARQ, framing, and seq budget are byte-for-byte identical to 6.0.19. `todo.md` had accumulated ten full multi-paragraph Verified-done blocks (6.0.10–6.0.19) that duplicated this file's canonical entries verbatim; that duplication is the drift hazard `version.py` exists to prevent (the two copies can disagree). This release collapses `todo.md`'s Verified-done section to one-line-per-release pointers and makes `docs/Version.md` the single source of truth for release detail. It also removes the closed Open 5 stub from `todo.md`'s `## Open` list (the `bytesRecvdForMessage` consumer landed in 6.0.19), renumbers the survivors with no gaps, and adds one genuinely untracked item: the stale `dashboard_assets-test.py` byte-count expectation. Two cosmetic `>` typos at the tails of the v6.0.3 and v6.0.1 entries in this file are removed.

### What moved

- `todo.md` — Verified-done collapsed from full blocks to one-liners (canonical detail now lives here); title bumped to 6.0.20; closed Open 5 removed from `## Open`; remaining Open items renumbered gap-free (old Open 6 → Open 5); new Open 6 tracks the stale dashboard-asset byte count; Verify + Hardware-re-test sections updated for 6.0.20.
- `docs/Version.md` — this entry (and `trim --keep 20` dropped v5.4.3); stray trailing `>` removed from the v6.0.3 and v6.0.1 entry tails.
- `include/AutoLink.h` / `library.properties` / `idf_component.yml` — `6.0.19 → 6.0.20` bump in lockstep (AGENTS rule 3).

### New tracked item — stale dashboard-asset byte count (todo Open 6)

`build/dashboard_assets-test.py` hard-codes `expected = 31222` for the runtime size of `DASHBOARD_HTML`, but the regenerated header is now `32094` bytes (parts: A 186 + CSS 4980 + B 5967 + JS 20935 + C 25 = 32093, plus the 1-byte concatenation terminator). The expectation has been stale since at least 5.4.3 (then 31222 vs 31801) and has drifted further as the JS grew. The generated header itself is correct and current (`make assets_check` regenerates byte-identically); only the test's literal is wrong. Promoted from a recurring out-of-scope footnote to a tracked Open item so a future release fixes the literal (or replaces it with a sum-of-parts assertion) rather than re-disclosing it every cycle.

### Wire format

Unchanged. No `.cpp`/`.h`/`.ino` touched. Same framer shape, COBS encap, `MAX_CHUNK = 250`, `MSG_HDR = 6`, and seq-space budget as 6.0.19.

### Regression test

None added — docs-only, no behavior to pin. The host suite is the gate and is identical to 6.0.19: `make test` 62/62 unit, `make itest` 3/3.

### Limitations

- Standing cross-compile carry-over: `build/verify_build.sh` was not re-run for 6.0.20 (no source delta; the 6.0.19 cross-compile already cleared the build path). No source on top of 6.0.19, so the risk stays bounded to the unchanged build path.
- The dashboard-asset byte-count fix is tracked, not applied here — this release only records it as Open 6.

### Result

`make test` 62/62 (~5.8 s wall), `make itest` 3/3 (~40 s wall), `python3 build/version.py check` green (20 entries), `python3 build/dashboard_assets-test.py` regenerates `AutoLinkWebHtml.h` byte-identically to the shipped copy (its `expected` literal still mismatches — now Open 6). No hardware delta vs. 6.0.19.
---
