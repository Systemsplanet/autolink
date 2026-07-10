# todo.md — v6.1.13

Release history: `docs/Version.md`. Closed/archived items live in
`docs/Version.md` and are not re-listed here.

## 1. Cross-compile gates not run this session

No network egress in this sandbox: `bash build/verify_build.sh` and
`bash build/check_arduino_iface.sh` were not run against
`esp32:esp32@3.3.5` / `esp32:esp32:firebeetle32`. Re-run both before
ship per AGENTS rule 4.

## 2. Hardware bench validation (physical FireBeetle pair)

- GBN loss floor: reproduce sustained frame loss on real UART noise
  (not just MockHal) and confirm the loss-sweep floors (99% @ 1% loss)
  hold outside simulation.
- Re-lock cadence symmetry, sweep walk-down, ASYNC flood bench, OTA
  (both upload paths).
- Confirm split-delivery CTRL frames (the v6.1.7 `okCarry_` fix)
  reassemble cleanly when a real UART read lands mid-candidate — the
  host itest exercises this via hand-split `onRx()` chunks, not an
  actual fragmented hardware read.

## 3. (Archived v6.1.11) LCK state machine arm is unreachable

Closed in v6.1.11 via option (a) — full LCK state-machine deletion
(see `docs/Version.md` v6.1.11 "What moved"). State::LCK +
handleLck_unlocked + onTimerLck_unlocked + lckRetries_ + the
decideLckTick / LckAction helpers all gone; the wire vocabulary
(REQ_CMD / LOCK_CMD) stays for any future option (b) re-introduction.

## 4. (Archived v6.1.11) Sweep-timeout arms uncovered by host tests

Closed in v6.1.11 — new `run_test_sweep_timeout_arms` clock-injected
host pin (see `docs/Version.md` v6.1.11 "What moved"). Four pins:
master P3 Stay / FallbackLockSlowest, pong P2 Stay / DropToPhase1.
The arms now have a regression pin; their behavior is the
documented-and-tested shape going forward.

## 5. (Archived v6.1.12) SYNC RTO ladder low coverage — illusory

Closed in v6.1.12 — re-audit found the gap was illusory. The SYNC
ladder (`syncRtoStep_unlocked` / `syncAwaitAcked_unlocked`) is already
pinned by `run_test_sync_resync_spiral::test_pin_sync_retx_ladder_resends_before_drop`
(4 LCs of `test_syncRtoStep` + `test_syncAttempt`). The side-effect
side (`onSyncAckTimeout_unlocked` mid-message drop + BREAK, single-
frame `noteTxReject_unlocked` only) is pinned by
`run_test_sync_stall_watchdog` (Pins 3-4). All four target paths are
host-covered; the v6.1.10 todo's "only via itest" claim was wrong.
See `docs/Version.md` v6.1.12.

## 6. (Archived v6.1.12) findMsgHeaderResync_unlocked — illusory

Closed in v6.1.12 — re-audit found the gap was illusory.
`LinkRx.cpp::findMsgHeaderResync_unlocked` is host-covered by three
existing unit suites:
- `run_test_alink_message_corrupt` (`test_corrupt_msg_header_resync_to_next_message`
  + `test_corrupt_msg_header_no_resync_clears_buffer`) drives the
  function through the recvMsg public surface with injected
  corrupt-then-valid + all-junk appBuf payloads.
- `run_test_alink_message_resync` (oversize-L header, junk-prefix
  resync, false-boundary rejection, multi-chunk loss surfaces).
- `run_test_msg_codec` (the pure-decision shim — `msgResyncScan`).

The function is not directly named in any of these tests (the
recvMsg wrapper calls it), so gcov's per-symbol accounting reads
0%, but the runtime contract is fully pinned. See
`docs/Version.md` v6.1.12.

## 7. (Closed v6.1.12) AutoLinkConfig clamp helpers — inlining artifact documented

Closed in v6.1.12. The 0% gcov reading on `clampToMaxBauds()` /
`allowedBaudSafe()` is an attribution artifact (both functions are
defined in `src/al/AutoLinkConfig.h` and gcc inlines them into every
Link.cpp site that calls them). Action taken: `clampToMaxBauds()` got
an explicit `inline` keyword (was implicit) so future maintainers
read the inlining as a documented decision rather than a missing
declaration. The function's behavioral round-trip is covered by
the broader Link suite that drives baud-index flows.

## 8. (Closed v6.1.12) Not-host-testable TUs — documented

Closed in v6.1.12. The three TUs are intentionally uncovered by
host tests:
- `src/al/link/Link.cpp` is an include-only TU (1 line: `#include
  "al/link/Link.h"`); its purpose is to give the Arduino toolchain
  a single-file compilation unit for the inline definitions in
  `Link.h`. The host suite pulls in every fragment directly.
- `src/al/web/AutoLinkWeb.cpp`, `AutoLinkWebHandlers.cpp` are
  `#ifdef ARDUINO`-gated handlers depending on `WiFi` /
  `Preferences` / `LittleFS` and need the ESP32 toolchain.
  CompileCheckTest Pins 1-3 already pin the ARDUINO-gated file
  shape (every guarded source parses cleanly under the stub
  Arduino header). Cross-compile gate (`verify_build.sh`) covers
  the real ESP32 path. No host-testable addition is possible
  without an ESP-IDF host shim that's out of scope.

## Verify
`cd test && make test && make itest` — full suite green this session,
cross-compile gates still UNVERIFIED (item 1, no network). Re-run
gates before any release ship per AGENTS rule 4.
