# AGENTS.md

Read this, then `README.md`, then `docs/Version.md`. History is the source of truth.

This file is project-specific operational rules. For the general engineering
philosophy behind the code-style and testing rules below (deep modules,
composition over inheritance, one concern per unit, test-through-interfaces,
red-loop-first debugging), see `docs/developer.md`. The rules here are how those
principles land on *this* codebase; `docs/developer.md` is the *why*.

## Layout

- `src/`, `include/`, `examples/`, `library.properties`, `idf_component.yml`, `docs/`, `build/` — the library.
  - Every package (a directory of `.cpp`/`.h` files) stays at or under 7 files and every file stays under 15 KB — split into a subdirectory by concern rather than growing a flat directory or a single file past either limit. `src/al/link/` is the reference example: `core` split into `LinkCore.cpp`/`LinkApi.cpp`/`Link.h`/`LinkWire.h`/`IHalCtx.h`/`ISweepCtx.h` at the root, with `io/` (frame/message codec), `timers/` (OK-state + sweep timer halves), `arq/`, and `sweep/` as siblings.
  - `src/al/web/` splits the same way: hand-written code at the root plus `handlers/` (httpd route implementations), `assets/` (committed CSS/HTML/JS source, edited by hand), and `generated/` (seven small per-part headers built by `build/dashboard_assets.py` — never hand-edited, never one large combined file).
- `test/test_desktop/` — host unit tests, subsecond. Mirrors `src/al/<mod>/` one level deeper: `al/link/gbn/`, `al/link/health/`, `al/link/message/`, `al/link/frame/`, `al/link/ack/`, `al/link/async/`, `al/link/timer/`, `al/link/misc/`, `al/link/arq/`, `al/link/sweep/` (+ `sweep/guard/`) group by test concern rather than 1:1 with a source file, since one source concern often has many pins. `al/web/` splits into the root (dashboard/routing tests) and `httpd/` (lifecycle/startup/retry tests); `al/` itself splits into `facade/` (runtime AutoLink-level tests) and `meta/` (structural/source-grep tests); `al/pingpong/` splits into the root and `ping/`. `dashboard-js/` holds the jsdom-based dashboard JS tests, run via `make test_dashboard_js`.
- `test/test_embedded/` — reserved, empty (see its README).
- `test/itest/test_desktop/` — host integration (two-Link loopback).
- `test/itest/test_embedded/` — Arduino-sketch integration, cross-compiled.
- `test/common/` — shared fixtures (`MockHal.h`, `WireSim.h`, `TestCfg.h`, `NullArqCache.h`, `EspHalStub.h`) plus `accessors/` (`AutoLinkTestAccessor.h`, `LinkTestAccessor.h` — the friend shims into `AutoLink`/`Link` internals). `accessors/` is on the include path via `-I../common/accessors` in both test Makefiles, so every `#include "MockHal.h"`-style bare include still resolves without a path prefix.
- `test/scripts/` — all test-side scripts, grouped by role:
  - `common/` — helpers shared across unit + integration (`peak_rss.py`, `summarize.py`).
  - `coverage/` — gcov coverage pipeline (`coverage_manifest.py`, `coverage_merge.sh`, `test_coverage_manifest.py`).
  - `env/` — host test environment setup (`install_system_stubs.py`, `arduino_stub_template.h`).
- `test/Makefile` — `test`, `itest`, `all`, `clean`.
- `build/build_env.sh` — installs `arduino-cli` + esp32 toolchain.
- `build/verify_build.sh` — cross-compile sketch. Run before declaring done. Host tests skip `src/al/web/handlers/*.cpp` (the four `#ifdef ARDUINO` httpd TUs) and `src/al/hal/EspHal.cpp`.
- `build/check_arduino_iface.sh` — ArduinoDroid sketch-TU flag-drop gate AND link-stage library-deps gate. Five phases: (1) standard happy-path compile via arduino-cli, (2) sketch-TU flag-drop simulation (catches the regression where the sketch TU compiles with no g++ flags and every `#ifdef ARDUINO` block in the public headers goes dark), (3) self-test on a sandboxed broken shim (proves the gate fires), (4) static source-grep pin on `library.properties` `depends=` + web TU `#include <FS.h>` (catches the ArduinoDroid link-stage bug where `fs::FS` / `fs::File` / `VFSImpl` symbols go missing because the IDE doesn't auto-resolve transitive library deps), (5) arduino-cli end-to-end link smoke test for AutoLinkWeb + LittleFS. Exit codes: 0 pass, 1 infra, 2 header-guard regression, 3 unrelated compile/link error, 4 self-test failure, 5 link-stage library-deps regression.
- `build/arduino-cli-cmd.sh` — wrapper. Use it; never bare `arduino-cli`.
- `build/pretty_print.py` — `clang-format -i` + legacy string-merge. Run on every `.cpp`/`.h` you change. Self-tests: `build/pretty_print-test.py`.
- `build/dashboard_assets.py` — reads `src/al/web/assets/` (committed) and writes the seven small generated headers under `src/al/web/generated/` (build output, not committed source in spirit even though it ships in the zip — regenerate rather than hand-edit). `--check` mode fails if any output is stale; run via the test Makefile's `dashboard_assets_regen` / `assets_check` targets.
- `build/version.py` — owns `docs/Version.md` structural invariant. Subcommands: `add`, `trim --keep 20`, `check` (CI gate).
- `build/pre_zip_check.sh` — pre-zip gate. Asserts the deliverable carries `AGENTS.md`, `README.md`, `docs/Version.md`, `docs/todo.md`. `--zip <path.zip>` checks the **archive** (load-bearing: a complete staging root can still ship a zip that lost a file to a stray `zip -x` glob); a bare path checks a staging root. Self-test: `build/pre_zip_check-test.sh`.
- `build/clean.py` — owns filesystem cleanup of build artifacts. Nothing else deletes test binaries.

## Rules

**Process**

1. Read `README.md`, this file, `docs/Version.md` first.
2. Push back once on requests that break the wire format without a major bump, remove an ACK/retransmit path, hide a failure mode, ship something board-specific, or trade a real fix for cosmetic cleanup. Then do what's asked.
3. Bump `AUTOLINK_VERSION` in `include/AutoLink.h` AND `version=` in `library.properties` in lockstep. The version is the contract.
4. Verify before done, in this order:
   ```
   cd test && make test && make itest      # allow >= 60 s for itest; the CI 30 s ceiling truncates it
   ./build/verify_build.sh                 # allow 600 s on a cold cache, 300 s once esp32:esp32 is installed
   bash build/check_arduino_iface.sh
   bash build/pre_zip_check-test.sh        # proves the pre-zip gate fires
   ```
   For protocol changes (`Link*.cpp`), also `make loopback`.
   The ESP32 cross-compile is **not optional on any change**: host tests skip
   the four `#ifdef ARDUINO` httpd TUs under `src/al/web/handlers/`,
   `src/al/hal/EspHal.cpp`, and every API surface that only exists in the
   Arduino build path. `check_arduino_iface.sh` is separate from it because `arduino-cli` always
   passes the full flag list and therefore cannot trigger the ArduinoDroid
   flag-drop bug class on its own. If the sandbox truncates a gate, treat it as
   unverified, say so in the delivery summary, and re-run somewhere longer-lived.

5. Update `docs/Version.md` (newest on top, one-line summary + fix + regression test + limitations). `build/version.py add --version <X.Y.Z> --title "..."` scaffolds; `trim --keep 50` enforces the cap. `check` is the pre-zip gate.
   - Never add anything to `todo.md` that an AI can't do. `todo.md` is the actionable queue for the next coding pass; every item must be completable in this environment (source edits, host tests, cross-compile in a network-capable sandbox). Work that requires physical hardware — FireBeetle bench runs, real-UART @ 512000 validation, scope traces — does NOT belong in `todo.md`; record it in the current `docs/Version.md` entry's disclosed limitations instead.
6. README example code must match `examples/` and `src/`. Diff line-by-line before zipping. A non-compiling snippet is a 100% reproducible install failure.
7. Never delete `build/`. It hosts `verify_build.sh`, `arduino-cli-cmd.sh`, `pretty_print.py`, `pretty_print-test.py`, `pre_zip_check.sh`, `pre_zip_check-test.sh`, and the verify sketch. If missing, restore from git or the last good zip before anything else.

**Delivery**

8. The zip is the deliverable. Stage flat at the archive root (`cd staging && zip -r -y /path.zip .`). No wrapper folder. Strip `*.o`, `*.bak`, `run_test_*`, `run_loopback*`, `*.gcno`/`*.gcda`, `node_modules/`, `compile_commands.json`. Keep `.cpp`, `.h`, `.ino`, `.md`, `.properties`, `.txt`, `build/`. Normalize perms: dirs 0755, files 0644, scripts 0755, `library.properties` 0644. Smoke-test by extracting into a fresh dir and running `make test`.
   **Mandatory:** `bash build/pre_zip_check.sh --zip <path.zip>` after staging. Exit 1 = the zip is rejected; do not deliver.
9. Diff the zip's file list against a known-good reference before declaring done. A non-empty "lost" set means a file the user depends on is missing even if the cross-compile passed:
   ```
   unzip -l <ref>.zip | awk 'NR>3 {print $NF}' | sort > /tmp/old.txt
   unzip -l <new>.zip | awk 'NR>3 {print $NF}' | sort > /tmp/new.txt
   comm -23 /tmp/old.txt /tmp/new.txt   # lost
   comm -13 /tmp/old.txt /tmp/new.txt   # added
   ```
10. Surface the zip in `<deliver-assets>` in the final response, with unit and itest totals and wall time (ms or s) reported separately and combined.

**Code style**

11. Strict comment policy (principle: `docs/developer.md`). Rarely add comments. Only if a very senior dev would need them. *Why*, *which side of the wire*, *which test pins this* — nothing else. No historical anchors ("this used to do X"). A senior dev reads the type and the function name.
12. No version references in code or scripts. No hard-coded `X.Y.Z` in `.cpp`/`.h`/`.ino`/`.py`/`.sh`/`.mk`/`.yml`/`.json`/`.txt`/non-Version `.md`. The only legitimate reference in code is `AUTOLINK_VERSION` (or equivalent) being read and displayed. Test fixtures, docstrings, help text, CLI examples, regression banners — all out. The version lives in `library.properties` / `idf_component.yml` / `docs/Version.md`.
13. Short names (principle: `docs/developer.md`). This codebase's vocabulary: `b`, `n`, `i`, `j`, `e`, `ok`, `lv`, `seq`, `cb`. Not `m_messageBufferLength`.
14. Composition over inheritance (principle: `docs/developer.md`). Here that means: function-pointer callbacks or `unique_ptr<Interface>` over virtual bases — saves a vtable per Link (~32B on ESP32) — and inheritance only at the user-extension boundary (`IHal`).
15. One concern per tool (principle: `docs/developer.md`). Concretely: a script that mixes "delete artifacts", "compute manifest", and "diff against reference" is brittle. New tools get one concern; split before a second grows into a third.
16. Logging: version at startup, wire-op results (`ack received`, `retransmit N`), state-change causes (`link dropped: 30 s idle`), error resolutions. Never hot-path chatter.

**RTOS / embedded**

17. Never do RTOS work in a ctor that may run before the scheduler. Namespace-scope `PingPong upp(...);` is hoisted into `.init_array` and runs before `app_main`/`setup()`. `xSemaphoreCreateMutex`, `xStreamBufferCreate`, `xTaskCreate`, `xTimerCreate`, even `malloc` can crash the kernel. Safe shapes: Meyers singleton via `getInstance()`, or store config in the ctor and allocate RTOS primitives in `begin()`.

**Testing**

(Test philosophy — through-interfaces, regression-pins, pure-logic-vs-I/O, injectable time, subsecond units — is in `docs/developer.md`. The rules below are how it maps onto this suite.)

18. Every fix gets a regression test that fails when the fix is reverted. Toggle off → red. Toggle on → green. Green/green means the test is useless. Don't write tests that discard the return value.
19. Unit tests are small, subsecond, and live under `test/test_desktop/`. No `sleep()`, no wall-clock busy-waits. Time-dependent behaviour via `MockHal::pumpClock` / `runFor`. Anything that can't stay subsecond goes under `test/itest/`.
20. Test files mirror the source package one level deeper: `test/test_desktop/al/<module>/<concern>/<Source>Test.cpp`. Not always 1:1 with a single source file — one source concern (e.g. `src/al/link/timers/`) often has enough pins that they split by *test* concern (`gbn/`, `health/`, `timer/`) rather than by source file, and structural/meta tests (`CompileCheckTest`, `VersionFreeSourceTest`, ...) live in their own `meta/` next to the runtime tests in `facade/`. Documented exception: `al/pingpong/` (`Ping`/`Pong` are `#ifdef ARDUINO`; loopback tests in `al/link/` exercise the same protocol).
20a. Every package (a directory of source or test files) stays at or under 7 files; every `.cpp`/`.h` file stays under 15 KB. When a directory or file would cross either limit, split into a subdirectory by concern — see `src/al/link/` and its test mirror for the reference shape. `test/common/`'s shared fixtures reach a subdirectory (`accessors/`) via an extra `-I` search path in both test Makefiles rather than qualifying every `#include` — prefer that shape when the alternative is rewriting a large number of bare includes across many files.
21. Pure decision logic separated from I/O. Protocol decisions are pure free functions returning enums (`decideArqSlot(ageMs, retxCount, ackRtoMs, maxRetx)`). Side effects in the caller. Table-tested exhaustively.
22. Time and scheduling are injectable. Idle-timeout, ACK RTO, SWP/LCK stall reached via `pumpClock` / `runFor`. Any `while(!deadline)` poll bypasses the host suite.
23. One owner for in-flight state. Protocol owns seq stamps; facade owns payload storage, sequenced by the same mutex. Cache hooks fire inside the link lock. No translation tables.
24. Cheap invariant checks in the host debug build. After every public facade call that mutates state, assert cross-checks under `#ifdef AUTOLINK_HOST_TEST`. Turn "fails silently after 3 drops" into "aborts on the drop that broke the invariant".

## Gotchas

- `portYIELD()` needs `<freertos/FreeRTOS.h>` at file scope, not inside `namespace autolink { ... }`.
- `arduino-cli --library` (singular). The plural flag doesn't recurse.
- `AutoLinkWeb.cpp` isn't covered by host tests. Cross-compile to verify any change.
- `AutoLink` ctor first arg is `uart_port_t`, not `int`.
- `Log::log().info(...)` — singleton accessor.
- `Stats` has `tx`, `rx`, `discCount`, `frameErrs`. No `txBps`/`rxBps` (computed in dashboard JSON).
- `make -f build/Makefile clean` runs `build/clean.py --apply` then per-dir `make clean`. Pass `CLEAN_FLAGS="--root /path"` to point it at a fresh extracted zip; `CLEAN_FLAGS=""` for dry-run.
- Generated headers (`src/al/web/generated/*.h`, seven files) are the byte output of `build/dashboard_assets.py` and are NOT clang-format targets. `build/pretty_print.py` skips them by path prefix (see `_GENERATED_HEADERS`); a new generated file under `src/al/web/generated/` is exempted automatically. `make test` auto-regens them from `src/al/web/assets/`; `make assets_check` (CI gate) verifies the committed copies are current.

## Stuck

- Compile error in `Link.cpp` on Arduino-ESP32 → check `freertos/FreeRTOS.h` is at file scope.
- Linker error about a `util/` symbol → `--libraries` (plural). Use `--library`.
- Linker error in production but not in tests → host tests skip `AutoLinkWeb.cpp`. Cross-compile.
- Dashboard times out → re-check `portYIELD()` at end of `Link::sendMsg` and that the httpd task is at lower priority than loopTask.
- Dashboard renders wrong → `make test_dashboard_js` in `test/test_desktop/`.
- Toggle test doesn't fail when fix is reverted → the test doesn't pin the bug. Rewrite.
