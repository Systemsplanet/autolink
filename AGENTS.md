# AGENTS.md

Read this, then `README.md`, then `docs/Version.md`. History is the source of truth.

This file is project-specific operational rules. For the general engineering
philosophy behind the code-style and testing rules below (deep modules,
composition over inheritance, one concern per unit, test-through-interfaces,
red-loop-first debugging), see `docs/developer.md`. The rules here are how those
principles land on *this* codebase; `docs/developer.md` is the *why*.

## Layout

- `src/`, `include/`, `examples/`, `library.properties`, `idf_component.yml`, `docs/`, `build/` — the library.
- `test/test_desktop/` — host unit tests, subsecond.
- `test/test_embedded/` — reserved, empty (see its README).
- `test/itest/test_desktop/` — host integration (two-Link loopback).
- `test/itest/test_embedded/` — Arduino-sketch integration, cross-compiled.
- `test/common/` — shared (`MockHal.h`, `WireSim.h`).
- `test/scripts/` — all test-side scripts, grouped by role:
  - `common/` — helpers shared across unit + integration (`peak_rss.py`, `summarize.py`).
  - `coverage/` — gcov coverage pipeline (`coverage_manifest.py`, `coverage_merge.sh`, `test_coverage_manifest.py`).
  - `env/` — host test environment setup (`install_system_stubs.py`, `arduino_stub_template.h`).
- `test/Makefile` — `test`, `itest`, `all`, `clean`.
- `build/build_env.sh` — installs `arduino-cli` + esp32 toolchain.
- `build/verify_build.sh` — cross-compile sketch. Run before declaring done. Host tests skip `AutoLinkWeb.cpp`.
- `build/check_arduino_iface.sh` — ArduinoDroid sketch-TU flag-drop gate AND link-stage library-deps gate. Five phases: (1) standard happy-path compile via arduino-cli, (2) sketch-TU flag-drop simulation (catches the regression where the sketch TU compiles with no g++ flags and every `#ifdef ARDUINO` block in the public headers goes dark), (3) self-test on a sandboxed broken shim (proves the gate fires), (4) static source-grep pin on `library.properties` `depends=` + web TU `#include <FS.h>` (catches the ArduinoDroid link-stage bug where `fs::FS` / `fs::File` / `VFSImpl` symbols go missing because the IDE doesn't auto-resolve transitive library deps), (5) arduino-cli end-to-end link smoke test for AutoLinkWeb + LittleFS. Exit codes: 0 pass, 1 infra, 2 header-guard regression, 3 unrelated compile/link error, 4 self-test failure, 5 link-stage library-deps regression.
- `build/arduino-cli-cmd.sh` — wrapper. Use it; never bare `arduino-cli`.
- `build/pretty_print.py` — `clang-format -i` + legacy string-merge. Run on every `.cpp`/`.h` you change. Self-tests: `build/pretty_print-test.py`.
- `build/version.py` — owns `docs/Version.md` structural invariant. Subcommands: `add`, `trim --keep 20`, `check` (CI gate).
- `build/clean.py` — owns filesystem cleanup of build artifacts. Nothing else deletes test binaries.

## Rules

**Process**

1. Read `README.md`, this file, `docs/Version.md` first.
2. Push back once on requests that break the wire format without a major bump, remove an ACK/retransmit path, hide a failure mode, ship something board-specific, or trade a real fix for cosmetic cleanup. Then do what's asked.
3. Bump `AUTOLINK_VERSION` in `include/AutoLink.h` AND `version=` in `library.properties` in lockstep. The version is the contract.
4. Verify before done: `cd test && make test`, `make itest`, `./build/verify_build.sh`, `bash build/check_arduino_iface.sh`. For protocol changes (`Link.cpp`), also `make loopback`. Allow at least a 60 s timeout for `make itest` — the host integration suite takes seconds of wall time and the sandbox / CI 30 s ceiling truncates the run before completion. The ESP32 cross-compile (`build/verify_build.sh`) is not optional: it must run on every change, not just protocol changes, because host tests skip `AutoLinkWeb.cpp` and any library API surface that only manifests in the Arduino build path. The wrapper `build/arduino-cli-cmd.sh` installs `esp32:esp32` on first use; that install is a one-time multi-minute cost (large toolchain download + board index), so give `verify_build.sh` at least a 10 minute timeout (600 s) on a clean cache and 5 minutes (300 s) once the core is installed. If the sandbox truncates below that, treat the build as unverified, surface it in the delivery summary, and re-run in a longer-lived environment. The ArduinoDroid sketch-TU flag-drop gate (`build/check_arduino_iface.sh`) is the negative test for the regression where the sketch TU compiles with no g++ flags and every `#ifdef ARDUINO` block in the public headers goes dark — it is independent of `verify_build.sh` because `arduino-cli` always passes the full flag list, so verify_build alone cannot trigger this bug class.
5. Update `docs/Version.md` (newest on top, one-line summary + fix + regression test + limitations). `build/version.py add --version <X.Y.Z> --title "..."` scaffolds; `trim --keep 20` enforces the cap. `check` is the pre-zip gate.
6. README example code must match `examples/` and `src/`. Diff line-by-line before zipping. A non-compiling snippet is a 100% reproducible install failure.
7. Never delete `build/`. It hosts `verify_build.sh`, `arduino-cli-cmd.sh`, `pretty_print.py`, `pretty_print-test.py`, and the verify sketch. If missing, restore from git or the last good zip before anything else.

**Delivery**

8. The zip is the deliverable. Stage flat at the archive root (`cd staging && zip -r -y /path.zip .`). No wrapper folder. Strip `*.o`, `*.bak`, `run_test_*`, `run_loopback*`, `*.gcno`/`*.gcda`, `node_modules/`, `compile_commands.json`. Keep `.cpp`, `.h`, `.ino`, `.md`, `.properties`, `.txt`, `build/`. Normalize perms: dirs 0755, files 0644, scripts 0755, `library.properties` 0644. Smoke-test by extracting into a fresh dir and running `make test`.
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
20. Test files mirror the source package: `test/test_desktop/al/<module>/<Source>Test.cpp` for `src/al/<module>/<Source>.cpp`. Documented exception: `al/pingpong/` (`Ping`/`Pong` are `#ifdef ARDUINO`; loopback tests in `al/link/` exercise the same protocol).
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
- Generated headers (`src/al/web/AutoLinkWebHtml.h`) are the byte output of `build/dashboard_assets.py` and are NOT clang-format targets. `build/pretty_print.py` skips them by path suffix (see `_GENERATED_HEADERS`); adding a new generated header requires adding it to that tuple. `make test` auto-regens this header from sources; `make assets_check` (CI gate) verifies the committed copy is current.

## Stuck

- Compile error in `Link.cpp` on Arduino-ESP32 → check `freertos/FreeRTOS.h` is at file scope.
- Linker error about a `util/` symbol → `--libraries` (plural). Use `--library`.
- Linker error in production but not in tests → host tests skip `AutoLinkWeb.cpp`. Cross-compile.
- Dashboard times out → re-check `portYIELD()` at end of `Link::sendMsg` and that the httpd task is at lower priority than loopTask.
- Dashboard renders wrong → `make test_dashboard_js` in `test/test_desktop/`.
- Toggle test doesn't fail when fix is reverted → the test doesn't pin the bug. Rewrite.
