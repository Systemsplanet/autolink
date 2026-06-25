# AGENTS.md

Read this, then `README.md`, then `docs/Version.md`. History is the source of truth.

## Layout

- `src/`, `include/`, `examples/`, `library.properties`, `idf_component.yml`, `docs/`, `build/` — the library.
- `test/test_desktop/` — host unit tests, subsecond.
- `test/test_embedded/` — reserved, empty (see its README).
- `test/itest/test_desktop/` — host integration (two-Link loopback).
- `test/itest/test_embedded/` — Arduino-sketch integration, cross-compiled.
- `test/common/` — shared (`MockHal.h`, `WireSim.h`, `peak_rss.py`).
- `test/Makefile` — `test`, `itest`, `all`, `clean`.
- `build/build_env.sh` — installs `arduino-cli` + esp32 toolchain.
- `build/verify_build.sh` — cross-compile sketch. Run before declaring done. Host tests skip `AutoLinkWeb.cpp`.
- `build/arduino-cli-cmd.sh` — wrapper. Use it; never bare `arduino-cli`.
- `build/pretty_print.py` — `clang-format -i` + legacy string-merge. Run on every `.cpp`/`.h` you change. Self-tests: `build/test_pretty_print.py`.
- `build/version.py` — owns `docs/Version.md` structural invariant. Subcommands: `add`, `trim --keep 20`, `check` (CI gate).
- `build/clean.py` — owns filesystem cleanup of build artifacts. Nothing else deletes test binaries.

## Rules

**Process**

1. Read `README.md`, this file, `docs/Version.md` first.
2. Push back once on requests that break the wire format without a major bump, remove an ACK/retransmit path, hide a failure mode, ship something board-specific, or trade a real fix for cosmetic cleanup. Then do what's asked.
3. Bump `AUTOLINK_VERSION` in `include/AutoLink.h` AND `version=` in `library.properties` in lockstep. The version is the contract.
4. Verify before done: `cd test && make test`, `make itest`, `./build/verify_build.sh`. For protocol changes (`Link.cpp`), also `make loopback`.
5. Update `docs/Version.md` (newest on top, one-line summary + fix + regression test + limitations). `build/version.py add --version <X.Y.Z> --title "..."` scaffolds; `trim --keep 20` enforces the cap. `check` is the pre-zip gate.
6. README example code must match `examples/` and `src/`. Diff line-by-line before zipping. A non-compiling snippet is a 100% reproducible install failure.
7. Never delete `build/`. It hosts `verify_build.sh`, `arduino-cli-cmd.sh`, `pretty_print.py`, `test_pretty_print.py`, and the verify sketch. If missing, restore from git or the last good zip before anything else.

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

11. Strict comment policy. Rarely add comments. Only if a very senior dev would need them. *Why*, *which side of the wire*, *which test pins this* — nothing else. No historical anchors ("this used to do X"). A senior dev reads the type and the function name.
12. No version references in code or scripts. No hard-coded `X.Y.Z` in `.cpp`/`.h`/`.ino`/`.py`/`.sh`/`.mk`/`.yml`/`.json`/`.txt`/non-Version `.md`. The only legitimate reference in code is `AUTOLINK_VERSION` (or equivalent) being read and displayed. Test fixtures, docstrings, help text, CLI examples, regression banners — all out. The version lives in `library.properties` / `idf_component.yml` / `docs/Version.md`.
13. Short names. `b`, `n`, `i`, `j`, `e`, `ok`, `lv`, `seq`, `cb`. Not `m_messageBufferLength`.
14. Composition over inheritance. Function-pointer callbacks or `unique_ptr<Interface>` over virtual bases. Saves a vtable per Link (~32B on ESP32). Inheritance only at the user-extension boundary (`IHal`).
15. One concern per tool. A script that mixes "delete artifacts", "compute manifest", and "diff against reference" is brittle. When adding a tool, give it one concern. If it grows to two, split before three.
16. Logging: version at startup, wire-op results (`ack received`, `retransmit N`), state-change causes (`link dropped: 30 s idle`), error resolutions. Never hot-path chatter.

**RTOS / embedded**

17. Never do RTOS work in a ctor that may run before the scheduler. Namespace-scope `PingPong upp(...);` is hoisted into `.init_array` and runs before `app_main`/`setup()`. `xSemaphoreCreateMutex`, `xStreamBufferCreate`, `xTaskCreate`, `xTimerCreate`, even `malloc` can crash the kernel. Safe shapes: Meyers singleton via `getInstance()`, or store config in the ctor and allocate RTOS primitives in `begin()`.

**Testing**

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

## Stuck

- Compile error in `Link.cpp` on Arduino-ESP32 → check `freertos/FreeRTOS.h` is at file scope.
- Linker error about a `util/` symbol → `--libraries` (plural). Use `--library`.
- Linker error in production but not in tests → host tests skip `AutoLinkWeb.cpp`. Cross-compile.
- Dashboard times out → re-check `portYIELD()` at end of `Link::sendMsg` and that the httpd task is at lower priority than loopTask.
- Dashboard renders wrong → `make test_dashboard_js` in `test/test_desktop/`.
- Toggle test doesn't fail when fix is reverted → the test doesn't pin the bug. Rewrite.
