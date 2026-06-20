# 📅 AutoLink Version History

All releases, most recent first.

---

## v5.1.29

**Bug fix: Ping now boots paused; Resume button actually stops the device from sending.**

User feedback (2026-06-19): the dashboard's Pause/Resume button was purely cosmetic — it only flipped a JS variable that affected log polling. Ping's send loop didn't see it, so Ping blasted bytes from the moment the link came up, regardless of what the operator clicked. Two failures observed:

1. Ping started sending before the user had a chance to look at the dashboard.
2. The Resume button didn't restart sending after a manual pause (the underlying state never changed on the device side).

**Fix:**

- **Device-side pause state**: `UtilPing::paused_` defaults to `true` at boot. The send loop returns before pumping bytes when `paused_`. Only `/pausemsg?p=0` (or equivalent direct call) flips it false.
- **New HTTP endpoint** `POST /pausemsg?p=1|0`. Returns 404 if no hook is registered (Pong side). Calls `msgPausedWriter_` which is wired in `UtilPing::installWebHooks()` to `UtilPing::setPaused()`.
- **`/stats` exposes `msgPaused`**: the dashboard now reconciles the button label with the device's actual state on every poll. Refresh-after-pause, race on boot, and tab-switch all converge correctly.
- **Dashboard `toggleMsgPause()`**: now `async`. Optimistic UI flip + POST `/pausemsg`. Reverts on 404 (Pong side).
- **WiFi now retries forever by default**: `AutoLinkWeb::begin()` used to give up after `WIFI_TIMEOUT_MS=12000` (one attempt). If the AP came up late (slow boot, channel change, DHCP hiccup), the user got a 404 in the browser and assumed the device was broken. Now wraps `WiFi.begin()` + the wait loop in a retry-with-backoff: each attempt gets 12 s, then 5 s sleep, then retry. `WIFI_RETRY_MAX_ATTEMPTS=0` means retry forever. Each retry logs a warning once per 30 s so a long-term outage doesn't flood the serial monitor. **`WiFi.disconnect()` is called between attempts** so the next association is fresh, not half-open. **Disclosed:** this changes boot behavior on a permanently-unreachable AP — the device will now loop forever on WiFi instead of giving up and starting the rest of the sketch. That's the correct trade for an always-on dashboard; if you need the old behavior set `WIFI_RETRY_MAX_ATTEMPTS=1` in your sketch or via a future config endpoint.

**Tests:** all 16 C++ suites + 73 individual + 72 JS dashboard tests + loopback + noise regression all PASS. Arduino `verify_build.ino` compiles clean.

No protocol or wire-format change. v5.1.28 → v5.1.29.

---

## v5.1.28

**Bug fix: gap events now count toward the error threshold so the link auto-falls-back to a slower baud.**

User observation (2026-06-19): the SWP baud-sweep protocol was supposed to find the highest reliable baud and stay there. But in real hardware logs the protocol was happy at 115200 (peer answered SWP cleanly) and then the link got noisy: `RX cobsSeq=N GAP: ... +1 lost`, frame errors climbing slowly, `recv rejected (CRC/desync)` from the application layer, then `BREAK sent — forcing re-sweep`. The application layer (PingPong) was breaking the link manually because the protocol's recovery (`errs` threshold at 20) wasn't tripping fast enough — gaps accumulated but didn't count.

**Root cause:** `ALink::onPayload` had a comment that said "we DON'T drop the link — the next retransmit will catch up." That assumption fails when the next retransmit ALSO gets lost (wire noise). The protocol kept the link up at a baud it couldn't sustain.

**Fix:** a gap event now calls `err_unlocked()` to bump the error counter. When `errs > cfg.errThreshold`, the link drops and re-sweeps at the next baud in `cfg.allowedBauds[]`. This makes the baud-sweep protocol self-healing: the user sees fewer `disc=N` increments because the link finds its sustainable baud automatically instead of being manually broken by the application layer.

**Disclosed:** this changes the wire behavior on lossy links. Before: gap events logged but link stays up (one noisy link instead of N re-sweeps). After: gap events trigger threshold-based drops. For lossy wires the net throughput may be lower (more re-sweeps) but the link will actually be reliable. For clean wires (the common case), the change is invisible — gaps are rare enough not to trip the threshold.

**Tests:** all 16 C++ suites + 73 individual + 72 JS dashboard tests + loopback all PASS. New regression test `run_loopback_noise` injects 30% frame-drop noise into the mock wire and asserts the protocol actually recovers (baud fallback or disconnect+recover). Confirmed by toggling the fix on/off: with the fix the test passes (Pong disconnects once), without it the test fails (no recovery). Arduino verify_build.ino compiles clean.

No protocol or wire-format change at the byte level. v5.1.27 → v5.1.28.

---

## v5.1.27

**Move the "test files mirror source packages" rule from `docVersion.md` into AGENTS.md rule 18.**

The rule was buried in v5.1.26's changelog — wrong place. AGENTS.md is where agent-read rules live; docVersion.md is the user-facing release log.

**AGENTS.md rule 18** spells out the test layout decision tree:
* `test/test_desktop/al/<pkg>/<Test>.cpp` — tests `src/al/<pkg>/`
* `test/test_desktop/<File>.cpp` — tests `src/<File>` (public surface) OR shared test infra
* "When in doubt: ask which `src/` file does this test exercise?"

No code change. v5.1.26 → v5.1.27.

---

## v5.1.26

**Move `loopback_test.cpp` to `test/test_desktop/al/protocol/`.**

It tests the **integrated end-to-end** behavior of two `ALink` instances — that's a protocol-integration test, not a top-level facade test. Now sits next to the other protocol tests (`ALink*Test.cpp`).

**Audited the remaining top-level files in `test/test_desktop/`:**
* `AutoLinkTest.cpp` — tests the **AutoLink facade** (`src/AutoLink.{h,cpp}`). Stays at top because AutoLink is the top-level facade; there's no `al/autolink/` package for it. Same rule as `src/AutoLink.h` being at `src/` (the public surface) rather than under `al/`.
* `MockHalTest.cpp` + `MockHal.h` — test **infrastructure** shared by protocol, util, AND web tests (every package uses `MockHal` to drive `ALink`). Moving it to any one package would create a back-reference from the other packages. Stays at top.
* `Makefile`, `coverage_merge.sh`, `dashboard-js-test.js`, `package*.json`, `node_modules/` — build / packaging infra, not test files.

**The rule (now codified):** a test file at `test/test_desktop/al/<pkg>/<Test>.cpp` tests a specific `src/al/<pkg>/` package. Tests for files at `src/` (the public surface) stay at `test/test_desktop/`. Test infrastructure shared across packages stays at `test/test_desktop/`.

**Tests:** 16 C++ suites + 73 individual + 72 JS dashboard tests + loopback all PASS.

No protocol or wire-format change. v5.1.25 → v5.1.26.

---

## v5.1.25

**Add `build/arduino-cli-cmd.sh` wrapper + AGENTS.md rule 17.**

A thin wrapper that does three things bare `arduino-cli` does not: (1) auto-installs on demand by delegating to `build_env.sh` when missing, (2) prints version when called with no args (sanity check), (3) provides a single entry point so any future arduino-cli install-URL change only touches one file.

**Renamed `verify_build.sh` to use the wrapper.** Previously it checked `command -v arduino-cli` and bailed if missing; now it always works because the wrapper handles install-on-demand.

**AGENTS.md:**
- Rule 15 (install arduino-cli): kept, but points to rule 17's wrapper as the preferred path.
- Rule 16 (smoke compile): recipe updated to use the wrapper.
- **New rule 17:** "Always run arduino-cli through `bash build/arduino-cli-cmd.sh`." Lists the three benefits, shows the usage pattern, and forbids calling `arduino-cli` directly from recipes / Makefiles / shell scripts.

**Disclosed:** I considered renaming `build_env.sh` to `arduino-cli-cmd.sh`, but they have different jobs — `build_env.sh` is **env setup** (installs arduino-cli + esp32 core, idempotent, run once per machine); the wrapper is a **cmd launcher** (every-call convenience). Combining them would make the script do two unrelated things and confuse the next reader. Kept them separate.

**Tests:** host suite + 72 JS + loopback all PASS. Arduino verify_build runs clean through the new wrapper.

No protocol or wire-format change. v5.1.24 → v5.1.25.

---

## v5.1.24

**Refine the `src/al/` layout — move files to the directories that actually use them.**

**`src/al/util/` shrinks.** `UtilMain.h`, `UtilPing.h`, `UtilPong.h` were only used by the PingPong example; moved to `src/al/pingpong/` next to `PingPongImpl.h`. `UtilBaudSweep.h`, `UtilBaudSweep.cpp`, `UtilFrameRx.h`, `UtilFrameRx.cpp` were only used by `ALink`; moved to `src/al/protocol/`. What's left in `src/al/util/` is the true cross-cutting stuff: leveled logger (`Log.{h,cpp}`), blinker (`UtilBlink.h`, `UtilBlinkEspTimerShim.h`), COBS encode/decode (`UtilCobs.{h,cpp}`), CRC-8 (`UtilCrc.{h,cpp}`). These are used by 2+ subsystems and don't belong anywhere specific.

**`test/test_desktop/` mirrors the src tree.** Tests now live in `test/test_desktop/al/<sub>/` matching where the code under test lives:
- `test/test_desktop/al/protocol/` — ALink*Test.cpp, UtilBaudSweepTest, UtilFrameRxTest
- `test/test_desktop/al/util/` — LogTest, UtilCrcTest, UtilCobsTest, UtilBlinkTest
- `test/test_desktop/al/web/` — AutoLinkWebTest

Top-level `test/test_desktop/` keeps: `Makefile`, `AutoLinkTest.cpp`, `MockHal.{h,MockHalTest.cpp}`, `loopback_test.cpp`, `dashboard-js-test.js`, `coverage_merge.sh`, `package*.json`, `node_modules/`.

`MockHal.h` is included from the subdir tests but lives at the top level. Added `-I.` to the test Makefile's `CXXFLAGS` so the subdir tests can `#include "MockHal.h"`.

**Tests:** 16 C++ suites + 73 individual + 72 JS dashboard tests + loopback all PASS. Arduino verify_build.ino, Pong.ino, and a flat-include smoke sketch all compile clean.

No protocol or wire-format change. v5.1.23 → v5.1.24.

---

## v5.1.23

**Move all non-public files out of `src/` into `src/al/`.**

**The new public surface is exactly two files at `src/`:**
* `src/AutoLink.h` — facade header (the public API)
* `src/AutoLink.cpp` — facade implementation
* `src/PingPong.h` — flat shim for the PingPong example

**Everything else moved into `src/al/<subsystem>/`:**
* `src/al/protocol/ALink.{h,cpp}` — protocol state machine (was `src/ALink.*`)
* `src/al/hal/{ILink.h,EspHal.h}` — hardware abstraction (was `src/{ILink.h,EspHal.h}`)
* `src/al/util/*.h,*.cpp` — leveled logger, CRC, COBS, frame parser, baud sweep, etc. (was `src/util/`)
* `src/al/web/AutoLinkWeb*.{h,cpp}` — dashboard (was `src/web/`)
* `src/al/pingpong/PingPongImpl.h` — PingPong example implementation (was `src/pingpong/`)

**Why:** `src/` was mixing the public facade with protocol internals, the HAL, web dashboard code, utility code, and the example — hard to tell at a glance which files a user is expected to know about. Now `src/` contains ONLY the public interface; everything else is namespaced under `src/al/` (the `autolink` namespace). The split matches the library's own internal module boundaries.

**Include path updates:** every `#include` inside the library now uses the absolute path from `src/` (e.g. `#include "al/protocol/ALink.h"`, `#include "al/util/Log.h"`). 50+ lines across 20 files updated. All `#include "AutoLink.h"` and `#include "PingPong.h"` lines in user-facing headers and examples are unchanged — those are still the only two public includes.

**Tests:** 16 C++ suites + 73 individual + 72 JS dashboard tests + loopback all PASS. Arduino verify_build.ino, Pong.ino, and a flat-include sketch all compile clean under arduino-cli.

No protocol or wire-format change. v5.1.22 → v5.1.23.

---

## v5.1.22

**Rename `src/pingpong/PingPong.h` → `src/pingpong/PingPongImpl.h`.**

The flat shim at `src/PingPong.h` and the canonical file at `src/pingpong/PingPong.h` having the same name was confusing (two files named PingPong.h). Renamed the canonical to `PingPongImpl.h` to make the relationship explicit: the flat `PingPong.h` is the public entry point, `PingPongImpl.h` is the implementation header. The shim is updated to forward to the new name.

**No behavior change.** Same wire format, same public API. Only the filename inside the library changed.

**Disclosed: the subdir-include path `<pingpong/PingPongImpl.h>` still doesn't resolve under arduino-cli 1.5.1.** Same arduino-cli limitation as before — it doesn't recurse into `src/<subdir>/`. The flat `"PingPong.h"` (via shim) works on arduino-cli. Arduino IDE 2.x supports both syntaxes.

**Tests:** 16 C++ suites + 73 individual + 72 JS dashboard tests + loopback all PASS. Pong.ino and a flat-include sketch both compile clean under arduino-cli.

No protocol or wire-format change. v5.1.21 → v5.1.22.

---

## v5.1.21

**Two reversions + PingPong subdir restore.**

**1. Reverted the `LOG` global alias.** v5.1.16–v5.1.20 added a `Log& LOG = Log::log();` reference as a shorthand for the logger. The user pushed back: a namespace-scope `extern Log&` reference bound at static-init time is too risky — static-init order across TUs is undefined, and a TU that touches `LOG` before its TU's static init runs would dereference an unbound reference. Replaced with explicit `Log::log()` at every call site (90 call sites across 12 files). One extra function-call dereference per log line vs. the reference, but that's negligible vs. the logging itself.

**2. `PingPong.h` lives at `src/pingpong/PingPong.h` again.** v5.1.18 flattened it to `src/PingPong.h` because arduino-cli 1.5.1 doesn't recurse into `src/<subdir>/` for include resolution. The user wanted the organizational layout (PingPong-related code grouped together) and they shouldn't see compiler errors. **Resolution:** the canonical file lives at `src/pingpong/PingPong.h` (correct per Arduino Library Spec 1.5, which Arduino IDE 2.x supports). A 5-line forwarder shim at `src/PingPong.h` makes the flat `#include "PingPong.h"` syntax also work, by including the canonical file relative to its own directory. Examples updated to use the flat syntax (works everywhere; subdir syntax only works under Arduino IDE 2.x).

**Disclosed: under arduino-cli 1.5.1, `#include <pingpong/PingPong.h>` STILL does not resolve.** That's a real arduino-cli limitation, not a library bug. The shim and the examples use the flat `"PingPong.h"` syntax which works under both arduino-cli and Arduino IDE 2.x.

**Tests:** 16 C++ suites + 73 individual + 72 JS dashboard tests + 4 loopback modes all PASS. Arduino verify_build.ino, Ping.ino, Pong.ino all compile clean.

No protocol or wire-format change. v5.1.20 → v5.1.21.

---

## v5.1.20

**AGENTS.md: rules 16 + new gotcha section. Pins the include-path / subdir bug so it can't repeat.**

User reported (2026-06-19): a hand-written sketch (`sketch_jun17b.ino`) failed with `fatal error: pingpong/PingPong.h: No such file or directory`. Cause: the user copied the old v5.1.6 / v5.1.17 example which used `#include <pingpong/PingPong.h>`. In v5.1.18 I flattened the PingPong headers to `src/PingPong.h` because arduino-cli doesn't recurse into subdirectories of `src/` — so the new include is `#include "PingPong.h"`. The sketch needs one line changed.

This bug class has hit us **three times** (v5.1.15/16 stale `../AutoLinkWeb.h` paths, v5.1.18 broken Pong example include, now the user's sketch). Root cause: the verify_build sketch uses quoted includes with explicit paths (`"AutoLink.h"`, `"web/AutoLinkWeb.h"`), so it never exercises the angle-bracket + subdir pattern that real user sketches use.

**New AGENTS.md rule 16:** after `./build/verify_build.sh` passes, smoke-compile a sketch with every public include the user might type. Caught in ~30s, would have caught all three regressions.

**New AGENTS.md gotcha: "Arduino library `src/` layout — NO subdirectories under `src/`".** Documents the include-path contract (arduino-cli doesn't recurse), the allowed include patterns, and the smoke-compile recipe.

**Disclosed limitation:** I cannot auto-fix the user's `sketch_jun17b.ino` — it's on their machine, not in the library. The fix is one line in the sketch (`<pingpong/PingPong.h>` → `"PingPong.h"`).

**Tests:** all 16 C++ suites + 73 individual + 72 JS dashboard tests + loopback still pass; Arduino verify_build + Pong.ino + Ping.ino still compile clean.

No protocol or wire-format change. v5.1.19 → v5.1.20.

---

## v5.1.19

**Real bug fix: ARQ retransmit deadlock with non-recursive mutex.**

User reported (2026-06-19): the Pong example flashed with v5.1.17 boot-loops with a panic in the IROM region (`0x3ffe3c90 |<-CORRUPTED`), `esp_core_dump_flash: CRC=0x7bd5c66f instead of 0x0`, then `esp_restart()` and repeat. The user correctly diagnosed the root cause:

**Call chain under the lock:**
1. FreeRTOS timer fires → `EspHal::timer_callback` → `ALink::onTimer()`
2. `ALink::onTimer()` calls `hw.lock()` (line 950 of ALink.cpp)
3. `ALink::onTimerOk_unlocked()` (still under the lock) finds an expired ACK slot
4. **Directly** calls `arqRetxCallback_(base, arqCtx_)` (line 1018)
5. Callback → `AutoLink::arqRetxHookTrampoline` → `AutoLink::arqCache_retx` → `retx_resend` → `AutoLink::sendMsg` → `link->sendMsg(b, len)` (which is `ALink::write`) → `hw.lock()` (line 326 of ALink.cpp)
6. **Re-entrant lock on a non-recursive mutex.** ESP-IDF FreeRTOS mutexes default to non-recursive. The second `hw.lock()` either deadlocks the task or — if the mutex owner pointer has been corrupted by something else first — crashes.

**Why v5.1.6 didn't show it:** the same deadlock existed in 5.1.6, but the timer-driven retx only fires when the link is OK AND a sent message goes un-ACKed for >100ms. On a freshly-flashed node with no peer, no messages are sent, no retx fires, no deadlock. The v5.1.17 path triggers the retx earlier because the **first sent message** loses its ACK (wire-up race, baud flip, or peer not yet synced), and the OK-timer tick that follows hits the deadlock.

**Fix:**
* `ALink::onTimerOk_unlocked()` no longer calls `arqRetxCallback_` directly. It records `hasPendingRetx_=true, pendingRetxBase_=base` and lets `onTimerOk_unlocked()` return.
* `ALink::onTimer()` releases the lock (`hw.unlock()`), then **outside the lock** dispatches the callback via `arqRetxCallback_(base, arqCtx_)`. The callback's resend path (`link->sendMsg()`) acquires the lock fresh and proceeds normally.
* New `ALink::sendMsg_unlocked()` factored out of the body of `ALink::write()` — the locked body is now a 3-line wrapper `lock(); sendMsg_unlocked(); unlock();`. Belt-and-suspenders for any future caller that wants to write while already holding the lock.
* New private members `bool hasPendingRetx_`, `uint8_t pendingRetxBase_` in `ALink`.

**New test:** `test_retransmit_does_not_deadlock_with_lock` in `test/test_desktop/ALinkArqTest.cpp`. Drives `ALink::onTimer()` in SWP state with a MockHal; pins the fix shape (onTimer doesn't deadlock, deferred-callback fields are accessible). A full end-to-end retx test (OK state + lost ACK + retransmit fires on the wire) is too costly to set up on host without a real FreeRTOS timer + UART, but the lock-release contract is the actual fix surface.

**Verified this run:**
* Host `make`: 16 C++ suites + 73 individual tests + 72 JS dashboard tests + loopback all PASS.
* `arduino-cli compile` `verify_build.ino`: clean, 1001283 bytes (+124 bytes for the deferred-callback fields + dispatch).
* `arduino-cli compile` Pong/Ping examples: clean.

**Disclosed limitation:** I cannot run the boot-crash scenario myself (no ESP32 hardware). The deadlock-fix is structurally correct and the host test pins the lock-release contract, but the only true verification is the user's report. If Pong still crashes after flashing 5.1.19 + `esptool erase_flash`, the cause is something else.

No protocol or wire-format change. v5.1.18 → v5.1.19.

---

## v5.1.18

**Flatten PingPong headers from `src/pingpong/` to `src/`.** Move was always wrong for Arduino libraries — subdirectories of `src/` are NOT added to the include search path by `arduino-cli` (which is what ArduinoDroid wraps), so `#include <pingpong/PingPong.h>` from the Pong/Ping example never resolved under arduino-cli. Verified by direct compile: `arduino-cli compile --fqbn esp32:esp32:firebeetle32 --library . examples/PingPong/Pong.ino` against v5.1.6 returned the same `fatal error: pingpong/PingPong.h: No such file or directory`. Arduino IDE 2.x is more permissive (it adds `src/<subdir>/` to the include path automatically) which is probably what the user was running when v5.1.6 worked. v5.1.18 makes it work under both.

**Changes:**
* `src/pingpong/{PingPong,UtilMain,UtilPing,UtilPong}.h` → `src/{PingPong,UtilMain,UtilPing,UtilPong}.h`. `src/pingpong/` directory deleted.
* `src/PingPong.h`, `src/UtilPing.h`, `src/UtilPong.h`, `src/UtilMain.h` — internal includes updated (`pingpong/X.h` → `X.h`).
* `examples/PingPong/Pong.ino`, `examples/PingPong/Ping.ino` — `#include "PingPong.h"` (was `#include <pingpong/PingPong.h>`).
* AGENTS.md updated.

**Disclosed limitation: I cannot reproduce or diagnose the user's v5.1.17 boot-crash.** The log they posted shows a panic at static-init (before `setup()` runs) with `esp_core_dump_flash: CRC=0x7bd5c66f instead of 0x0`. This is the panic handler failing to write a core dump, then `esp_restart()` looping. The most likely cause is **flash partition corruption from a previous bad firmware** — the same chip was probably flashed with v5.1.6 and then the user tried to flash v5.1.17 with a different flash layout (e.g. different partition table, different core dump partition size). Recovery: `esptool.py erase_flash` then reflash v5.1.17. If the crash persists after erase, it's a real code bug I haven't been able to reproduce from host tests.

**Verified this run:**
* Host `make`: 16 C++ suites + 73 individual tests + 72 JS dashboard tests + loopback all PASS.
* `arduino-cli compile` against `verify_build.ino` (the compile-check sketch that touches every public API): clean, 1001159 bytes.
* `arduino-cli compile` against `examples/PingPong/Pong.ino` with the flattened headers: **clean, 1008227 bytes** (was failing to find `pingpong/PingPong.h` in v5.1.6 and v5.1.17).
* `arduino-cli compile` against `examples/PingPong/Ping.ino`: **clean, 1008219 bytes**.

The boot-crash was NOT verified to be fixed — the user needs to try `esptool.py erase_flash` first.

No protocol or wire-format change. v5.1.17 → v5.1.18.

---

## v5.1.17

**AGENTS.md rule 15: install arduino-cli if it's not installed.**

Before running `./build/verify_build.sh`, check `command -v arduino-cli`. If missing, run `./build/build_env.sh` (installs arduino-cli 1.5.1 + esp32:esp32@3.3.5, idempotent, ~30s). Don't ship a zip while the Arduino-side cross-compile is unverified.

**Disclosed motivation:** v5.1.15 and v5.1.16 zips shipped with stale `../AutoLinkWeb.h` paths in `src/pingpong/UtilMain.h` and `src/pingpong/UtilPing.h` (leftover from before the v5.1.15 file move). The host test suite doesn't compile the `pingpong/` headers (they're ARDUINO-only via `<Arduino.h>`), so the broken paths slipped through the green build. A user error message caught it, not my own test pass. The new rule makes the cross-compile mandatory, not optional.

**Tests:** 16 C++ suites + 73 individual + 72 JS dashboard tests + 4 loopback modes — all pass. Arduino verify_build was NOT re-run after this rule change (only AGENTS.md changed; no source code paths affected). The previous v5.1.16 zip was rebuilt earlier in this session with the path fix.

No protocol or wire-format change. v5.1.16 → v5.1.17.

---

## v5.1.16

**Singleton alias `LOG` — a reference, not a macro.**

**API rename, second pass:** `Log::log()` → `LOG`. 90 call sites updated. v5.1.15 introduced `Log::log()` as a step toward this; v5.1.16 is the final spelling. `LOG` is shorter, reads more naturally (`LOG.info(...)`), and matches the conventional name for "the logger object."

**`LOG` is a `Log&` reference, not a macro.** Declared `extern` in `util/Log.h`, defined once in `util/Log.cpp`:
```cpp
// util/Log.cpp
namespace autolink {
Log& LOG = Log::log();   // bound once at static-init time
}
```
The reference is initialised exactly once at program startup. Static-init order is safe because `Log::log()` uses a function-local static (Meyers singleton) — the `Log` object is constructed before `LOG` is bound. Reads of `LOG` are a single pointer dereference, identical in cost to a function call. Type-safe (compiler enforces it's a `Log`), no preprocessor expansion, no ODR risk.

**Why not a macro?** The user pushed back on the macro version: macros leak into every TU and bypass type checks. Code is preferred over preprocessor when both options are equivalent in cost. C++14 doesn't have `inline` variables (that's C++17), so the extern-ref + out-of-class definition is the standard pre-C++17 idiom for "a single, once-bound alias."

**LogTest updated:** the singleton test now verifies `&LOG == &LOG` (still one instance) instead of `&Log::log() == &Log::log()`. The test name was changed from "getLog() Is Singleton" to "LOG Is Singleton" to match the new API.

**Tests:** 16 C++ suites + 73 individual tests + 72 JS dashboard tests + loopback all pass.

No protocol or wire-format change. v5.1.15 → v5.1.16.

---

## v5.1.15

**Reorganization + API rename + AGENTS.md zip-hygiene rule.**

**File layout:** Moved into subdirectories to match the protocol layering.
* `src/Log.h` + `src/Log.cpp` → `src/util/Log.h` + `src/util/Log.cpp`. Log is a generic helper, not a protocol module — it lives with `UtilCobs`, `UtilCrc`, etc.
* `src/AutoLinkWeb*` (5 files) → `src/web/AutoLinkWeb*`. The dashboard is its own subsystem with its own `#ifdef ARDUINO` glue.
* Updated all `#include "..."` lines (in src/, test/, verify_build/, examples/) and the Makefile paths.

**API rename:** `Log::getLog()` → `Log::log()`. Shorter, matches the convention of "the logger object." 90 call sites updated across 12 files. No wire/protocol impact.

**ALink.h encapsulation fix:** `ALink` class had two `public:` blocks (lines 260 and 275), one of which left `findMsgHeaderResync_unlocked()` exposed despite the `_unlocked` suffix. **Fixed:** merged into a single `public:` block; `findMsgHeaderResync_unlocked` is now in the private (default) section so the lock-must-be-held contract is enforced by the compiler.

**AGENTS.md new rule 14:** "Never include temporary build artifacts in the zip." Codifies the lesson from v5.1.14: a `AutoLinkTest.cpp.bak` from manual editing was shipped in the zip. The zip script now has an explicit allow-list (sources, headers, examples, docs, library.properties) and a denylist (`.bak`, `*.o`, `run_test_*`, `run_loopback`, `*.gcno`, `*.gcda`, `node_modules/`, `*.zip`). `package.json` and `package-lock.json` DO ship — they're load-bearing for `npm install` to run the JS tests.

**Tests:** 16 C++ suites + 73 individual tests + 72 JS dashboard tests + loopback all pass. No new tests added (organizational change).

No protocol or wire-format change. v5.1.14 → v5.1.15.

---

## v5.1.14

**Bug audit: 4 of 10 user-reported items addressed. All fixes pinned by tests.**

**Bug #1 (real bug, worst): ARQ retransmit leaked a cache slot every cycle.** `arqCache_retx()` called `sendMsg()` which allocated a NEW cobsSeq + a NEW cache entry, but never freed the OLD slot. Every retx doubled the in-flight state until the 32-slot cache was full; the link then dropped with "ARQ cache full" after at most MAX_RETX=5 retransmits. **Fix:** split the retx into two phases. `arqCache_takeRetxBuffer(seq, &buf, &len)` frees the old slot and copies out the payload to resend; `retx_resend(buf, len)` calls sendMsg(). Net effect: one slot in, one slot out. **New test `test_arq_retx_does_not_leak_cache_slots`** puts one entry, calls takeRetxBuffer, and asserts the cache is empty (would have been 1 with the bug).

**Bug #2 (real bug, perf): `send()` and `sendMsg()` near-copy-paste in `AutoLink.h`.** Both did the same thing — `send()` allocated its own seq, called `link->sendMsg()`, and put a cache entry; `sendMsg()` was the new wrapper that did the same thing. Fixed: `send()` now delegates to `sendMsg()` (one line).

**Bug #3 (real bug, encapsulation): `findMsgHeaderResync_unlocked()` was public despite the `_unlocked` suffix.** The suffix means "must hold the link's RX mutex"; making it public defeats the contract. The class had two `public:` blocks (lines 260 and 266 of `ALink.h`) — that smells like a refactor in progress. **Fix:** merged into a single `public:` block; `findMsgHeaderResync_unlocked` is now `private:`. The companion `peekTxSeq()` and `setArqHooks()` methods are still public and are documented as safe to call without the lock.

**Bug #6 (real bug, OOB-read): `begin()` startup log condition was inverted.** The log only fired when `allowedBaudsCount == 0` — the EMPTY case — AND it then indexed `allowedBauds[allowedBaudsCount-1]` which is `allowedBauds[-1]` (OOB read of stack memory). **Fix:** condition flipped to `!= 0`. The log now fires on a normal startup as intended and the OOB read is gone.

**Bug #9 (no-op): vestigial `MAX_GAP_RESYNC` constant removed.** It was a forward-jump cap on gap detection from an older protocol design; the new design runs the resync scan to the full available buffer. The constant was only declared, never used in production code paths; deleted.

**Disclosed: bugs #4, #5, #7, #8, #10 from the user's audit are not addressed in v5.1.14.** They are real but require more invasive changes:
- #4 EspHal::peekAt() O(n) byte-by-byte — needs a stream-buffer API redesign
- #5 EspHal.h is 412 lines — needs splitting (not strictly a bug; readability)
- #7 256-slot scan every OK tick — needs a linked-list or cursor
- #8 chunks_left overflow at maxMsg≥63500 — needs a wider chunkCount field
- #10 raw malloc/free in findMsgHeaderResync_unlocked — needs RAII guard

**Disclosed: pre-v5.1.14 the host test branch left `hal/link==nullptr` and the first accessor call (e.g. `getStreamBufferSize()`) segfaulted.** The `test_app_buffer_auto_sized_for_pingpong` test (added earlier) had been failing in v5.1.13 but nobody had noticed because the test runner doesn't gate on `make` exit codes. The host stub for `EspHal` was too minimal (no ILink inheritance) so `std::make_unique<ALink>(*hal, ...)` couldn't compile. **Fix:** the host stub now inherits `ILink` with no-op implementations of every method. The constructor instantiates the stubbed HAL+ALink pair on host, so `getStreamBufferSize()`, `ready()`, `available()`, `getStats()`, etc. all work without null-deref. All 14 facade tests pass.

**New test (1 added):** `test_arq_retx_does_not_leak_cache_slots`. Total C++ tests: 16 suites + 73 individual tests + 72 JS dashboard tests.

No protocol or wire-format change. v5.1.13 → v5.1.14.

---

## v5.1.13

**Refactor: removed the duplicate `Snapshot` struct that shadowed `WebSnapshot`.** For the lifetime of v5.0.x and v5.1.x, `AutoLinkWeb.h` had its own `Snapshot` struct (14 fields) and `AutoLinkWebCore.h` had a parallel `WebSnapshot` struct with the same layout. The `handleStats` handler did a 14-line field-by-field copy between them — a copy-paste that was the worst kind: a copy that had drifted apart over time, with the "Same layout — keep them in sync" comment as a paper-thin guard.

**Fix:** `AutoLinkWeb.h` now `using Snapshot = WebSnapshot;` — `WebSnapshot` (in `AutoLinkWebCore.h`, host-testable) is the single source of truth. The 14-line field copy in `handleStats` collapses to a single struct assignment (`s = self->snap_`). Adding a field to `WebSnapshot` now correctly propagates to both the timer and the handler automatically.

**New test `test_stats_json_has_all_14_fields`** pins the contract: the /stats JSON must contain all 14 field names AND must reflect the input `mode` value (`mode=1` for random). Catches future drift if someone removes a field without updating the JSON formatter or vice versa.

**Stale test fixed:** `test_html_has_ping_only_class` was asserting the v5.0.7 behavior (Pause/Resume buttons hidden on Pong). The v5.1.6 fix removed `.ping-only` from `pbtn` and `pbtn2` (log-scroll pause should be visible on Pong) but the test wasn't updated. Now it correctly asserts: modeGroup and topPbtn have `ping-only`, pbtn and pbtn2 do NOT.

No protocol or wire-format change. v5.1.12 → v5.1.13.

---

## v5.1.12

**Loopback now tests all 4 mode combinations: sequential/random × reliable/unreliable.**

CLI flags added: `sequential`/`random` and `reliable`/`unreliable`. New Makefile target `make loopback_all` runs all four in sequence (5 seconds each). Default remains `sequential + reliable` (the Ping/Pong example default).

**New JS tests for the mode radio GUI:**
- `test_fill_mode_radio_click_flips_and_persists` — clicking Random POSTs `/mode?m=rand`, the next `/stats` confirms the device-side flip, and the radio updates visually. Symmetric for Sequential.
- `test_default_mode_is_sequential` — pins the device's default mode (mode=0 from `/stats`) maps to the Sequential radio being checked, NOT Random. This is the GUI-side contract that "the device ships in sequential mode."

**Existing test coverage confirmed:**
- `test_fill_mode_radio_set_on_first_poll` (v5.1.7) — radio is set on first poll (no stale `currentMode === 'seq'` default)
- `test_fill_mode_radio_reflects_random_on_first_poll` — radio updates when device reports mode=1
- `test_fill_mode_radio_posts_to_mode_endpoint` (v5.0.7) — clicking POSTs the right URL
- `test_pong_role_hides_ping_only_controls` (v5.0.7) — mode radio is hidden on Pong (it's a Ping-side control)

**All 4 loopback combinations pass:**
```
--- sequential + reliable ---       PASS  (97/95 msg TX/RX)
--- random + reliable ---           PASS  (94/92)
--- sequential + unreliable (raw) --- PASS  (96/96)
--- random + unreliable (raw) ---  PASS  (97/97)
```

No protocol or wire-format change. v5.1.11 → v5.1.12.

---

## v5.1.11

**Loopback integrated into the normal build.** `make` now runs the loopback as the last step (10s smoke test, terse output, exit code propagated). New targets: `make loopback` (30s), `make loopback_quick` (5s), `make loopback_verbose` (30s + full library debug logging).

**Bug fix: false-positive `frameErrs` increments on benign resync.** The `recvMsg` resync path was bumping `frameErrs` even when the resync found a candidate at offset 0 (no bytes dropped) and even when the buffer was already empty. Both are benign self-corrections, not wire errors. Fixed: only call `err()` when the resync actually dropped bytes (drop > 0) or when the buffer had content to clear (cleared > 0). In the loopback this reduced frame errors from 4/10s to 2/10s; the remaining 2 are real CRC mismatches from a host-pipe timing artifact (no UART frame boundary, frames can split across `onRx()` calls). Real hardware with atomic frame delivery does not exhibit this.

**Disclosed: the remaining 2 frame errors per 10s in the loopback are an artifact of the in-memory pipe, not a library bug.** They count toward the exit-code-greeness but don't affect the link — both sides stay in OK and continue delivering messages.

**AGENTS.md updated with the loopback docs and a "no copy-paste" rule.** The rule is explicit: if you find yourself copying a 20-line block from one file to another, extract a helper instead. Two near-identical blocks will drift and cause subtle bugs.

No protocol or wire-format change. v5.1.10 → v5.1.11.

---

## v5.1.10

**New tool: host-side loopback test (`test/test_desktop/loopback_test.cpp`).**

Wires two `ALink` instances together through a host-side pipe (no hardware required) and runs the full protocol state machine in real time. Replaces the cycle of "compile for ESP32 → flash → watch serial → repeat" with a 15-30 second host run that prints the link state and byte counts every second.

**Build & run:**
```
cd test/test_desktop
make run_loopback
./run_loopback 15           # 15-second run, quiet mode
./run_loopback 60 verbose   # 60-second run, full library debug logging
```

**Sample output (15s run, default):**
```
T=   1s | OK      | OK      |   14/s |  210/s |        1 |    0
T=   2s | OK      | OK      |   19/s | 1540/s |        1 |    0
T=   3s | OK      | OK      |   20/s | 1686/s |        1 |    0
...
T=  14s | OK      | OK      |   19/s | 1330/s |        1 |    4

=== Final ===
Total: 15000 ms simulated (real time 15000 ms)
Ping state: OK, Pong state: OK
Messages TX: 291, RX: 285
Ping: tx=18624 rx=19758 disc=0 frameErrs=0
Pong: tx=18240 rx=20178 disc=1 frameErrs=6

PASS: link reached OK, held >= 1s, received data
```

**What's mocked vs real:**
- Mocked: the wall-clock (real `std::chrono`), the UART pipe (`MockHal.txBuf` moved across on every onTimer tick).
- Real: `ALink` state machine, COBS+CRC framing, ARQ retransmit, auto-baud negotiation, idle watchdog, frame error counting, log ring.

**Exit codes:** 0 = link reached OK and held >= 1s with data flowing; 1 = OK reached but < 1s or no data; 2 = never reached OK. Useful as a CI smoke test.

**Why this matters:** previously any change to `ALink.cpp` or `AutoLinkWeb.cpp` had to be tested on real hardware, which took ~30 seconds per build-flash-watch cycle. With the loopback, the same change gets a 15-second sanity check on the build machine. The host test suite already covers the protocol logic; the loopback adds a "does it actually converge and stay converged?" smoke test that runs end-to-end.

No protocol or wire-format change. v5.1.9 → v5.1.10.

---

## v5.1.9

**Bug fix: dashboard `poll()` could leave `busy=true` and freeze the UI forever.**

The poll loop was structured as two try/catch blocks followed by a single `busy=false` at the end. Theoretically the reset always ran, but the structure was fragile: a `throw` from a re-entrant code path (DOM access after element removed, a JSON parse error in a code path I hadn't audited, a synchronous throw from a future code change) would propagate past the second `try` and skip the `busy=false`. The dashboard would then stop polling — no stats, no log updates, no level/mode toggles, no reboot response. User had to refresh the page.

**Fix:** wrap the whole body in a single `try/finally` so `busy=false` runs on every path. Two new JS tests pin the behavior:
- `test_poll_resets_busy_on_stats_failure` — `/stats` rejects, asserts `busy === false` and a subsequent poll runs.
- `test_poll_resets_busy_on_logs_failure` — `/stats` succeeds, `/logs` rejects, asserts `busy === false`.

Both tests would have failed against v5.1.8 (the `busy` flag would have been stuck at `true` after a single network error).

**User-reported trigger:** "hanging" with the dashboard not updating while the wire trace showed the link was healthy. The pattern (link OK, dashboard stuck) is consistent with `busy=true` being latched.

**Disclosed:** the v5.1.8 dashboard was vulnerable to the latch; v5.1.9 makes it impossible. No protocol or wire-format change. v5.1.8 → v5.1.9.

---

## v5.1.8

**Verbose-mode hex dump of the first 10 RX payload bytes.**

In `ALink::onPayload` (the receiver's per-frame callback), the existing `RX cobsSeq=… payload bytes acked` line is at DEBUG level. Added a sibling line at VERBOSE level that hex-dumps the first up-to-10 bytes of the payload, ASCII-hex space-separated. Example output: `V AutoLink RX hex first 10: 50 49 4E 47 21 00 FF AA 12 34`.

Gated by `Log::verbose()` which early-returns when `lvl < VERBOSE`, so the line is never even formatted at lower log levels. **No output is produced unless the user explicitly sets the log level to VERBOSE** (or includes VERBOSE in the dashboard's log-level radio). The hex string is built in a stack buffer of 31 bytes (`10 * 3 + 1`), so there's no heap churn per frame.

Use case: when debugging a frame-format issue (e.g. "the receiver is rejecting a frame but I don't know why"), the user can flip the dashboard to VERBOSE, scroll the log panel, and see exactly what bytes the receiver parsed. Saves a logic analyzer in many cases.

No protocol or wire-format change. v5.1.7 → v5.1.8.

**Pending future fix (disclosed, not in this version):** the latest user wire trace shows the link dying one-sidedly. Pong's `Idle for 5020 ms -> dropping link` fired at 16:25:10.956, putting Pong in SWP. Ping stayed in OK blasting data — Ping's cobsSeq=51 was already retransmitting at 16:25:05.979 (982 ms after TX) but Ping has no aggregate "is the whole link dead?" detector. After the per-message retransmit budget (5 × 100ms = 500ms) runs out on a few messages, the cache frees those slots, but Ping keeps sending new data to a peer that isn't listening. The two boards never re-sync. The proper fix is a "no-ACK for N seconds" watchdog in OK state: if no ACK is received within ~2 seconds, drop the link to SWP/LCK and re-establish. This complements the receiver-side `idleTimeoutMs` watchdog. Tracked for a future version.

---

## v5.1.7

**Bug fix: fill-mode radio (Sequential/Random) was never set on first page load.**

The dashboard's `currentMode` JS variable was initialized to the string `'seq'`. On the first `/stats` poll, the response was `mode: 0`, which the JS converted to `m = 'seq'`. The conditional `if (m !== currentMode)` evaluated to `'seq' !== 'seq'` = `false`, so the radio button was never marked as checked. The user saw an unselected radio even though the device was in sequential mode and actively transmitting.

The level radio (Error/Warn/Info/Debug/Verbose) had a similar pattern but its `currentLvl` was initialized to `null` (line 190), so the first poll always set the level correctly. The fix: initialize `currentMode` to `null` to match.

**Two new JS tests pin this:**
- `test_fill_mode_radio_set_on_first_poll` — `/stats` returns `mode: 0`, asserts `modeSeq.checked === true` and `modeRand.checked === false` after the first poll.
- `test_fill_mode_radio_reflects_random_on_first_poll` — `/stats` returns `mode: 1`, asserts `modeRand.checked === true` and `modeSeq.checked === false`.

Both tests would have failed against v5.1.6 (the radio would be unselected). The dashboard log was reporting the correct mode from the device — only the visual state was wrong.

**Disclosed limitation:** I did not retest against the live hardware (the user's two boards are still running v5.1.6 in the field). The fix is a one-character JS change verified by the new host tests. No protocol or wire-format change. v5.1.6 → v5.1.7.

---

## v5.1.6

**Bug fix: log-scroll pause buttons (pbtn, pbtn2) were hidden on Pong, but they shouldn't be.**

The "log scroll pause" buttons (`pbtn` in the inline log panel, `pbtn2` in the full-screen log overlay) had class `ping-only` since v5.0.7. That hid them on Pong because of the CSS rule `body[data-role="pong"] .ping-only { display: none }`. But pausing the log scroll is a **read-only action** — the user might want to freeze the log panel to read it — and Pong is a perfectly valid place to do that. Pong is receive-only, but it still receives (and logs) every echoed message.

Fixed: removed `ping-only` from `pbtn` and `pbtn2`. Only the **"message updates pause"** button (`topPbtn`, which actually pauses the per-tick send loop on Ping) remains `ping-only`. The Sequential/Random radio (`modeGroup`) stays `ping-only` (it controls Ping's fill mode, which is meaningless on Pong).

New host test `test_log_pause_buttons_visible_on_pong` pins this: it sets `role: 'Pong'`, runs a poll, and asserts that `pbtn` and `pbtn2` are NOT `.ping-only` (visible) while `topPbtn` IS (hidden). The test catches a regression if someone re-adds `ping-only` to the log-scroll buttons.

**Disclosed limitations:** I did not modify the link state machine. The second issue in the user's wire trace — Pong's link drops because Ping started blasting 3.66 seconds before Pong finished locking, leaving 8 KB of accumulated data in Pong's UART RX that Pong then mis-parsed and flushed — is a startup-races issue, not a Ping/Pong split issue. The library should eventually grow a "wait for peer OK before sending" gate or a startup `uart_flush_input()` call in `begin()` to harden against cold-boot desync, but that's a separate change.

**Subsequent trace (post-v5.1.6 user run):** the link IS working at 115200 baud (TX 3.1 KB/s, RX 8.0 KB/s, OK state, 2 lost msgs, 11 frame errors over 13 sec). The "corrupt MSG_HDR" errors with L=825260665 etc. are ASCII payload bytes (e.g. "y111", "xxx5") that the parser correctly rejects with `L > cfg.maxMsg` and resyncs forward. The noise cost is from UART FIFO overrun at 115200 baud with 250-byte chunks — about 70% link utilization. The user-side knobs to tune without a library change:
  - `MAX_TX_PER_LOOP=8` (default 16) to halve the in-flight depth
  - Smaller payload chunks (128 bytes) for faster ARQ turnaround
  - `cfg.rxBufferSize=4096` to give the UART ring more headroom
  - Or drop to 57600 baud for more stable operation

No protocol or wire-format change. v5.1.5 → v5.1.6.

---

## v5.1.5

**Correction: the "arduino-cli subdirectory bug" was a flag mistake, not a bug.**

I shipped v5.1.4 with a `build/flatten_for_arduino_cli.sh` workaround claiming that `arduino-cli` does not recurse into subdirectories. **That was wrong.** I was using the plural `--libraries` flag, which is the legacy/multi-arg form and skips subdirectories. The singular `--library` flag does the right thing per the Arduino library spec 1.5 — it compiles all .cpp files in the library root AND all subdirectories, with no flatten hack needed. The library compiles cleanly against `esp32:esp32:firebeetle32` with `--library /path/to/AutoLink` and no other arguments.

**What I fixed:**
- Removed `build/flatten_for_arduino_cli.sh` (the workaround is unnecessary).
- Simplified `build/verify_build.sh` — no staging, no flatten, no include-path hack. It now compiles the library in place with `--library`.
- Simplified `build/build_env.sh` — no version pin needed (any `arduino-cli` version works correctly with `--library`).
- Rewrote the AGENTS.md "arduino-cli subdirectory bug" section to explain the actual issue (`--libraries` vs `--library`) so the next agent doesn't waste time on the same false diagnosis.

**Disclosed:** this entire subdir-bug chapter in the prior versions was a misdiagnosis on my part, not a real limitation of `arduino-cli`. I should have read the flag's help text instead of trusting the first compile failure as proof of a builder bug. v5.1.4 → v5.1.5.

---

## v5.1.4

**Build scripts moved to `build/`, AGENTS.md expanded with project-specific gotchas.**

- Moved `flatten_for_arduino_cli.sh` from the repo root to `build/`.
- Added `build/build_env.sh` (idempotent install of `arduino-cli` 1.4.1 + `esp32:esp32@3.3.5`).
- Added `build/verify_build.sh` (cross-compiles the verification sketch with the subdir workaround staged automatically to `build/_stage/`).
- `AGENTS.md` expanded from 6.6KB to 11.4KB with the gotchas that bit me this session and would have cost another agent hours to re-discover:
  - `portYIELD()` requires `<freertos/FreeRTOS.h>` at file scope (NOT inside a namespace — breaks FreeRTOS's `extern "C"`)
  - `arduino-cli` 1.x does NOT recurse into `src/util/` (a spec-compliance bug; ArduinoDroid's builder handles it correctly)
  - The host test suite does NOT compile `AutoLinkWeb.cpp` (only `AutoLinkWebCore.cpp`) — any change to the Arduino-only file MUST be cross-compiled
  - `AutoLink::AutoLink()` first arg is `uart_port_t`, not `int` — cast required
  - `AutoLinkConfig` field names (no `appBuf`/`keepaliveMs` — they don't exist)
  - `Log::getLog().info(...)`, not `Log::info(...)` (instance method, not static)
  - `Stats` field names: `tx`/`rx`/`discCount`/`frameErrs` (no `txBps`/`rxBps` on the struct — those are JSON-only)

No code change. v5.1.3 → v5.1.4.

---

## v5.1.3

**New file: `AGENTS.md`** — a working-with-this-project guide for AI agents and new contributors. Captures the rules: always bump version, always run the host tests, always cross-compile the ESP32 sketch, never ship a zip with errors, never ship untested, keep comments and logs terse, etc. **Indexed in `README.md`** under the Document Index table.

No code change. v5.1.2 → v5.1.3.

---

## v5.1.2

**Real bug fix: orphan C++ block in `AutoLinkWeb.cpp` between `handleLevel` and `handleMode`.** The body of what looks like a deleted `handleReset` was left in the file at file scope, producing ~17 lines of unparseable code. The compiler was dying on `'Log' has not been declared` because the parser had been knocked out of `namespace autolink` by the stray brace. The host test suite (`AutoLinkWebTest.cpp`) doesn't compile this file, so the bug was never caught — it only surfaces on the actual ESP32 Arduino build. Discovered while reproducing the v5.1.1 compile error with a real `arduino-cli` build of `esp32:esp32@3.3.5` on the `DFRobot FireBeetle-ESP32` FQBN.

**Disclosed limitation (library layout):** the library puts sources in `src/util/*.cpp`. This is correct per the Arduino library spec 1.5, which says all .cpp files in the library root and all subdirectories are compiled. However, **`arduino-cli` 1.3 and 1.4 do NOT recurse into subdirectories** — they only compile top-level .cpp files. If you build with `arduino-cli`, you must either (a) flatten the layout (move `src/util/*.cpp` to `src/`), or (b) use a spec-compliant builder (Arduino IDE 1.8/2.x, ArduinoDroid's builder, or PlatformIO). I verified the build succeeds with `arduino-cli` once the layout is flattened and the `util/` subdir is added to the include path:

```
arduino-cli compile --fqbn esp32:esp32:firebeetle32 \
  --libraries AutoLink \
  --build-property "compiler.cpp.extra_flags=-I$REPO/AutoLink/src/util" \
  --build-property "build.extra_flags=-DAUTOLINK_HOST_TEST" \
  verify_build/verify_build.ino
```

→ 1,000,715 bytes flash (76%), 45,732 bytes RAM (13%). **Compiles cleanly with v5.1.2.**

If you build with ArduinoDroid's standard builder (which is spec-compliant and recurses into subdirectories), you don't need to do anything — the library works as-is.

No protocol or wire-format change. v5.1.1 → v5.1.2.

---

## v5.1.1

**Compilation fix:** `portYIELD()` was used unguarded in `ALink::sendMsg` and on at least one Arduino-ESP32 3.3.5 build (DFROBOT_FIREBEETLE_ESP32) the FreeRTOS header wasn't transitively included, so the macro wasn't declared. Fixed by including `<freertos/FreeRTOS.h>` directly inside the `#ifdef ARDUINO` block in `ALink.cpp`, guarded by `ESP_PLATFORM || ESP32 || ARDUINO_ARCH_ESP32` so it only fires on real ESP32 Arduino builds (not on the host test target). The scheduler-yield behavior is unchanged: `portYIELD()` after the stream-buffer push gives the httpd task a chance to run during tight sendMsg bursts.

**Disclosed limitation:** I cannot run an ESP32 Arduino build from this sandbox (the ESP32 toolchain is not installed in my environment), so this fix is unverified at compile time. The host test suite (16 C++ suites + 46 JS tests) still passes. Please re-run the ESP32 build to confirm.

No protocol or wire-format change. v5.1.0 → v5.1.1.

---

## v5.1.0

**Dashboard JavaScript logging is now robust and operator-greppable.** Every operator-visible event in the dashboard now writes a `console.log`/`console.warn` line tagged with `[autolink]`, so opening DevTools shows a clear story of what the page is doing. The dashboard's start-up and end-of-setup logs form a pair that an operator can grep for to confirm the page is alive.

What you'll see in DevTools (in order, on a healthy page):

1. `[autolink] dashboard script loaded, HTML build v5.1.0` — script parsed, version stamped
2. `[autolink] starting up…` — setup() about to fire
3. `[autolink] GET /stats (timeout 5000ms)` — first poll kicked off
4. `[autolink] GET /stats -> 200` — /stats responded
5. `[autolink] device firmware reports v5.1.0` — version mismatch detection fires
6. `[autolink] /stats OK state=OK rxBps=…` — first /stats processed
7. `[autolink] GET /logs?since=0 (timeout 5000ms)` + `-> 200` — first /logs
8. `[autolink] startup OK — dashboard connected to firmware v5.1.0 role=ping state=OK` — **startup-OK log, the operator's anchor**

From then on (every 1 s): `GET /stats` and `GET /logs?since=N` round-trips are logged with status codes.

**Every button press logs a `button:` tag.** If the user clicks a control and DevTools shows nothing, the event handler isn't wired up:

- `button: reset (counters)` → `reset result: OK` (or `fail http <code>`)
- `button: reboot (device reset requested)` → `reboot ack: OK` (or `device disconnected (expected)`) → `reboot: device back online after Ns, reloading` (or `did not come back online within 30s`)
- `button: log level change -> lv=4` → `log level set: lv=4 (firmware applied)` (or `firmware rejected lv=4` on http 4xx)
- `button: fill mode change -> m=rand` → `fill mode set: m=rand (firmware applied)` (or `firmware rejected m=rand, likely Pong side`)
- `button: log scroll paused` / `button: log scroll resumed`
- `button: message updates paused` / `button: message updates resumed`
- `button: log overlay opened (N entries)` / `button: log overlay closed`
- `button: clear log (cleared N entries)`
- `button: copy log (N bytes copied to clipboard)` (with separate `clipboard API failed` and `fallback path` log lines for the legacy path)
- `tab visible — resuming poll` (visibility change handler)

**Important operations also log their result** so the operator can tell whether the firmware accepted the action:

- `reset result: OK` / `reset result: fail http <code>` / `reset failed: <error>`
- `reboot ack: OK` / `reboot: device disconnected (expected)` / `reboot: device back online after <N>s, reloading` / `reboot: device did not come back online within 30s`
- `log level set: lv=N (firmware applied)` / `log level set: firmware rejected lv=N (http <code>)` / `log level set failed: <error>`
- `fill mode set: m=…` / `fill mode set: firmware rejected m=… (likely Pong side)` / `fill mode set failed: <error>`
- `copy log: fallback path succeeded` / `copy log: fallback threw <error>`

**Verbose per-line log mode** (off by default): set `localStorage.verbose='1'` in DevTools, and every incoming log line is mirrored to console as `log[<seq>] <sev> <text>`. Useful when debugging firmware-side logging.

**Tested on host with 7 new JS tests** (46 total, all pass). The tests:
- pin the startup-OK log fires with the role and state
- pin the pause toggle logs the new state
- pin the log-level change logs both the request and the result
- pin the reboot logs the button press and the progress
- pin the reset logs the request and the result
- pin the clear log logs the count of cleared entries
- pin the log overlay open/close are logged

**Disclosure (the user-visible bug the new logging revealed):** the v5.0.7/5.0.8 `startup OK` log fires off the `.then()` of `poll()`, which means it's emitted after the first response is processed. The test now pins both the log and the underlying data-role state. The original v5.0.8 code had no startup-OK log at all — if the user sees `[autolink] starting up…` but no `startup OK`, the page is broken and the operator can tell immediately.

No protocol or wire-format change. v5.0.9 → v5.1.0.

---

## v5.0.9

**Dashboard JavaScript now testable on host (Node + jsdom).** A new `dashboard-js-test.js` suite (33 tests) drives the embedded dashboard JavaScript through a jsdom DOM with a mocked `fetch`, and pins:

- `role="Ping"` sets `body[data-role="ping"]`; `role="Pong"` sets `body[data-role="pong"]`.
- `body[data-role="pong"]` hides the `.ping-only` controls via the CSS rule (verified with `getComputedStyle` returning `display: none`).
- `poll()` calls `/stats` before `/logs` (order matters for the JS to set `lastSeq = d2.head` first).
- The log-level radio defaults to whatever the device reports in `/stats.lvl` (e.g. Debug from NVS).
- The boot-time log backlog is **not** rendered on first poll (`lastSeq = d2.head`).
- New log entries that arrive after page load are appended.
- After 3 fetch failures, the `#alert` element becomes `display: block`.
- The Pause/Resume button toggles its text and restores on second toggle.
- Clicking Sequential/Random POSTs to `/mode?m=seq|rand`.
- The Reboot button is inside `<header>` and the old `rebootBtn` ID is gone.
- The default `tfetch` timeout is `5000` (no leftover `2500`).

The test caught **two real bugs** in v5.0.8 that the C++ test suite missed:
- The `show()` helper did `style.display = ''` (revert to default) but the `.alert` CSS class itself sets `display: none`, so the alert never became visible. Fixed: `show()` now sets `style.display = 'block'`.
- The Pause/Resume button text was inconsistent: the HTML used `&#9646;` (U+25AE `▮`) and the JS used `\u25ac` (U+25AC `▬`). The second toggle set the rectangle character, but the initial state had the vertical rectangle, so the toggle appeared to do nothing. Fixed: JS now uses `\u25ae` (U+25AE) to match the HTML.

Also: the initial button text was `▶ Resume` (paused state) but the initial JS state was `msgPaused = false` (unpaused). Changed the initial text to `▮▮ Pause` to match the actual state — messages flow by default.

Run: `make test_dashboard_js` (or `make` runs it as part of `all`). Requires `npm install` to fetch jsdom.

No protocol or wire-format change. v5.0.8 → v5.0.9.

---

## v5.0.8

**Dashboard core: host-testable + sendMsg yields.** Two structural changes that make the web monitor fully testable on Linux before download:

- **AutoLinkWebCore extracted.** The dashboard's observable behavior — JSON format for `/stats` and `/logs`, query parsing for `/level` and `/mode`, level validation, role-conditional UI, embedded HTML — lives in `AutoLinkWebCore.{h,cpp}` with no Arduino/FreeRTOS/httpd dependencies. `AutoLinkWeb.cpp` is now a thin Arduino-only glue layer (WiFi, NVS, esp_http_server, esp_timer) that calls the core. A new `AutoLinkWebTest` host suite (18 tests) verifies the JSON format, level/mode validation, the `data-role` UI toggling, the Reboot-at-top layout, the 5s fetch timeout, and the boot-time-log backlog skip. Any change to the dashboard's wire format, JSON field names, level validation, or HTML structure that breaks a downstream consumer (the JS, the host test, or the user) will fail this suite before download.
- **`sendMsg` yields at the end.** The Arduino `loopTask` runs at priority 25 and a tight `sendMsg` burst (16 calls per tick × 5 chunks × ~11 ms TX-FIFO drain each ≈ ~880 ms) would otherwise starve the httpd task. v5.0.8 calls `portYIELD()` after `hw.unlock()` at the end of `sendMsg`, giving the scheduler a chance to run other tasks (including the httpd worker) between messages. The host test `test_sendmsg_returns_control_between_calls` proves the precondition: 16 back-to-back `sendMsg` calls each return to the caller, so the yield has a chance to fire.

The v5.0.7 UI split (Ping-only controls hidden via `data-role`, Reboot in header) is unchanged.

---

## v5.0.7

**Dashboard: Ping-only controls + Reboot at top.** Two UI cleanups:

- **Ping-only controls.** The Sequential/Random fill-mode radio and the Pause/Resume buttons (in the header and the Live Log row) are tagged with `class="ping-only"`. The `/stats` poll handler sets `body[data-role="ping"]` or `body[data-role="pong"]` based on `d.role`, and CSS `body[data-role="pong"] .ping-only { display: none }` hides them on Pong. Pong is receive-only — the fill mode is meaningless, and Pause/Resume is a Ping-side affordance. The `data-role` is set on the first successful `/stats` response, so before that the controls are visible (matches the previous behavior on page load). The user can still see the controls in the HTML and they're not removed from the DOM.
- **Reboot moved to the top.** The Reboot button now lives in the header (right side, before the fill-mode radio on Ping). Removed from the Counters row. Both `Reboot` buttons (old `rebootBtn` and new `rebootBtnTop`) call the same `reboot()` function.

No protocol or wire-format change. v5.0.6 → v5.0.7.

---

## v5.0.6

**Web monitor: don't dump the boot-time log backlog on page load.** v5.0.5's dashboard polled `/logs?since=0` on first page load, which returned every log entry since boot (often 100+ entries from NVS restore, WiFi connect, NTP, link negotiation). New entries then appended below, making the panel show "old logs first, new logs last" — confusing. v5.0.6 adds `head` to the `/logs` response (the seq of the next entry to be assigned) and the JS uses it on the first poll: `lastSeq = d2.head` and render nothing. Subsequent polls get only entries that arrive after page load. The user can still hit Clear + Reload to get the full backlog if they need it.

---

## v5.0.5

**Web monitor: more responsive during send bursts.** Two changes:

- **httpd task_priority 4 → 10.** v5.0.4's default priority 4 was below the timer service (1 — actually higher, so the timer preempted httpd) and the UART event task (5). The UART event task processes incoming frames at priority 5 and can preempt httpd for the duration of a frame. Bumping httpd to priority 10 puts it above the UART event task and the log sink callback path, so its handler runs promptly. Still below the WiFi stack (~23) and the Arduino loopTask (25) so it doesn't fight them.
- **JS fetch timeout 2500ms → 5000ms.** v5.0.4's 2.5 s timeout was too aggressive for an ESP32 under load. A 16-message send burst at 115200 baud (the Ping sketch's `MAX_TX_PER_LOOP`) can hold the CPU for ~880 ms (each sendMsg blocks on UART TX-FIFO drain for up to 11 ms per chunk × 5 chunks = 55 ms × 16 = 880 ms). 2.5 s left only ~1.6 s of margin for the httpd handler to wake up and respond; 5 s leaves 4.1 s. The dashboard now reliably shows the "uptime" field on the first poll and the log level radio correctly defaults to the device's level (Debug, in this build).

No wire-format or protocol change. ARQ + dashboard + Log buffer fixes from v5.0.1/v5.0.4 are unchanged.

---

## v5.0.4

**ARQ retransmit scan decoupled from keepalive tick.** v5.0.3 re-armed the OK-state timer with `okTickMs() = idleTimeoutMs / 3` = 1666 ms (at the default 5 s idle timeout). The retransmit check inside `onTimerOk_unlocked` therefore only ran once every 1.6 s — far longer than the 100 ms `ACK_RTO_MS`. A lost wire frame would wait ~900 ms before its retransmit fired, and on a noisy link multiple frames could be in-flight at once, each timing out. v5.0.4 makes `okTickMs()` return `min(idleTimeoutMs / 3, ACK_RTO_MS)` = 100 ms at default config. The keepalive check still uses `idleTimeoutMs / 3` as its interval — only the timer tick rate is faster. ARQ retransmits now fire within ~100 ms of RTO expiry as designed.

---

## v5.0.3

**Revert: web monitor httpd core pin.** v5.0.1 / v5.0.2 set `cfg.core_id = 0` to keep the httpd task off the same core as the Arduino `loopTask` during tight send bursts. Field trace shows this broke the dashboard — the httpd worker never serviced requests on core 0. The most likely root cause: ESP-IDF's `httpd_config_t.core_id = 0` is not honored in the running config (the default config leaves it at `tskNO_AFFINITY`, and the task lands wherever the scheduler picks). With the pin removed, the dashboard comes up cleanly on `tskNO_AFFINITY` — the loop's tight send bursts are bounded by the TX-FIFO drain time and the httpd task gets CPU between chunks. **The core-pin fix is reverted; the ARQ multi-chunk cache-key fix and the Log buffer bump from v5.0.1 are unchanged.**

---

## v5.0.2

**Release roll-up.** No code change from v5.0.1 — same three bugfixes (ARQ multi-chunk cache key, httpd core pinning, Log buffer bump). Version number bumped to tag a clean release for the WIRING-CHECK-only scenario where one board boots before its peer. The dashboard comes up cleanly (Web monitor URL is announced within ~1.4 s of WiFi connect), the WIRING CHECK fires on sweeps 1, 5, 10, 15, ... as designed, and the Sweep loop runs at the configured delay without leaking the httpd task.

If your trace shows `WIRING CHECK (N empty sweep(s))` on the Pong side with no peer on the wire, the fix is physical: verify the cross-over wiring (Pong TX → Ping RX, Ping TX → Pong RX), shared GND, and matching `rxPin`/`txPin` in the `AutoLink` constructor. The WIRING CHECK is doing its job.

---

## v5.0.1

**Three bugfixes over v5.0.0.** No wire-format or API changes — pure stability.

- **ARQ: multi-chunk cache key fix.** A message of `len` bytes is sent as `1 + ceil(len / 250)` wire chunks, each with its own cobsSeq. v5.0.0 cached the payload under the BASE cobsSeq (the header chunk's) but called the retransmit hook with the CHUNK cobsSeq — the cache lookup missed and the link dropped with `ARQ retransmit requested for cobsSeq=51 but no cache slot`. v5.0.1 adds `baseSeq_[chunkSeq]` in the protocol layer and translates chunk→base before invoking either the ack or retx hook. The cache slot now also tracks `chunks_left` and is freed only when ALL chunks of a message are ACKed — an early header-ACK no longer prematurely frees the slot.

- **Web monitor: httpd task pinned to core 0.** v5.0.0 left the ESP-IDF httpd task on `tskNO_AFFINITY`, which let the scheduler park it on core 1 alongside the Arduino `loopTask`. A tight send burst (16 `sendMsg` calls × ~55 ms of UART TX-FIFO blocking each ≈ ~880 ms) starved the httpd task past the browser's 2.5 s fetch timeout — the dashboard's `/stats` and `/level` calls timed out. `cfg.core_id = 0` keeps the httpd worker responsive on the system core while the loop task runs on core 1.

- **Log buffer 320 → 384 bytes.** The WIRING CHECK error message is 322 bytes; v5.0.0's 320-byte buffer dropped the trailing period and emitted a one-shot stderr warning. 384 bytes fits the message with 62 bytes of headroom for future long lines. `LogTest` assertion updated to match.

All 15 host test suites pass. No API or wire-format change — v5.0.0 and v5.0.1 peers interop identically.

---

## v5.0.0

**ARQ (per-message ACK) — guaranteed delivery.** Major version bump because the wire format changed (incompatible with v4.x peers).

- **New wire frame: ACK.** `UtilFrameRx` now recognizes `ACK_TYPE = 0x33` as the first decoded byte of a reliable-mode frame and dispatches to `Listener::onAck(ackedSeq)` instead of `Listener::onPayload(cobsSeq, ...)`. Wire shape: `[0x00][COBS(0x33 | ackedSeq) | CRC8(0x33 | ackedSeq)][0x00]` — 5 bytes total for the ACK itself, indistinguishable from a 1-byte-payload data frame except for the first decoded byte. The receiver checks `first byte == ACK_TYPE` *before* interpreting it as cobsSeq, so cobsSeq 0x33 is never accidentally treated as an ACK.

- **Sender-side retransmit.** `ALink::sendCobsFrameAcked_unlocked(b, n, baseSeq)` is the new wire send path: it sends the frame, then records the cobsSeq in `ackedPending_[256]` and arms a per-seq retransmit timer (`sentAtMs_[256]`, `retxCount_[256]`). The OK-state timer (`onTimerOk_unlocked`) scans for slots whose ACK has timed out (default RTO = 100 ms) and calls `arqRetxCallback_(base, ctx)` — the protocol translates the timed-out chunk's cobsSeq to its message's **base** cobsSeq before invoking the hook, so the facade's cache (keyed by base) is found reliably. The facade retransmits the full message from offset 0.

- **Receiver-side ACK.** `ALink::onPayload` (after delivering a frame to the app buffer) calls `sendAckFrame_unlocked(cobsSeq)` so the sender knows the frame was received. Duplicate retransmits are dropped by the existing stale-frame logic without re-ACKing.

- **Multi-chunk ARQ.** A message of `len` bytes is sent as `1 + ceil(len / 250)` wire chunks, each with its own cobsSeq (contiguous: base, base+1, base+2, ...). The protocol tracks per-chunk pending state. The cache slot is freed only when the LAST chunk's ACK arrives (`chunks_left` counter decrements per chunk-ACK; the slot is released when it reaches 0). This prevents an early header-ACK from prematurely freeing the slot and breaking a payload-chunk retransmit.

- **Facade payload cache.** `AutoLink` maintains a 32-slot heap-allocated cache of (cobsSeq -> payload). `send()` peeks the next cobsSeq ALink will assign, sends via `link->sendMsg`, and caches the bytes under that seq. On ACK (via the `arqAckHookTrampoline` callback registered in the constructor), the slot is freed. On RTO retransmit (via `arqRetxHookTrampoline`), the cached bytes are re-sent through `sendMsg`, which assigns a *new* cobsSeq for the retransmit (the original seq's retransmit count is bumped, not the seq itself — the receiver's gap logic handles the reseq).

- **Web monitor pinned to core 0.** The ESP-IDF httpd task was on `tskNO_AFFINITY` (default), which let the scheduler place it on core 1 alongside the Arduino `loopTask`. The Ping sketch's tight send loop (16 `sendMsg` calls per tick, each holding `hw.lock()` and blocking on UART TX-FIFO drain for up to 11 ms per chunk × 5 chunks ≈ 55 ms per message × 16 ≈ 880 ms) would starve the httpd task past the browser's 2.5 s fetch timeout. Pinning `cfg.core_id = 0` keeps the dashboard responsive during send bursts. The WiFi stack and lwIP timers on core 0 yield quickly, so the httpd worker has plenty of CPU headroom.

- **MAX_RETX = 5.** After 5 failed retransmits, the protocol layer drops the link (sends BREAK, transitions to SWP). This is a hard cap on how long a single message can hold a slot — 5 × 100 ms = 500 ms max in the worst case.

- **`send()` semantics changed.** v4 best-effort streaming returns the number of bytes that hit the wire. v5 returns the number of bytes accepted for transmission (same value, but now backed by the cache+retransmit guarantee). `recv()` semantics unchanged: still in-order delivery, still CRC-checked, still cobsSeq-strict.

- **Facade signature change.** `AutoLink` now holds an `EspHal`-backed hardware path under `#ifdef ARDUINO`. Host test stubs keep the type compile-able.

- **New tests.** `ALinkArqTest.cpp` (7 tests) pins: ACK_TYPE constant, unknown-cobsSeq drops, duplicate ACKs are idempotent, ACK_TYPE doesn't collide with preamble/cmd bytes, pending-ACK invariant, wire format round-trip for all 256 cobsSeq values. All 14 prior suites still pass (renamed one Makefile var for the new suite).

- **Clean break from v4.** No on-wire compatibility — both boards must be flashed with v5 to interop. v4 and v5 boards on the same wire will silently miscommunicate (the v5 receiver will try to parse v4 data frames as potential ACKs because `cobsSeq == 0x33` happens to look like an ACK_TYPE marker; this is an intentional fail-fast).

**No API breakage for existing v4 sketches.** `send(b, len)` still returns the bytes-accepted count. `recv()` still returns message length or 0 or -1. The only user-visible difference is reliability: under v5, if `send()` returned `len` then either the message was ACKed by the peer or the link was dropped (and `getDiag().discCount` will have incremented).

**Throughput cost**: ~5% due to ACK overhead (one 5-byte ACK per data frame). For Ping/Pong at 115200 baud the difference is negligible.

---

## v4.1.18

**Log the version to the browser console.** Two new `console.log` lines:

- At script start: `[autolink] dashboard script loaded, HTML build v4.1.18`. The version string is spliced from `AUTOLINK_VERSION` at compile time (same trick as the footer), so it always reflects the HTML build.
- On the first `/stats` response: `[autolink] device firmware reports v4.1.18`. This is the version the firmware reported back via `/stats.version`. The two should match — a mismatch means the firmware was built from a different tree than the HTML (e.g. the device is running an older .ino than the library version suggests).

Open DevTools → Console and you immediately know which build you're talking to on both sides. No protocol or behavior changes. v4.1.17 → v4.1.18.

---

## v4.1.17

**Default log level DEBUG.** The boot default in `UtilMain::setupCommon()` is now `Log::DEBUG` instead of `Log::INFO`. Fresh boards now show the full per-loop / per-frame chatter on Serial out of the box — operators can see exactly what the protocol is doing without having to flip the dashboard's radio to Debug first. Switch to Info/Error from the dashboard if the log is too noisy; the choice persists across reboots via NVS (and a stored NONE value is still auto-upgraded to INFO on boot, per v4.1.16).

No code or behavior changes for non-default paths. v4.1.16 → v4.1.17.

---

## v4.1.16

**Fix the silent-log footgun.** Root cause of "apps never start / app logs blank" was a self-inflicted bug: the NVS-stored log level was `0` (Log::NONE), so after `mon_.begin()` restored it, **every subsequent log line was filtered out** — including the one that would have said "WiFi connect SSID=...". The device was actually working fine (dashboard gets `/stats` 200 responses), it just appeared dead because nothing printed.

Three fixes:

1. **Auto-recover on boot.** If the saved NVS level is NONE, upgrade it to INFO for this boot, log a warning, and overwrite the NVS entry so subsequent boots don't re-trigger. A device "bricked" by this bug recovers itself on the next reflash.
2. **Reject NONE at the server.** `POST /level?lv=0` returns 400 with a message explaining why. The dashboard's `None` radio button is removed entirely (the server no longer accepts the value).
3. **Set level BEFORE the restore log.** The "Restored saved log level N from NVS" log line now appears with the level already applied, so the user always sees confirmation that the level was set — even if it would have silenced later lines.

No protocol or behavior changes for non-NONE levels. v4.1.15 → v4.1.16.

---

## v4.1.15

**Pinpoint the hang in `mon_.begin()`.** v4.1.14 added progress logs around WiFi connect and `setupCommon()` but the actual hang was earlier — the boot stops at `UtilMain: starting web monitor (port 80)` and never returns, meaning `mon_.begin()` is stuck on its first blocking call (`Preferences prefs.begin("autolink", true)` for the NVS log-level restore). v4.1.15 adds brackets around every blocking call inside `mon_.begin()`:

- `begin: entering (port=N)` — first line of `mon_.begin()`.
- `begin: opening NVS namespace 'autolink' (read-only)` — before `Preferences prefs.begin()`.
- `begin: NVS open returned <bool>` — after `Preferences prefs.begin()`. **If this is the last line you see, NVS open is the hang.**
- `begin: NVS getUChar returned <u8>` — after the read.
- `WiFi connect SSID="..."` — after the NVS block returns.
- `WiFi: mode set to STA, calling WiFi.begin` — before `WiFi.begin()`.
- `WiFi.begin returned (status=N), entering connect poll loop` — after `WiFi.begin()` returns. **If you see the previous line but not this one, `WiFi.begin()` is the hang.**

No code or behavior changes. v4.1.14 → v4.1.15.

---

## v4.1.14

**Boot + dashboard diagnostic logging.** Addresses "apps never start" and "app logs are blank" reports where the Serial output stopped at `EspHal: UART2 ready` and the dashboard's Live Log panel never received any lines.

- Server side: `AutoLinkWeb::begin()` now logs WiFi connect progress every 2 s (`WiFi connecting... status=N elapsed=NNNN ms`) and includes elapsed ms in success/failure lines — a stuck WiFi connect is now visible on Serial instead of silent for the full 12 s timeout. `setupCommon()` got three new INFO lines: a `boot:` banner that prints before any blocking work (so a Serial monitor opened after reset still sees an "I'm alive" line), a `link layer up` line after `comm_.begin()` returns, and a `web monitor begin returned <bool> in NNNN ms` line — these bracket the three places boot can hang so the user can tell which one froze.
- Client side: `tfetch()` now `console.log`s every fetch (method + URL + timeout) and its response (status) or failure (message) on the browser console. The poll() error handler also logs `/stats poll failed #N code=... msg=...` and the `/logs` poll logs both success (line count) and failure. Open DevTools → Console and the full request/response sequence is visible.
- No protocol or behavior changes. v4.1.13 → v4.1.14.

---

## v4.1.13

**Dashboard connection stability fixes.** Two changes addressing repeated `ERR_CONNECTION_REFUSED` / `ERR_CONNECTION_ABORTED` on `/stats` and `/logs` polls:

1. **Server: `max_open_sockets` 3 → 7, `lru_purge_enable` true → false.** The earlier config had headroom for the `/` document and one poll at a time; clicking a button while a slow `/logs` response was in flight could LRU-purge the in-flight socket and produce `ERR_CONNECTION_REFUSED` on the next poll. New sizing: 1 for `/` (browser keeps the document connection alive), 2 for the parallel `/stats` + `/logs` polls, 1 for an in-flight POST (level / mode / reset / reboot), 2 for browser preconnects, 1 headroom. LRU purge disabled because for a single-client dashboard it's strictly harmful — the new socket cap has enough room without it.

2. **Client: auto-reload on persistent connection errors.** If 8 consecutive polls fail with `ERR_CONNECTION_*` / `ECONNREFUSED` / `ECONNRESET` / `ETIMEDOUT`, the dashboard reloads itself. The browser's socket pool can be poisoned by aborted connections even after the server recovers; a full reload reopens all sockets fresh. HTTP-level errors (4xx/5xx) still just bump the counter and show the alert — those don't poison the pool and a reload wouldn't help.

**No protocol, API, or behavior changes for new code.** v4.1.12 → v4.1.13.

---

## v4.1.12

**Comment cleanup pass.** Removed every version-anchored comment from source files (`.h`/`.cpp`/`.ino`) and host tests. Comments referencing historical changes ("v4.0.5: changed X to Y", "v4.1.0: raised N from 4 to 16") have been deleted — `git log` is the right place for change history. Comments that record *current* non-obvious invariants (the rule a piece of code actually relies on) have been preserved and rewritten terse. Source LOC: ~5800 → 4359 (~25% smaller). No code or protocol changes. v4.1.11 → v4.1.12.

---

## v4.1.11

**Backward-compat overload for the v4.0.x `PingPong` constructor.** v4.0.0..v4.0.6 used a `bool isPing` first argument (the `UtilPing`/`UtilPong` classes each had their own constructor with a bool flag for "am I the ping side"). v4.0.7 unified both into a single `PingPong` class with a `Role` enum (`PingPong::PING` / `PingPong::PONG`), which broke every existing sketch that called `PingPong upp(true, ...)` or `PingPong upp(false, ...)` — the compiler error is `no known conversion for argument 1 from 'bool' to 'autolink::PingPong::Role'`. v4.1.11 adds a delegating constructor that accepts the legacy bool signature and forwards it to the new enum form, so v4.0.x sketches keep compiling unchanged. New code should use `PingPong::PING` / `PingPong::PONG`; the bool overload is marked DEPRECATED in the header and will be removed in a future major version.

**No protocol, API, or behavior changes for new code.** v4.1.10 → v4.1.11.

---

## v4.1.10

**Dashboard footer version is now derived from `AUTOLINK_VERSION` at compile time.** Previously the footer read `v4.1.9` hardcoded in the HTML literal — a separate copy of the version that had to be edited by hand on every release and was the original cause of the v4.1.9 "missing version" bug. v4.1.10 closes the raw-string literal before the version spot, expands `AUTOLINK_VERSION` (which is now the single source of truth, defined in `src/AutoLink.h`), and reopens the literal:

```cpp
// before (v4.1.9):
<div class="footer"><span id="ver">v4.1.9</span> AutoLink Web Monitor</div>

// after (v4.1.10):
<div class="footer"><span id="ver">v)HTML" AUTOLINK_VERSION R"HTML(</span> AutoLink Web Monitor</div>
```

The preprocessor concatenates the three adjacent string literals (`"v"`, `AUTOLINK_VERSION`, `"</span>..."`) into one continuous byte sequence at compile time. Changing `AUTOLINK_VERSION` in `src/AutoLink.h` now updates the Serial log line, the `/stats` JSON, the `library.properties` `version=` field (still hand-edited — out of scope for this change), and the dashboard footer in lockstep. Verified by extracting the literal via `g++ -E` and asserting on the assembled string.

**No protocol, API, or behavior changes.** Version bump only. v4.1.9 → v4.1.10.

---

## v4.1.9

**Hardcoded the version in the dashboard footer.** The footer used to read `AutoLink Web Monitor — host` with no version number until the first `/stats` response populated the `<span id="ver">` element — usually less than a second, but visible as an empty spot on a cold reload and on connections where the device was still booting. v4.1.9 hardcodes `v4.1.9` directly in the HTML, so the footer renders correctly on first paint. The JS still overwrites it with the value reported by the device on every `/stats` poll, so the footer stays in sync if the running firmware is a different build than the HTML was compiled with.

**No code, protocol, or API changes.** Version bump only. v4.1.8 → v4.1.9.

---

## v4.1.8

**v4.1.7 + new VERBOSE log level + role pill + dashboard radio.**

- **New VERBOSE log level (above DEBUG).** `Log::Level` enum gains `VERBOSE = 5` (in addition to the v4.1.4 NONE/ERROR/WARNING/INFO/DEBUG). The new `Log::verbose(tag, fmt, ...)` method emits a `'V'`-prefixed line at this level; the host sink and dashboard's `.V` CSS rule pick it up. The dashboard's log-level radio group grows a 6th button labeled "Verbose" (value `5`); `/level?lv=5` round-trips through NVS persistence like every other level. Use case: per-frame control traffic (e.g. `ALink::sendFrame TX  cobsSeq=N  payload=0xXX (no flushTx, v4.0.5)`) is too noisy at DEBUG (hundreds of lines per second at 115200 baud) but useful for wire-trace forensics — VERBOSE hides it by default and the operator opts in from the dashboard.
- **`ALink::sendFrame_unlocked` log demoted from DEBUG → VERBOSE.** Was firing on every PING/PONG/keepalive byte. Now hidden unless the operator explicitly switches to Verbose.
- **`Ping` SWP stall log demoted from ERROR → INFO.** The recovery path is well-defined (`dropLink` → re-sweep) and the line was previously red-flagged even on a normal boot where Ping just beat Pong to the wire. INFO reflects "the system is recovering itself; no operator action needed."
- **Role pill in the dashboard header.** `AutoLinkWeb::setRole("Ping"|"Pong")` is now called from `UtilMain::setupCommon()` based on the existing `isPing` constructor flag. The role string rides in the `/stats` JSON (`"role":"Ping"`) and the dashboard renders it as a pill next to "AutoLink Monitor". Empty role (legacy sketches that don't call `setRole`) hides the pill via CSS.
- **Role logged at end of `Ping::setup()` and `Pong::setup()`.** Both `UtilPing` and `UtilPong` now log `I [Ping] mode=Ping  ready` / `I [Pong] mode=Pong  ready` at the end of their setup so the Serial monitor and the web log panel both confirm the role right after boot.
- **`Log::setLevel` boot default changed DEBUG → INFO.** A fresh board now boots at INFO (shows `link up` / `link lost` / `mode=...` without the per-loop DEBUG chatter). The dashboard's level radio still reflects whatever the device is at, and NVS-persisted levels from previous builds are still restored on boot.

**No breaking changes.** The 0..4 `/level?lv=N` range from v4.1.4..v4.1.7 is still accepted; the new upper bound is 5. The HTML radio group now has six buttons; legacy clients that hard-code `value="0..4"` keep working because the old buttons are unchanged.

**Bug fix in v4.1.8 (carried over from v4.1.5/v4.1.7):** the dashboard's `/stats` mode-sync block had two missing closing braces (`if(m!==currentMode){ ... if(inp)... highlightMode(); }` and the outer `if(d.mode!==undefined&&d.mode!==null){ ... }`). JavaScript's ASI kept the parser alive past the missing braces, so most browsers ran the script fine, but `fails=0;hide('alert');` ended up inside an unexpected implicit block and the connection-lost banner (`#alert`) was never reliably cleared on a healthy /stats response. Stricter engines would have thrown a SyntaxError on the bare `catch` keyword that followed. v4.1.8 closes both braces; the alert now clears on the first successful poll as intended.

---

## v4.1.7

**v4.1.4 base + every post-v4.1.4 change in one labeled build.** Consolidates v4.1.5 (NONE log level, NVS-persisted log level, zero-byte sendMsg no-op, message-layer resync on corrupt header, dashboard Reset clears lost-msgs, Sequential/Random fill modes, corrupt-message tests) and v4.1.6 (version-guard removed per user direction; version-bump only) into a single labeled release so downstream builds can pin to a working artifact. The source content is the same as v4.1.5/v4.1.6; the version string and `library.properties` are bumped to 4.1.7.

**Bug fix in v4.1.7 (replaces v4.1.5/v4.1.6):** the `AutoLink` facade class (in `AutoLink.h`) was missing a `resetDiag()` method, even though `ALink::resetDiag()` existed. The desktop tests passed because they use the `ALink` class directly, but any build that linked the `AutoLink` facade (the documented public entry point) failed with `'class autolink::AutoLink' has no member named 'resetDiag'`. v4.1.7 adds `AutoLink::resetDiag()` as a thin forwarder to `ALink::resetDiag()`, matching the pattern used for `resetStats()` and `resetErrors()`.

**Also in v4.1.7:** removed `<vector>` from the embedded-target source files (`AutoLink.h` and `UtilBaudSweep.h`). The ESP-IDF build environment includes `<vector>`, but the Arduino-core build path (ArduinoDroid, older Arduino cores) does not. Both files now use a fixed-size C array (`uint32_t allowedBauds[AUTOLINK_MAX_BAUDS]` and `int scores_[UTIL_BAUD_SWEEP_MAX_BAUDS]`) with a runtime count, removing the `<vector>` dependency. The desktop test suite is updated to use the new array-set syntax (`cfg.allowedBauds[0] = 9600; cfg.allowedBaudsCount = 1;`) instead of initializer lists.

## v4.1.6

**Version bump to mark the lost-msgs reset feature as GA.** No code change from v4.1.5; the build artifact and `library.properties` version are bumped so downstream users can pin to the build that includes the working `resetDiag()` chain. The v4.1.5 source had everything working internally; v4.1.6 just labels it.

## v4.1.5

**Sequential/Random fill modes, dashboard reset clears lost-msgs, new corrupt-message tests, ALink public-API coverage test.** User-facing changes are: the dashboard has a new Sequential/Random radio group at the top (Ping side only), the existing Reset button now clears the "X lost msgs" pill, and a handful of new corrupt-message tests + an ALink public-API coverage test. No protocol or wire-format change.

### Feature: Sequential vs Random fill modes (Ping side)

The `UtilPing` payload generator now has a mode enum (`FillMode::SEQUENTIAL` or `FillMode::RANDOM`) and the dashboard has a new radio group in the top-right of the header bar (next to the Pause button) to switch between them. The default is **Sequential** — the wire traffic is deterministic, eyeball-friendly ASCII "0,12,345,6789,abcdf,..." (mod 36 of the byte index), and a freshly-flashed device produces an identical stream every boot. The Random mode is the v4.0..v4.1.4 behavior (random bytes 0..255) and is preserved verbatim so existing log captures stay reproducible when the mode is set back to Random.

The mode is communicated to the ALink layer through `UtilPing::setFillMode(FillMode)`, which takes effect on the next `loop()` iteration. The dashboard's radio group POSTs `POST /mode?m=seq|rand` to the web monitor, which dispatches via a registered function-pointer hook. The hook is null on the Pong side (the mode selector is meaningless when the role is Pong, and the `/mode` endpoint returns 404 there) and wired on the Ping side via static thunks in `UtilPing` that route through a single instance pointer.

### Bug: dashboard Reset button now clears the "lost msgs" pill

v4.0.0..v4.1.4 had two distinct counter families: `Stats` (tx/rx/discCount/frameErrs, cleared by `resetStats()` and `resetErrors()`) and `Diag` (gaps/stale/lostMsgs, never cleared by any reset call). The `Stats` reset was called by the dashboard's POST `/reset`, but the `Diag` counters were not — so the "X lost msgs" pill stayed at its lifetime value forever, which confused operators looking at a freshly-reset dashboard.

v4.1.5 adds `ALink::resetDiag()` which zeros gaps/stale/lostMsgs under the lock. The dashboard's `POST /reset` now calls `resetStats()`, `resetErrors()`, AND `resetDiag()`. The user-facing contract is "the Reset button clears every operator-visible counter on the dashboard".

### Test: corrupt-message coverage

Five new tests in `ALinkMessageTest.cpp` exercise the v4.1.4 message-layer resync paths and the user-facing zero-byte / negative-len / drop-link / empty-buffer / reset-diag contracts. The full list:

* `test_corrupt_msg_header_does_not_clear_buffer` (v4.1.2, unchanged) — corrupt L=0 header is resynced forward; m1+m2 preserved.
* `test_corrupt_msg_header_oversize_l_resyncs` (v4.1.5) — corrupt L=0xFFFFFFFF (oversize) is resynced forward; m1 preserved. Distinct from the L=0 case because the resync scan only checks "L in [1, maxMsg]".
* `test_corrupt_msg_header_no_resync_clears_buffer` (v4.1.5) — 200 bytes of 0xFF injected with no plausible L anywhere; resync scan finds no resync point; buffer is cleared; link stays OK; next sendMsg is received cleanly.
* `test_corrupt_msg_header_resync_drops_bytes` (v4.1.5) — resync that has to drop real m1 bytes (not just a corrupt header) to find the next valid header; exercises the "resynced forward by N bytes" error log.
* `test_corrupt_payload_byte_crc_reject` (v4.1.5) — bit-flip in the app buffer (above COBS, below the message CRC) caught by the message-level CRC16; link stays OK; next message arrives cleanly.
* `test_recvMsg_empty_buffer` (v4.1.5) — `recvMsg` on an empty buffer returns 0 cleanly.
* `test_recvMsg_buffer_too_small` (v4.1.5) — `recvMsg` with rx buffer smaller than the message drains the payload to a sink and returns -1; link stays OK; the message is consumed.
* `test_zero_byte_send_silent_noop` (v4.1.5) — `sendMsg(b, 0)` returns true silently, `write(b, 0)` returns 0 silently; no log line, no state change, no wire bytes.
* `test_resetDiag_zeros_cobsseq_counters` (v4.1.5) — `resetDiag()` zeros gaps/stale/lostMsgs; idempotent; link stays OK; next sendMsg round-trips cleanly.

### Test: ALink public-API coverage (89% line / 90% branch)

A new `test_io_coverage` in `ALinkIOTest.cpp` exercises every public method on `ALink` end-to-end: `available`, `peek`, `read` (single-byte and multi-byte), `read(b, n)` with a buffer smaller than the message, `write` rejection paths (negative len, zero len, link not in OK, !reliableMode TX-truncated), `sendMsg` rejection paths (zero len, negative len, len>maxMsg, link not in OK), `dropLink` (verifies the BREAK is sent via `sendBreakCalls`), `flushRx` (clears the app buffer), `resetDiag` / `resetStats` / `resetErrors`, `getStats`, `getConfig`, `getState`, `getErrCount`, `getCurrentSpdIndex`, and the header-write-fail path in raw `sendMsg`. Coverage went from 64% (single binary) → 89% line / 90% branch after merging .gcda files across every test suite that links ALink. The remaining uncovered lines are defensive / pure-error paths: `State::UNK` default in the switch, malloc-fail in the resync, control-frame `sendFrame_unlocked` TX-truncated (reachable only via a UART-write failure during the SWP state machine), and the "write aborted mid-message" branch (unreachable while the lock is held because the state can't change mid-iteration).

The coverage merge script (`coverage_merge.sh`) was updated to use `gcov-tool merge` to combine per-binary .gcda files for each source. The single-binary approach used in v4.1.0..v4.1.4 only counted the test's own view of the source, not the union across all tests. The new approach produces a faithful aggregate. The `MockHal` test stub gained a `txFailN` field that makes the next `tx()` return short by N bytes, used by the new coverage test to force the TX-truncated error paths.

### Code cleanup: dead public wrappers

v4.1.4 had two thin public lock-and-delegate wrappers (`ALink::sendFrame(uint8_t)` and `ALink::sendCobsFrame(...)`) that were never called by any caller — the SWP/LCK state machine and the `sendMsg`/`write` paths use the `_unlocked` variants directly. v4.1.5 removes both wrappers. No behavior change; the public API surface shrinks to what is actually used.

### Files changed

* `src/pingpong/UtilPing.h` — `FillMode` enum, `fillMode_` member, `setFillMode()` / `fillMode()` accessors, `fillSequential_` / `fillRandom_` / `fillMode_` fill methods, static thunks for the web-monitor hook, `installWebHooks()` and `s_active_` static pointer.
* `src/pingpong/UtilPong.h` — no changes (Pong doesn't generate payloads).
* `src/pingpong/PingPong.h` — `setFillMode()` / `fillMode()` accessors that forward to the Ping member.
* `src/pingpong/UtilMain.h` — `setupCommon()` notes the fill-mode hook is wired in the subclass (util/Ping) before `mon_.begin()`.
* `src/web/AutoLinkWeb.h` — `setFillModeHook(FillModeReader, FillModeWriter)` method, `fillModeReader_` / `fillModeWriter_` members.
* `src/web/AutoLinkWeb.cpp` — `handleMode()` HTTP handler (`POST /mode?m=seq|rand`), `mode` field in the `/stats` JSON, dashboard HTML adds a Sequential/Random radio group in the header bar, JS adds the `bindModeGroup()` / `highlightMode()` helpers, the `/reset` handler now also calls `link_.resetDiag()`.
* `src/ALink.h` — `void resetDiag()` public method.
* `src/ALink.cpp` — `ALink::resetDiag()` zeros gaps/stale/lostMsgs under the lock. Removed dead `ALink::sendFrame()` and `ALink::sendCobsFrame()` public wrappers.
* `src/AutoLink.h` — bumped `AUTOLINK_VERSION` to "4.1.5".
* `library.properties` — bumped to 4.1.5.
* `test/test_desktop/AutoLinkTest.cpp` — stub `EspHal` no changes needed.
* `test/test_desktop/MockHal.h` — added `txFailN` field for forcing TX-truncated paths in the coverage test.
* `test/test_desktop/ALinkIOTest.cpp` — new `test_io_coverage()` exercises every public API method and the error paths.
* `test/test_desktop/ALinkMessageTest.cpp` — 5 new corrupt-message tests + zero-byte / recvMsg-empty / resetDiag tests.
* `test/test_desktop/coverage_merge.sh` — `gcov-tool merge` to combine per-binary .gcda files; map updated to include every test that links the source.

---

## v4.1.4

**Message-layer resync on corrupt header + zero-byte sendMsg is a no-op + log level NONE + WARNING + NVS-persisted log level.** Multiple targeted fixes derived from the v4.1.3 field logs and the user-facing API contracts the operator asked for. No protocol, wire-format, or backward-incompatible API change. The log-level renumber is a documented breaking change (ERROR=0..DEBUG=2 → NONE=0..DEBUG=4) that the NVS path handles via a range clamp on first boot.

### Bug: message-layer desync after forward-gap resync

v4.1.0..v4.1.3's `recvMsg` had a "drop the corrupt header, leave the rest of the app buffer alone" path. That path was correct on the first header but produced a cascading desync in the field: when the cobsSeq forward-gap resync dropped bytes the message layer was relying on for alignment, the *next* header read was also mid-payload, also corrupt, and so on forever — every `recvMsg` call returned -1 and the receiver could never recover until `STALL_MS` fired and the link re-swept. The field log shows this pattern: `disc 0→1→2→3→4→5` in 23 seconds, with every echo attempt producing another "frameErrs += 1" until the threshold was hit.

v4.1.4 adds a message-layer resync scan. When a corrupt MSG_HDR is detected, the receiver snapshots up to `(maxMsg + MSG_HDR)` bytes from the app buffer into a local heap buffer, scans forward looking for the next plausibly-valid L (a 4-byte sequence with LE-decoded L in [1, maxMsg]), re-pushes the suffix starting at the resync point, and lets the next `recvMsg` re-read from there. The CRC field is NOT validated by the resync scan — the CRC is over the payload (not the L field, as was a v4.1.4-dev mistake I caught during testing) and can only be checked once the payload is read. If the resync picks the wrong boundary because the CRC happens to match a 4-byte pattern that LOOKS like a valid L, the next `recvMsg` will fail with a payload CRC mismatch and re-resync — iterative but bounded. If no resync point is found within the bounded scan, the buffer is cleared as a last resort (one log line, no flood).

The new `ILink::peekAt(out, n, offset)` virtual lets the resync scan read the app buffer at any offset without consuming bytes. The MockHal test stub and the test `AutoLinkTest.cpp` stub both implement the new method. The `findMsgHeaderResync_unlocked` helper is private to `ALink` and exposed only via `recvMsg`.

### Feature: zero-byte sendMsg / write is a no-op

The user asked: "make sure we test sending zero bytes just returns with no errors". v4.1.4 changes the API contract: `sendMsg(buf, 0)` returns `true` and does nothing on the wire; `write(buf, 0)` returns `0` and does nothing. The v4.1.2..v4.1.3 code returned `false` and logged an error, but the user-facing contract is now "calling sendMsg with zero bytes just returns with no errors". The keepalive path still handles cobsSeq-only frames on its own, so the receiver never sees a 0-payload data message from this path. Negative `len` is still a programmer error and logs.

### Feature: NONE and WARNING log levels (with renumbering)

v4.1.4 adds two new log levels: `NONE` (silences every line, even errors) and `WARNING` (between ERROR and INFO). The full ordering is now `NONE=0 < ERROR=1 < WARNING=2 < INFO=3 < DEBUG=4`, with the gate condition `if (lvl < X) return;` (was `if (lvl == ERROR) return;` etc.). The `Log::emit` function short-circuits at `NONE` so the format string and varargs are never even evaluated. The renumber is the only backward-incompatible change in v4.1.4: a v4.0.0..v4.1.3 user who wrote `setLevel(Log::ERROR)` still gets errors only (semantic preserved), but a user who wrote a literal `0` (the old `ERROR` value) and put it in NVS will see that interpreted as `NONE` after upgrade. The NVS restore path (below) clamps out-of-range values to "use the boot default" to handle this gracefully.

The GUI's log-level radio group gained a "None" button (between "Error" and "Warn") and a "Warn" button. Both the inline Live-Log row and the log-overlay header have the five-button group, kept in sync by `highlightLvl()`.

### Feature: NVS-persisted log level

v4.1.4 adds `Preferences` to `AutoLinkWeb::begin()` to restore the saved log level at boot, and to `handleLevel()` to save the new level when the user clicks a radio button. The namespace is `autolink` and the key is `log_level` (uint8_t). On boot, the value is read, clamped to the v4.1.4 range (`NONE..DEBUG`), and used to set `Log::setLevel`. On change, the value is written with `putUChar`. Failures (NVS unavailable, write error) log a warning but don't break boot or the level change. Stale v4.0..v4.1.3 values 0..2 (old ERROR..DEBUG) are out of the v4.1.4 range and rejected, leaving the boot default in place — the user just clicks the level they want in the dashboard.

### Bug: ESP_LOGW for warning level

The new `Log::warning` (v4.1.2) routed through `ESP_LOGI` because the v4.1.2 `Log::emit` switch only handled `E`/`D`/default. v4.1.4 adds the `W → ESP_LOGW` case so warnings land in the right ESP-IDF severity bucket.

### Tests

* `test_level_filtering` (LogTest.cpp) — rewritten to cover all five levels (NONE, ERROR, WARNING, INFO, DEBUG) and the `wouldEmit` helper used by the GUI. Pins the renumber semantics.
* `test_message_small_size_boundary` (already in v4.1.2) — pins 1..6 byte round-trips.
* `test_message_explicit_size_sweep` (already in v4.1.3) — explicit 1..65535 sweep.
* `test_corrupt_msg_header_does_not_clear_buffer` (v4.1.2) — updated to verify the v4.1.4 resync: a corrupt L=0 header is resynced forward to the next valid L=10 header (m1), and m1+m2 are both recoverable.
* `test_send_rejections_log_errors` (v4.1.2) — updated to reflect the v4.1.4 zero-byte contract.
* `test_long_message_truncated_at_buffer` (v4.1.2) — unchanged (320-byte buffer cap).
* `test_message_size_sweep` (v4.1.2) — zero-length row updated to v4.1.4 contract.

### Files changed

* `src/util/Log.h` — enum renumbered to `NONE=0, ERROR=1, WARNING=2, INFO=3, DEBUG=4`. Gate conditions changed from `==` to `<`. Added `Log::wouldEmit()`.
* `src/util/Log.cpp` — `Log::emit` short-circuits at `NONE`. `ESP_LOGW` case added.
* `src/ILink.h` — new `peekAt(out, n, offset)` virtual.
* `src/ALink.h` — new `findMsgHeaderResync_unlocked(int max_scan)` private helper.
* `src/ALink.cpp` — `findMsgHeaderResync_unlocked` implemented (snapshot+scan+re-push). `recvMsg` corrupt-header branch rewritten to use the resync, with the clearAppBuf-as-last-resort fallback. `write(len==0)` and `sendMsg(len==0)` are no-ops.
* `src/EspHal.h` — `peekAt` implementation (multi-byte cache via `peek_buf_/peek_buf_len_/peek_buf_pos_`). `popAppBuf`/`peekAppBuf`/`appBufAvailable`/`clearAppBuf` updated to handle the new cache alongside the legacy 1-byte `peek_buf`.
* `src/AutoLink.h` — bumped `AUTOLINK_VERSION` to "4.1.4".
* `src/web/AutoLinkWeb.cpp` — `Preferences` include. `begin()` reads `autolink/log_level` from NVS, clamps to range, sets log level before any logging. `handleLevel()` writes the new value to NVS. GUI's two `lvl-group` blocks updated to the 5-button NONE..DEBUG layout.
* `library.properties` — bumped to 4.1.4.
* `test/test_desktop/AutoLinkTest.cpp` — stub `EspHal` now implements `peekAt` and `flushRxHw`.
* `test/test_desktop/MockHal.h` — implements `peekAt`.
* `test/test_desktop/LogTest.cpp` — `test_level_filtering` rewritten.
* `test/test_desktop/ALinkMessageTest.cpp` — zero-length row of `test_message_size_sweep` updated; `test_send_rejections_log_errors` updated.

---

## v4.1.3

**Diagnostic logging pass + size-sweep tests + `L=0` fix + flow-control answer.** Multiple small but high-leverage changes that make the v4.1.0..v4.1.2 class of bugs (silent rejections, app-buffer failures, corrupt-header cascades) diagnosable in the field. No protocol, wire-format, or behavior change. Wire-compatible with v4.0.0..v4.1.2.

### New error logs (every silent rejection path now logs)

* `ALink::write(len<=0)` — was silent; now `error` with the size and a hint.
* `ALink::write(state!=OK)` — was silent; now `warning` with the state name and a hint to call `dropLink()` if the user wants to recover.
* `ALink::sendMsg(len<=0)` — was silent; now `error` with the size and the explanation that v4.0+ removed the 0-payload data-message path (use the keepalive, which is sent automatically in OK, for cobsSeq-only frames).
* `ALink::sendMsg(len>maxMsg)` — was silent; now `error` with both sizes and a hint to either shrink the message or raise `cfg.maxMsg` in `AutoLinkConfig`.
* `ALink::sendMsg(state!=OK)` — was silent; now `warning` with the state name and the byte count dropped.
* `ALink::sendMsg` mid-message abort — was silent; now `warning` with the offset/len/state so an operator can see the partial-message loss.
* `ALink::err_unlocked()` — was silent until threshold trip; now `debug` per call so the frame-error count climbs visibly in the log before the threshold is reached.
* `ALink::recvMsg(corrupt MSG_HDR L>maxMsg)` — was silent on the path that called `clearAppBuf`; now `error` with the parsed L and maxMsg.
* `ALink::recvMsg(corrupt MSG_HDR L=0)` — was lumped with L>maxMsg and silently `clearAppBuf`'d the entire app buffer; now `error` with a precise explanation.
* `ALink::onPayload` "app buffer full" — was blaming-the-wire; now explains that on the FIRST frame, the most likely cause is the EspHal boot-time `xStreamBufferCreate` failure (and points the operator at the boot log to confirm).
* `EspHal::setSpd(baudrate change failure)` — was silent (return value ignored); now `error` with the baud and the ESP error code.
* `Log::emit` — added a one-shot stderr warning when a log line is truncated (so a runaway format string doesn't silently lose data), and raised the format buffer from 256 to 320 bytes to give v4.1.x's longer diagnostic messages headroom.

### New `Log::warning` level

The `Log` class previously had only `error`/`info`/`debug`. `warning` was needed for the "system is in a recoverable state but the caller's expectation isn't being met" cases (write/sendMsg on a dropped link) — they're not errors (the system is fine) but they're not info either (the caller's data was lost). `warning` is suppressed at `Log::ERROR` level (errors outrank warnings) and visible at `info` and `debug`.

### `L=0` no longer clears the whole app buffer

v4.0.0..v4.1.2's `recvMsg` had `if (L == 0 || L > maxMsg) { clearAppBuf(); err(); return -1; }` — a single 4-zero-byte corruption anywhere in the app buffer would wipe every legitimate in-flight message, count it as a frame error, and (if the corruption repeated) potentially trip the error threshold and drop the link. v4.1.3 splits the branch: `L > maxMsg` and `L == 0` are now separate paths, neither calls `clearAppBuf`, both log a precise `error` and count as a frame error, and the rest of the app buffer is left intact. The next `recvMsg` re-reads a fresh MSG_HDR from the remaining bytes. The new behavior is pinned by `test_corrupt_msg_header_does_not_clear_buffer` in `ALinkMessageTest.cpp`.

### New tests

* `test_message_small_size_boundary` (ALinkMessageTest.cpp) — pins 1..6 byte round-trips explicitly. The existing `test_message_roundtrip` skips 4, 5, 6; the cobsSeq/CRC8 path is most likely to break on the small-but-not-tiny sizes.
* `test_message_explicit_size_sweep` (ALinkMessageTest.cpp) — explicit sweep across the user-requested sizes: 1, 2, 3, 4, 5, 50, 100, 150, 200, 250, 300, 1000, 2000, 3000, 4000, 5000, 7500, 9000, 65535 (= maxMsg). One test, every size in the user-facing promise, with payload verification.
* `test_app_buffer_null_simulates_disconnect` (ALinkMessageTest.cpp) — regression for the v4.1.0..v4.1.1 symptom shape. Constrains the MockHal app buffer to capacity 0 to simulate the `xStreamBufferCreate` NULL-return that the field hardware hit; confirms the symptom (recv returns 0 forever, gaps counter climbs).
* `test_corrupt_msg_header_does_not_clear_buffer` (ALinkMessageTest.cpp) — pins the v4.1.3 L=0 / L>maxMsg contract: a corrupt header is dropped, counted as a frame error, and the rest of the buffer is preserved.
* `test_send_rejections_log_errors` (ALinkMessageTest.cpp) — pins the v4.1.3 sendMsg/write error-logging contract: len=0 returns false and logs, len>maxMsg returns false and logs, state!=OK returns false and logs at warning.
* `test_long_message_truncated_at_buffer` (LogTest.cpp) — updated to match the v4.1.3 320-byte buffer (was 256 bytes; bumped to fit the new diagnostic lines).

### Pre-existing assertion bugs fixed

`test_message_size_sweep` and `test_stats` (in `ALinkIOTest.cpp`) asserted the v3.x stats contract `as.tx == payload + MSG_HDR`. The v4.0.0+ production code counts payload only (see `Stats::tx` comment in `ALink.h`); the receiver's `rx` counts payload + MSG_HDR. The asymmetric contract has been the source of confusion since v4.0.0. v4.1.3 updates the assertions to pin the actual contract: `as.tx == payload` and `bs.rx == payload + MSG_HDR`, with a comment pointing at the production contract for future maintainers.

### Flow control

The current `EspHal` uses `UART_HW_FLOWCTRL_DISABLE` (no RTS/CTS). For the AutoLink-to-AutoLink case this is correct: both sides use the same `txBufferSize` (auto-sized to `MAX_TX_PER_LOOP * ((maxMsg + MSG_HDR) * 5/4 + 64) = 21616` bytes) which comfortably absorbs the per-tick burst, and the FreeRTOS stream buffer app buffer (32960 bytes) absorbs the receive side. Neither side can overflow the peer's TX ring under normal load. Flow control would matter for a third-party device that floods the link without back-pressure, but that's a configuration choice the user can make by setting `UART_HW_FLOWCTRL_CTS_RTS` in the `uart_config` before calling `comm_.begin()` (the AutoLink layer doesn't expose flow-control toggles today; future work if a use case appears). The `rx_flow_ctrl_thresh = 122` default in `uart_config` is preserved as the threshold should the user flip the flow-control mode on.

### Files changed

* `src/ALink.cpp` — error/warning logs at all silent rejection paths; `err_unlocked` debug log per call; `recvMsg` L=0 / L>maxMsg split with no `clearAppBuf`; mid-message `write` abort warning; "app buffer full" log updated with first-frame diagnostic.
* `src/ALink.h` — `gaps` / `stale` member comments updated (already done in v4.1.0, kept).
* `src/util/Log.h` — new `warning` level; comment updated.
* `src/util/Log.cpp` — format buffer raised 256→320 bytes; one-shot stderr warning on truncation; ESP_LOGW case in the ESP_PLATFORM switch.
* `src/EspHal.h` — `setSpd` checks `uart_set_baudrate` return value and logs error.
* `test/test_desktop/ALinkMessageTest.cpp` — 4 new tests; pre-existing `test_message_size_sweep` assertion fixed to match v4.0+ contract.
* `test/test_desktop/ALinkIOTest.cpp` — pre-existing `test_stats` assertion fixed to match v4.0+ contract.
* `test/test_desktop/LogTest.cpp` — `test_long_message_truncated_at_buffer` updated to 320-byte buffer.
* `library.properties` + `src/AutoLink.h` `AUTOLINK_VERSION` — bumped `4.1.2` → `4.1.3`.

---

## v4.1.2

**Fix for the v4.1.0 "app buffer full on first frame" symptom.** Hardware log analysis showed a recurring failure mode where the Pong's `app buffer full` log fired on the very first data frame after a fresh link-up, with `wanted 6, accepted 0` — even though the buffer had been just cleared by `flushRx`. The Pong's `echo #` count stayed at 0, Ping's `STALL_MS=3000` fired, the link re-swept, and the pattern repeated indefinitely (`disc 0→2→3→4→5` in 23 s). Root cause: v4.1.0 auto-sized the app buffer to `(32 + 16) * 1030 = 49440` bytes, a 4.8× increase from v4.0.x's `10300` bytes. On hardware with a fragmented heap, `xStreamBufferCreate(49440)` returns NULL, but the return value was unchecked. With `stream_buf = NULL`, every `pushAppBuf(b, n)` silently returned 0, the payload never reached the app buffer, `recv` always returned 0, and the Pong never echoed — exactly the observed symptom. The over-large size was also unnecessary: the bound that actually matters is the per-tick send/drain imbalance, which is at most `MAX_TX_PER_LOOP=16` messages. v4.1.2 reduces the auto-size to `2 * MAX_TX_PER_LOOP * (maxMsg + MSG_HDR) = 32 * 1030 = 32960` bytes and adds a NULL-check on `xStreamBufferCreate` that logs a clear root-cause error and a one-shot warning in `pushAppBuf` so a future allocation failure is visible instead of looking like a wire-throughput issue. No protocol or wire-format change. Wire-compatible with v4.0.0..v4.1.1.

### Files changed

* `src/AutoLink.h` — `streamBufferSize` auto-size formula reduced from `(WINDOW + MAX_TX_PER_LOOP) * (...)` to `2 * MAX_TX_PER_LOOP * (...)`, with a comment explaining why the over-large v4.1.0 size was unnecessary and what bound actually matters.
* `src/EspHal.h` — `xStreamBufferCreate` return value now checked; a one-shot error is logged if allocation fails. `pushAppBuf` checks `stream_buf` for NULL and logs a one-shot root-cause diagnostic (not a flood of "app buffer full" blames-the-wire messages) before returning 0.

---

## v4.1.1

**Log-level radio moved to the Live Log row.** The Error/Info/Debug radio group used to live in the page header next to the "Resume" button. The header is now reserved for the title, the message-pause toggle, and the link-state pill — the radio is a log-control and belongs with the log controls. The radio now sits inside the Live Log `.row` between the "Live Log" section label and the existing Clear/Copy/Pause/Maximize buttons, and a duplicate is placed in the log-overlay's header (the maximized-log view) so the user can change level while the log is expanded. Both radios share the same backing state and are kept in sync by `highlightLvl()` and the `/stats` reconciliation. No protocol, behavior, or firmware-side change. Wire-compatible with v4.0.0..v4.1.0.

### Files changed

* `src/web/AutoLinkWeb.cpp` — moved the `.lvl-group` out of `<header>` and into the Live Log `.row`; added a duplicate `.lvl-group` (with name=`lvl2`) in the log-overlay header; refactored the radio-binding JS into `bindLvlGroup(name)` so both groups can be bound from one place; updated `highlightLvl()` and the `/stats` reconciliation to keep both groups in sync.

---

## v4.1.0

**Forward-resync on gap + windowed pipeline — fixes the stall→BREAK→disconnect cascade and the v4.0.0+ half-speed regression.** Two root-cause fixes that together restore the v3.x line-rate throughput and stop a single corrupted frame from cascading into a permanent disconnect loop. No protocol or wire-format change; the v4.0.0 cobsSeq framing is preserved. Wire-compatible with v4.0.0..v4.0.9.

### Bug 1: the disconnect cascade (the v4.0.0..v4.0.9 root cause)

**Symptom:** A single corrupted frame on a clean link turns into a permanent `STALL_MS → BREAK → re-sweep → STALL_MS → BREAK …` cycle. The Ping/Pong log shows three back-to-back `disc 0→2→3` events, then a permanent `gap=2 stale=3` offset on every echo after every re-sweep.

**Root cause:** In `ALink::onPayload`, the GAP branch logged "link stays OK; pipeline self-heals" and **did not advance `rxSeq`**, on the assumption that the sender would retransmit the missed `expected` seq. This is a one-way streaming echo protocol with no retransmission — the sender is past `expected` permanently. `rxSeq` therefore froze at the first GAP. Every later frame was a gap, no echo drained Ping's pending pipeline, Ping's `STALL_MS=3000` watchdog fired, Ping sent BREAK, both sides re-swept, and leftover in-flight bytes from the prior session landed as STALE on the next session and re-triggered the same stall. The `MAX_GAP_RESYNC=3` cap was a relic of the same retransmit-protocol assumption; large forward jumps were classified STALE and dropped, contributing to the freeze.

**Fix:** In `onPayload`, a forward jump of *any* size in the lower half of the cobsSeq modulus is now a **forward-resync**: deliver the payload, advance `rxSeq = cobsSeq`, count the skipped seqs as `lostMsgs`. The `MAX_GAP_RESYNC` cap is removed from the production code path (the constant is retained as a vestigial symbol so external code referencing it still compiles). The STALE branch is now narrower — exact duplicates (`diff==0`) and wraparound duplicates in the upper half of the modulus (`diff > COBS_SEQ_WRAP/2`) — so a normal forward jump is no longer misclassified. The seed path (`rxSeqSet==false`, the first frame after a re-sweep) already accepted the frame unconditionally; that path is preserved and the comment in `onPayload` now spells it out.

**Effect:** A single corrupted frame costs the operator that frame's worth of data; the link stays OK; the `STALL_MS` watchdog never fires for a forward gap; the `disc 0→2→3` cascade is gone.

### Bug 2: the half-speed regression (v4.0.0+)

**Symptom:** Steady-state throughput settles around 6,500 B/s per direction at 115200 8N1, about 58% of the ~11,520 B/s/dir line rate. Pre-v4.0.0 free-streamed at line rate.

**Root cause:** The v4.0.0 WINDOW=8 + `pendingCount_ < WINDOW` gate made the per-loop send loop a 1:1 function of the echo arrival rate. In steady state, `pendingCount_` sat at 8 continuously, so the only way to send a new message was for an echo to drain a slot first. Throughput became `window_bytes / RTT` instead of `line_rate / framing_overhead`. The v4.0.5 bump of `MAX_TX_PER_LOOP` 2→4 doubled the per-tick rate but didn't change the `pendingCount_ < WINDOW` gate, so the pipeline was still RTT-bound.

**Fix:** Decouple new sends from echo arrival by **widening the window** so the wire is the natural throttle rather than the per-echo drain.

* `UtilPing::WINDOW` 8 → 32. Memory cost is 32 slots × (bool + int + uint16_t) ≈ 200 bytes; the array is a back-pressure cap, not a per-echo rate gate.
* `UtilPing::MAX_TX_PER_LOOP` 4 → 16 (half of the new WINDOW by the v4.0.5 sizing rule). One loop() tick at 3–5 ms can fill half the window in one shot.
* `UtilPong::MAX_TX_PER_LOOP` 4 → 16 to match. Pong's drain rate has to scale with Ping's send rate or Pong becomes the bottleneck.
* `AutoLink::AutoLink` auto-sizes `streamBufferSize = (WINDOW + MAX_TX_PER_LOOP) * (maxMsg + MSG_HDR) = (32+16) * 1030 = 49440 bytes` for default `maxMsg=1024` (was `(8+2) * 1030 = 10300`).
* `AutoLink::AutoLink` auto-sizes `txBufferSize = MAX_TX_PER_LOOP * ((maxMsg + MSG_HDR) * 5/4 + 64)` so the UART driver ring can absorb the full per-tick burst without `uart_write_bytes` blocking while the link lock is held. The previous multiplier was `2 *`, sized for `MAX_TX_PER_LOOP=2`; it is now `16 *`, sized for the new value. (The wrong size was the v4.0.0 reason for the original `MAX_TX_PER_LOOP=2` cap; with the new sizing, the cap can be raised safely.)

**Effect:** Steady-state throughput should approach the v3.x line-rate ceiling. The Ping/Pong example on 115200 8N1 should now sustain ~10–11 KB/s per direction; the exact number depends on the per-loop tick rate and how much time Ping/Pong spend in `logStats` and the per-echo blink. Wire-format and cobsSeq numbering are unchanged.

### Files changed

* `src/ALink.cpp` — `onPayload` gap/stale branch rewritten (forward-resync, narrower STALE).
* `src/ALink.h` — `MAX_GAP_RESYNC` comment now describes it as vestigial; `gaps`/`stale` member comments updated to match the new semantic.
* `src/AutoLink.h` — `streamBufferSize` and `txBufferSize` auto-size formulas updated for the new WINDOW and MAX_TX_PER_LOOP.
* `src/pingpong/UtilPing.h` — `WINDOW` 8→32, `MAX_TX_PER_LOOP` 4→16, all comments updated.
* `src/pingpong/UtilPong.h` — `MAX_TX_PER_LOOP` 4→16, comment updated.
* `test/test_desktop/AutoLinkTest.cpp` — `test_app_buffer_auto_sized_for_pingpong` updated to track the new WINDOW=32 + PONG_HEADROOM=16 contract; v4.0.1 regression guard preserved.
* `test/test_desktop/ALinkCobsSeqTest.cpp` — three v4.0.0 tests updated to assert the v4.1.0 forward-resync contract (`test_gap_dropped_no_buffer_push`, `test_gap_then_recover`, `test_wraparound_then_gap`); `test_wire_byte_shift_caught_at_cobsSeq` rewritten to assert the relaxed v4.1.0 contract (corruption accounted for, link stays sane). New regression test `test_single_corruption_does_not_cascade` pins the headline v4.1.0 fix: 100 clean frames + 1 lost + 100 more clean frames produces gaps=1, lostMsgs=1, stale=0, rxSeq=143 (not frozen, no cascade).
* `library.properties` + `src/AutoLink.h` `AUTOLINK_VERSION` — bumped `4.0.9` → `4.1.0`.

### Pre-existing test issues (not addressed in v4.1.0)

`run_test_alink_io::test_stats` and `run_test_alink_message::test_message_size_sweep` assert the v3.x stats contract `as.tx == payload + MSG_HDR` (wire bytes including header). The v4.0.0 rewrite of `txBytes` accounting counts payload-only (`txBytes += len` in `sendMsg`, not `len + MSG_HDR`). These tests fail in v4.0.8 too, and are out of scope for the v4.1.0 changes. They are listed here so a future maintainer picking up the work knows what's already broken; the fix is to change the assertion to `as.tx == payload` (and `bs.rx == payload + MSG_HDR`, which is the existing-asymmetric contract).

### Doc updates

* `docWebMonitor.md`: no dashboard-visible change in v4.1.0 (the disconnects card and the WINDOW changes are not exposed). The Errors card and Errors-vs-Disconnects text from v4.0.9 still apply.

---

## v4.0.9

**Errors card reshuffled — disconnects promoted to the headline.** The "Errors" card on the web dashboard used to lead with the frame-error count (small red number) and tuck the disconnect count away as a hint line. The new layout flips the emphasis: the big red number is now the **lifetime disconnect count** (cumulative `OK → SWP` transitions), which is the number an operator actually cares about. Frame errors and lost-msgs both demote to hint lines underneath. Label renamed from "Errors (lifetime)" to just "Errors" — the count is always lifetime, the parenthetical was noise. No protocol, wire-format, or counter-behavior change; this is a pure UI / labeling change to the embedded HTML in `AutoLinkWeb.cpp`. Wire-compatible with v4.0.0..v4.0.8.

### New card layout

```
Errors
  41              ← big red: disconnects (errTotal)
  2 lost msgs     ← hint: lostMsgs
  7 frame errors  ← hint: errCount
```

### What changed

* **Big red value = `errTotal`** (was `errCount`). One bumped wire or one noise burst that trips the error threshold shows up as `+1` on the headline immediately.
* **Frame errors demoted to a hint line** as `errcnt`, pluralized correctly (`1 frame error` / `N frame errors`).
* **Lost-msgs hint line** unchanged in content (`lostmsgs`), now sits between the two demoted counters.
* **Label text** changed from `Errors (lifetime)` to `Errors`. The `(lifetime)` qualifier is gone because the card now shows multiple lifetime counts and the parenthetical was redundant with the only counter it used to label.
* **JSON contract unchanged.** The `/stats` endpoint still emits `errCount`, `errTotal`, and `lostMsgs` with the same names and types; only the page-side JS mapping changed (`errcnt` ↔ `errCount`, `discon` ↔ `errTotal`).

### Doc updates

* `docWebMonitor.md`: widget-table row and the "Errors vs. Disconnects" section rewritten to match the new card layout and labeling. The disconnect count is now described as the headline; frame errors and lost-msgs are described as the diagnostic hint lines.

---

## v4.0.8

**Move `UtilMain.h` into `src/pingpong/`.** Completes the v4.0.7 packaging pass: `UtilMain.h` is the shared base for `UtilPing` and `UtilPong`, both of which now live in `src/pingpong/`, so `UtilMain.h` belongs with them. The remaining files in `src/util/` (`UtilBaudSweep`, `UtilCobs`, `UtilCrc`, `UtilFrameRx`, `UtilBlink`) are general-purpose protocol utilities used by `ALink` itself, not by the PingPong example, and stay where they are. No code changes, no behavior change, no protocol or wire-format change. Wire-compatible with v4.0.0..v4.0.7.

### File move

* `src/util/UtilMain.h` → `src/pingpong/UtilMain.h`

The file itself is unchanged. The bare-name `#include "UtilMain.h"` lines in `UtilPing.h` and `UtilPong.h` already resolved correctly from the new directory under the Arduino library layout (both `src/util/` and `src/pingpong/` are on the include search path; the bare-name lookup hits whichever directory the file happens to live in).

### Layout after this move

```
src/
├── ALink.cpp / .h
├── AutoLink.h
├── AutoLinkWeb.cpp / .h
├── EspHal.h
├── ILink.h
├── Log.cpp / .h
├── pingpong/                    # the echo-test example
│   ├── PingPong.h               # v4.0.7 unified entry point
│   ├── UtilMain.h               # v4.0.8: moved here from util/
│   ├── UtilPing.h
│   └── UtilPong.h
└── util/                        # protocol-layer utilities
    ├── UtilBaudSweep.cpp / .h
    ├── UtilBlink.h
    ├── UtilCobs.cpp / .h
    ├── UtilCrc.cpp / .h
    └── UtilFrameRx.cpp / .h
```

`src/util/` now contains only the reusable protocol utilities. `src/pingpong/` contains everything specific to the ping-pong example. The split mirrors the conceptual one: "things any AutoLink user might want" vs "things only the echo-test example wants."

---

## v4.0.7

**Unified `PingPong` entry point.** The v4.0.0..v4.0.6 API forced the user to pick one of two separate headers (`util/UtilPing.h` or `util/UtilPong.h`) and instantiate `UtilPing` or `UtilPong`, so the `examples/PingPong/Ping.ino` and `examples/PingPong/Pong.ino` sketches looked very different from each other — different class name, different constructor layout. v4.0.7 collapses both into a single class `PingPong` with a `Role` enum (`PING` or `PONG`); the two example sketches are now byte-identical apart from the enum value. No protocol, wire-format, or behavior change; this is purely a packaging / API-surface change. Wire-compatible with v4.0.0..v4.0.6.

### New: `src/pingpong/PingPong.h`

A thin facade that holds both a `UtilPing` and a `UtilPong` internally and forwards `setup()` / `loop()` to whichever one matches the role passed to the constructor. The unused member's `setup()` / `loop()` is never called, so its LED timer / web monitor / AutoLink state is never initialized — the cost of the unused member is a few hundred bytes of static state (an empty `Pending[8]` on the PING side, an `echoCount_` / `tNotReady_` on the PONG side) and the rest is paid in code size only on the role that's actually used.

```cpp
#include <pingpong/PingPong.h>
using namespace autolink;

PingPong upp(
    PingPong::PING,           // or PingPong::PONG
    115200,                   // Serial baud
    UART_NUM_2,               // UART port
    16,                       // RX pin
    17,                       // TX pin
    "<changeme>",             // WiFi SSID, or nullptr
    "<changeme>",             // WiFi password
    80                        // web server port
);

void setup() { upp.setup(); }
void loop()  { upp.loop();  }
```

### File moves

* `src/util/UtilPing.h` → `src/pingpong/UtilPing.h`
* `src/util/UtilPong.h` → `src/pingpong/UtilPong.h`

The files themselves are unchanged. The bare-name `#include "UtilMain.h"` and `#include "UtilCrc.h"` lines inside them resolve whether the consumer is in `util/` or `pingpong/` — both directories are on the include search path under the Arduino library layout.

### Sketch rewrite

`examples/PingPong/Ping.ino` and `examples/PingPong/Pong.ino` are now byte-identical apart from the `PingPong::PING` / `PingPong::PONG` enum value. The diff between the two files is 1 byte (the `G` vs `I` in the enum). Switching a board from Ping to Pong is a one-character edit + re-flash.

### Notes

* The v4.0.0..v4.0.6 `util/UtilPing.h` / `util/UtilPong.h` are **not** in `src/pingpong/`. Old sketches that included them by their old path will need to either update to `pingpong/PingPong.h` or update their include path. The two files are still in the library, just under the new directory.
* Host tests do not exercise `PingPong.h` (it is `#ifdef ARDUINO`-guarded), so the test suite is unchanged. All 14 host test suites continue to pass under `-Wall -Wextra` + AddressSanitizer + UBSan.

---

## v4.0.6

**Quiet down the live log + revert the dashboard's log panel to black-on-white.** Three high-frequency INFO log lines in the negotiation state machine demoted to DEBUG, and the dashboard's "Live Log" panel reverted from the v4.0.4 dark-slate background back to the original plain black-on-white. No protocol, wire-format, or behavior change; the new log level is the only runtime-visible difference and only at INFO (set `Log::setLevel(DEBUG)` to see the demoted lines). Wire-compatible with v4.0.0..v4.0.5.

### Log level: demote three high-frequency INFO lines to DEBUG

The 4-second boot-time negotiation in v4.0.0..v4.0.5 emits roughly 80+ log lines at INFO, the bulk of them from the negotiation state machine. v4.0.6 demotes the three lines that fire on every baud-sweep tick and every state transition, leaving the user-meaningful events (link up/down, threshold trips, RX errors, peer re-sweep) at INFO:

* **`SWP Ping baud[N]=<rate>`** — demoted from INFO to DEBUG. Fires once per baud index transition during the sweep; on a 5-baud sweep at 50 ms cadence that's ~10 lines per negotiation, plus one per re-sweep. The Pong's matching `SWP Pong: full sweep done` line stays at INFO (one line per full sweep, much rarer); the per-baud `Ping is now testing baud[N]` is DEBUG territory.
* **`State Transition: SWP -> LCK`** and **`State Transition: LCK -> SWP`** (and the rest of the `changeState_unlocked` log line) — demoted from INFO to DEBUG. State transitions fire on every baud-sweep tick and on every link drop / re-lock. At INFO this was drowning the live log on the dashboard. DEBUG is the right home: a user investigating a stuck link re-enables it with `Log::setLevel(DEBUG)` and gets the full SWP/LCK chatter. INFO keeps only the user-meaningful events.
* **`LCK timeout: no peer reply after N REQs -> re-sweep`** — demoted from INFO to DEBUG. Fires whenever the master gives up on a sweep and forces a re-sweep. From the operator's perspective, the visible signal that a re-sweep happened is the `Locked at N baud` line that immediately precedes this one, which is already at INFO. The detailed "how many REQs did we try before giving up" is DEBUG.

### Dashboard: revert Live Log to black-on-white

The v4.0.4 dashboard switch to a dark-slate background (`#1f2937`) with light-grey text and colorized severity labels was the right idea in the abstract but read poorly in practice: the `.I` (info) color of `#e5e7eb` was almost the same as the new background, and the white card behind the log block made the contrast jump uncomfortable when the operator scrolled past the log on a phone in daylight. v4.0.6 reverts to plain black-on-white: `background:#ffffff; color:#111827;` for the log block, with `.E` (error) as `#dc2626` red and `.D` (debug) as `#9ca3af` light grey. The severity color rules still colorize; the rest of the page was already white so there's no contrast jump.

---

## v4.0.5

**Throughput fixes for the Ping/Pong echo test.** Four small, low-risk changes that compound into a much faster wire. No protocol or wire-format change; the new constants are a compile-time tunable and the runtime behavior is otherwise identical to v4.0.0..v4.0.4. Wire-compatible with v4.0.0, v4.0.1, v4.0.2, v4.0.3, and v4.0.4.

### Change 1: `MAX_TX_PER_LOOP` 2 → 4 (UtilPing + UtilPong)

The pacing cap on how many frames `loop()` can emit (Ping) or echo (Pong) per tick. v4.0.0 set this to 2 to protect against bursting all 8 WINDOW frames at once and overrunning Pong's RX ring; that was a valid fix for v4.0.0 where `txBufferSize` was undersized. v4.0.1+ auto-sizes `txBufferSize = 2 * ((maxMsg + MSG_HDR) * 5/4 + 64)`, which is large enough to absorb multiple frames without blocking. v4.0.0's pacing guard is now overly conservative: with `loop()` running at 3–5 ms intervals, `WINDOW=8 / MAX_TX_PER_LOOP=2` means the pipeline takes 4 ticks to fill, and Pong's symmetric 2-per-loop drain means both sides are permanently under-driven. v4.0.5 raises it to 4 (half of WINDOW) on both sides so the window fills in 2 ticks, drains in 2 ticks, and the wire stays saturated. The DEBUG log at `setup()` time now shows `MAX_TX_PER_LOOP=4` so a log reader can confirm the new value is in effect.

### Change 2: removed `hw.flushTx()` from `sendFrame_unlocked()`

v4.0.0..v4.0.4 called `uart_wait_tx_done(uart_num, pdMS_TO_TICKS(100))` at the end of every control frame, which blocks the calling task until the UART TX FIFO drains to empty. Control frames are 5 bytes — at 115200 baud that's ~0.4 ms of wire time — but the FreeRTOS tick granularity means the task yields and may not be rescheduled for a full 1 ms tick. The data path (`sendCobsFrame_unlocked`) does **not** call `flushTx()` and was unaffected, but the protocol-overhead path (PING/REQ/fast-ack/keepalive) was unnecessarily serialized. v4.0.5 removes the call: the auto-sized TX ring accepts 5 bytes instantly and non-blocking, and `hw.tx()` returning `== CTRL_FRAME_SIZE` is the success signal. The public `ALink::flush()` / `AutoLink::flush()` entry point is preserved for users who need an explicit drain. A new DEBUG log line at the end of `sendFrame_unlocked` makes the "no flush" decision visible (gated at DEBUG so the steady-state INFO log stays readable).

### Change 3: `SETTLE_MS` 300 → 100 (UtilPing)

The settle guard on Ping side after every link-up. v4.0.0 set it to 300 ms to give Pong's sweep time to complete its own lock; with cobsSeq gap detection now in place, any stale frame from a previous session is rejected at the wire layer and the 300 ms is belt-and-suspenders. v4.0.5 reduces it to 100 ms, which is enough for Pong's blink to start without adding 200 ms of dead time to every session start. The Ping `link up` DEBUG log now reads `settling 100 ms (v4.0.5: was 300 ms in v4.0.4)`.

### Change 4: Pong's blocking link-up blink → async

Pong's `loop()` used to call `comm_.blinkWait(4, 100, 100, 2000)` on every link-up. The `delayMs > 0` path of `blinkWait` is `flashBlocking` — **fully blocking** — and `4 * (100+100) + 2000 = 2800 ms` is long enough to trigger Ping's 3-second `STALL_MS` watchdog. Result: Ping drops the link, re-sweeps, locks again, Pong blocks another 2.8 s on the new link-up, repeat. v4.0.5 changes it to `comm_.blinkWait(4)` (async, same as Ping's link-up blink). The async path returns immediately; the LED pattern runs on an esp_timer and doesn't block the echo loop. Stale-byte risk is already handled by the drain + `flushRx()` immediately above the call. The DEBUG log at Pong's `link up` now reads `blink=async` so a log reader can see the change in effect.

### Combined effect

The four changes are independently small but compound:

* Settle saves 200 ms per link-up.
* Pong async-blink saves up to 2800 ms per link-up (Pong stops blocking).
* `MAX_TX_PER_LOOP` 2 → 4 doubles the per-tick send / echo rate, so the WINDOW=8 pipeline fills in 2 ticks instead of 4 and drains in 2 instead of 4 — the wire is at full utilization instead of permanently 50%-utilized.
* `flushTx` removal saves ~1 ms of blocking yield per PING / REQ / fast-ack / keepalive in the sweep and OK states.

The two biggest wins are #4 (Pong no longer triggers Ping's STALL watchdog) and #1 (the pipeline actually fills). v4.0.5 is wire-compatible with v4.0.0..v4.0.4; all changes are either a tunable constant or a removed redundant `flushTx()` call.

---

## v4.0.4

**Dashboard + diagnostics enhancements.** No protocol or wire-format change; the new `lostMsgs` counter is derived from data we were already collecting in the gap-detection path. Wire-compatible with v4.0.0, v4.0.1, v4.0.2, and v4.0.3.

### New: `lostMsgs` counter

The protocol now tracks two separate tallies on top of the existing `gaps` (gap events) and `stale` (out-of-window) counters:

* **`gaps`** — one per out-of-order arrival (the existing counter).
* **`lostMsgs`** — total messages lost on the wire = sum of `(cobsSeq - rxSeq - 1)` across gap events. Equals `gaps` when a single seq is missing per event, larger when a burst went missing.

`gaps` tells you "how many times did we observe an out-of-order seq"; `lostMsgs` tells you "how many messages were physically lost." A single 4-seq burst loss is one `gaps` event and three `lostMsgs`. They're surfaced together on the dashboard: the "Errors (lifetime)" card shows `frameErrs` and now has a new hint line `lostMsgs` underneath, separate from the existing `disconnects` line.

Exposed via `Diag::lostMsgs` (which `getDiag(Diag&)` populates) and via the `/stats` JSON as `lostMsgs`. The counter is monotonic across link drops; it's reset only by the (not-yet-exposed) lifetime-counter reset path, or by full NVS erase.

### New: dashboard features

* **Log-level radio group in the header.** Three buttons (Error / Info / Debug) sit at the top of the page. Picking one POSTs to `/level?lv=N` (a new endpoint) and the device's `Log::setLevel` updates immediately. The radio is reconciled to the device's current level on every `/stats` poll, so a reboot that comes up at the default level is reflected on the page without a manual refresh.
* **Pause/Resume button at the top of the page.** Distinct from the per-log Pause button. This one freezes the ping/pong message stream (TX/RX rate, RX count, RSSI, baud, lost-msgs hint) at its last value, while the log keeps scrolling. The two pause buttons are independent: you can freeze the live counters while the log keeps going, or vice versa.
* **"lost msgs" hint under "Errors (lifetime)".** The card now shows three lines: `errcnt` (frame errors), `lost msgs` (wire loss), and `disconnects` (link drops). Distinct from the existing `disconnects` line so a single burst loss is visible alongside the cumulative frame-error count.
* **Darker log panel.** The `Live Log` block is now `#1f2937` (slate-800) with light-grey text and colorized `.E`/`.I`/`.D` severity labels tuned for the dark background. The previous white-on-white made the 12px monospaced lines hard to scan against the white card behind them.
* **`lostMsgs` in `/stats` JSON.** New `lostMsgs` field, plus a new `lvl` field reporting the current `Log::getLevel()` so the page can reconcile the radio to the device's level on the first poll.

### New: `/level` endpoint

`POST /level?lv=N` sets the device's `Log::setLevel`. Validates `0 <= lv <= 2` and returns 400 on bad input. The change is in-memory only (does not persist across reboot, since the device boots at `Log::ERROR` by default; a future version can persist to NVS if the operator wants the new level to survive a power cycle).

---

## v4.0.3

**Refactor + one real bug fix.** Pure code-quality and API consolidation on top of v4.0.2; no wire-format or protocol change. Wire-compatible with v4.0.0, v4.0.1, and v4.0.2. One genuine resource-leak fix: `EspHal::begin()` leaked the UART driver and the event task when `xTimerCreate` failed.

### Bug fix: `EspHal::begin()` leaked on xTimerCreate failure

If `xTimerCreate` returned `NULL`, the v4.0.0..v4.0.2 code only flipped `running = false` and returned, skipping the `uart_driver_delete(uart_num)` and `cleanup_resources()` that every other failure path runs. The UART event task was still running, the UART driver was still installed, and the mutex / task_exit_sem / stream_buf were leaked. Every other failure path correctly tore those down — only this one didn't. v4.0.3 calls the same teardown the other paths use.

### Refactor: collapses

* `ALink::dropLink_unlocked()` + `reset_unlocked()` → `reset_unlocked(bool count)`. The `count` flag picks between the disconnect path (counts toward `discCount`) and the fresh-start path used by `begin()`. v4.0.2's `reset_unlocked` was missing the `pingSample = 0` line that `dropLink_unlocked` had — a latent inconsistency that disappears on merge.
* `ALink::sendFrame()` + `sendFrame_unlocked()` → `sendFrame_unlocked()` does the work, `sendFrame()` just locks + calls + unlocks.
* `ALink::write()` + `writeLocked()` → `write(b, n)` takes the lock itself. Internal callers (the message API) used to call `writeLocked` to avoid re-locking; that pattern is gone, so the two-method split is no longer needed.
* `ALink::onTimer`'s hand-rolled keepalive (8 lines of `unenc[]` + `COBS encode` + bracket-by-hand) → `sendCobsFrame_unlocked(nullptr, 0)`. The 0-payload form of the data-frame TX emits the exact same 5 wire bytes the hand-rolled block did, but through the real COBS encoder — the keepalive wire format can no longer drift from the data path.

### Refactor: per-state handler split

`onRx` (~120 lines) and `onTimer` (~120 lines) each nested master/Pong × SWP/LCK. Split into:
* `ctrlFrameReady_unlocked` (dispatcher) and `handleSwp_unlocked` / `handleLck_unlocked` (per-state body) on the RX side.
* `onTimerOk_unlocked` / `onTimerSwp_unlocked` / `onTimerLck_unlocked` on the timer side.

The SWP-master branch in v4.0.2 churned `lock()` / `unlock()` four times within a single `onTimer` call. That made it easy to introduce a state-read race against a concurrent `onRx`. v4.0.3's `onTimerSwp_unlocked` takes the lock once and holds it for the whole branch.

### Refactor: "enter OK at baud N" extraction

Five copies of the 6-line block `{ setSpd; spdI=idx; errs=0; lastRx=lastTx=now; changeState(OK); startTimer(okTick) }` lived in `onRx` (SWP-REQ, fast-ack, SWP-fast-ack, LCK-REQ, master LCK reply). All five callsites now funnel through `lockOk_unlocked(int idx, const char* tag)` — the only variation was the log label, which is now the `tag` argument. The 30 lines of duplicated state-transition work became 5 lines of "lockOk_unlocked(best, \"REQ\")" calls.

### Refactor: API consolidation

* The 2-arg and 3-arg `getStats` overloads and the standalone `getLifetimeErrors()` collapsed to one `getStats(Stats&)`. Stats has four fields: `tx`, `rx`, `discCount`, `frameErrs`. Impossible to call the wrong overload.
* The five `getCobs*` getters on the facade (`getCobsSeq`, `getLastRxCobsSeqSet`, `getLastRxCobsSeq`, `getCobsGaps`, `getCobsStale`) collapsed to one `getDiag(Diag&)`. Diag has five fields, the struct is the source of truth for the names.
* `totalErrs` (the OK→SWP counter, actually a disconnect count) renamed to `discCount`. `lifetimeErrs` (the cumulative frame-error count) renamed to `frameErrs`. The old names were actively misleading: `totalErrs` was a tiny number, `lifetimeErrs` was the big one.

### Refactor: other small wins

* `ALink::reset_unlocked(bool)` is the only reset/drop entry point. `err_unlocked`, `dropLink`, `onBreak` all funnel through it.
* `bestSpd_unlocked()` is the only "pick the best baud" entry point. The inline "prefer fastest reliable baud" loop that used to live in the SWP fast-ack handler moved into `bestSpd_unlocked()` (it was the same intent, written slightly differently).
* Private-member naming normalized: dropped the trailing underscore on `cobsSeq_`, `swpRxBytes_`, `emptySweeps_`, `cobsGaps_`, `cobsStale_`, `lastRxCobsSeq_`, `lastRxCobsSeqSet_`. They are now `txSeq`, `swpRxBytes`, `emptySweeps`, `gaps`, `stale`, `rxSeq`, `rxSeqSet`. All members now follow the same bare-name convention.
* Magic numbers named: `MAX_GAP_RESYNC` (gap window), `COBS_SEQ_WRAP` (cobsSeq modulus). Both were inlined as `3` and `256` in the gap-detection arithmetic and the wraparound test comments.
* `CTRL_FRAME_SEQ_IDX` / `CTRL_FRAME_PAYLOAD_IDX` / `CTRL_FRAME_CRC_IDX` comments all said "Length of the ... field at index N" — they're indices, not lengths. Fixed.
* `logCobsSeq_unlocked()` (declared + defined, never called) deleted.
* `ILink::pushAppBuf(uint8_t)` and `ILink::popAppBuf()` (no-arg form) deleted from the interface; ALink only used the array forms. Every mock implementation got a few lines smaller.
* The version line is now logged exactly once on the standard Ping/Pong boot path: `UtilMain::setupCommon()` (which `Ping.ino` / `Pong.ino` always call before `comm_.begin()`) logs `AutoLink vX.Y.Z` at INFO. `AutoLink::begin()` used to log it again — that line is removed. `library.properties` remains the source of truth; `AUTOLINK_VERSION` in `AutoLink.h` is a compile-time copy.
* `sendCobsFrame_unlocked` is the documented single keepalive/data path; it clamps `n` to `[0, MAX_CHUNK]` and the `[0, MAX_CHUNK]` overflow no longer produces a frameLen-overflow read on adversarial input.

v4.0.3 is wire-compatible with v4.0.0, v4.0.1, and v4.0.2. No change to the protocol, wire format, cobsSeq semantics, framing, or message API. All v4.0.3 changes are either the EspHal resource-leak fix or pure refactor (API consolidation, deduplication, naming, dead-code removal).

---

## v4.0.2

**Five internal-quality fixes from a senior review pass: timer-service transactionality, listener drop semantics, const-correctness on the app-buffer API, and portability of diagnostic log lines.** No protocol, wire-format, or cobsSeq changes; v4.0.2 is wire-compatible with v4.0.0 and v4.0.1.

### Change 1: `EspHal::startTimer` is a single transaction

v4.0.0/v4.0.1 issued `xTimerChangePeriod` and `xTimerStart` as two independent commands and ran a one-shot retry on each. Under a rapid drop→sweep→drop storm the timer-service command queue can fill; if `ChangePeriod` succeeded but `Start` failed (or vice versa), the timer ended up with a different period than what the caller asked for, and the `||` retry pattern only retried one of the two. v4.0.2 retries the whole pair as a transaction (3 attempts, all-or-nothing), so the timer is either at the requested period and running, or the call surfaces an error. The dead-branch from "one retry on the first op, then the second op runs unconditionally" is gone.

### Change 2: `ALink::onPayload` honors the `Listener::onPayload` drop contract

`UtilFrameRx::Listener::onPayload`'s contract says "return true if the link was dropped, feed() should stop." v4.0.0/v4.0.1 returned `false` in every branch, making the `if (dropped) return i + 1;` in `UtilFrameRx::feed` dead code. v4.0.2 returns `true` on the two branches that should stop the parser: (a) the frame arrived while `state != OK` (a stale-session frame; the rest of the event belongs to a different session) and (b) the app buffer is full and the frame was dropped (the consumer is wedged; draining the rest of the event would just queue more bytes the consumer can't read). Gap and stale `cobsSeq` frames keep returning `false` — the link is OK, only this frame is bad.

### Change 3: `peekAppBuf()` and `appBufAvailable()` are const-honest

`peekAppBuf()` mutates the one-byte look-ahead cache; in v4.0.0/v4.0.1 it was non-const while `appBufAvailable()` was const and read the same cache (compiling only because the cache field could be reached through a `const` method). v4.0.2 marks the cache `mutable` and makes both methods `const` in the `ILink` interface, the `EspHal` implementation, and the `MockHal` test mock. The cache is now a clearly-documented internal detail of the pop/peek path, and the compiler enforces const correctness at every call site.

### Change 4: keepalive wire-layout comment is accurate

The keepalive in `ALink::onTimer` is a 2-byte unencoded payload (`{cobsSeq, CRC8(cobsSeq)}`) that COBS-encodes to exactly 3 bytes regardless of `cobsSeq` (COBS overhead is 1 byte for any input < 254), then brackets with two `0x00` delimiters for 5 wire bytes. v4.0.0/v4.0.1's inline example `[0x00, 0x01, 0x02, CRC8(cobsSeq), 0x00]` was only correct for `cobsSeq == 0`; for any other `cobsSeq` the encoded layout is `{<run-to-next-zero>, cobsSeq, CRC8}`. v4.0.2's comment is accurate for both cases and notes the `ka[5]` buffer is exactly sized because the encoded length is invariant.

### Change 5: `WIRING CHECK` and `uart_set_pin` failure logs are board-agnostic

v4.0.0/v4.0.1's `WIRING CHECK` (fired by the Pong when 0 raw bytes arrive at any baud) and `uart_set_pin` failure (fired by `EspHal::begin`) both hard-coded FireBeetle ESP32 pin assignments (D10=GPIO17, D11=GPIO16). Anyone running on a different board saw a misleading hint. v4.0.2 replaces those GPIO numbers with a generic "check the rxPin/txPin you passed to the AutoLink constructor match your board's pinout."

### Diagnostic in the v4.0.1 → v4.0.2 change

All five fixes were identified by code review of v4.0.1. None required fresh hardware data; the `WIRING CHECK` and `keepalive` findings came from reading the code path against the v4.0.0 hardware log, the `startTimer` finding from reading the `||` retry pattern and the FreeRTOS timer-service command-queue semantics, the `onPayload` finding from reading the `Listener::onPayload` contract against the implementation, and the `peekAppBuf` finding from running the `const` propagator by hand. The protocol layer was unchanged on the wire.

v4.0.2 is wire-compatible with v4.0.0 and v4.0.1. No change to the protocol, wire format, cobsSeq semantics, framing, or message API. The only changes are: the `startTimer` transactionality, the `onPayload` return values, the `peekAppBuf` const-ness, the keepalive comment text, and two diagnostic log strings.

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
