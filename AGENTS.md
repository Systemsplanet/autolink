# AGENTS.md — Working With AutoLink

Read this file first, then `README.md` (Document Index), then
`docs/Version.md` for what was already tried, what failed, and
what the user rejected. That history is the source of truth.

## Project shape

- `src/`, `include/`, `examples/`, `library.properties`,
  `idf_component.yml`, `docs/`, `build/` — the library.
- `test/`
  - `test_desktop/` — unit tests (host, subsecond).
  - `test_embedded/` — reserved for embedded-target unit
    tests; currently empty (see its `README.md`).
  - `itest/test_desktop/` — host integration tests
    (two-Link loopback, noise).
  - `itest/test_embedded/` — Arduino-sketch integration
    test cross-compiled against the real ESP32 toolchain.
  - `common/` — shared infrastructure (`MockHal.h`,
    `WireSim.h`, `peak_rss.py`, `summarize.py`) on the
    include path of both unit and itest Makefiles.
  - `Makefile` — `make test` (unit), `make itest`
    (host integration), `make all` (both + combined
    summary), `make clean`.
- `build/`
  - `build_env.sh` — installs `arduino-cli` + `esp32:esp32@3.3.5`.
  - `verify_build.sh` — cross-compiles
    `build/verify_build/verify_build.ino`. **Always run this
    before declaring done.** Host tests do NOT compile
    `AutoLinkWeb.cpp`.
  - `arduino-cli-cmd.sh` — wrapper for the toolchain.
  - `pretty_print.py` — the single canonical pretty-printer
    for the project. Wraps `clang-format -i` plus a
    one-time string-merge pass for legacy string-literal
    splits. Run it on every `.cpp` / `.h` you change. See
    `build/test_pretty_print.py` for its self-tests.

## Hard rules

1. **Push back on requests that would break the wire format
   without a major bump, remove an ACK or retransmit path,
   hide a failure mode, ship something that only works on a
   specific board, or trade a real bug fix for cosmetic
   cleanup.** Push back once with the trade-off in plain
   language, then do what the user asks.

2. **Read the markdown first.** `README.md`, this file,
   `docs/Version.md`. The history is the source of truth.

3. **Bump the version on every change.** Update
   `AUTOLINK_VERSION` in `include/AutoLink.h` AND `version=`
   in `library.properties` in lockstep. The version is the
   contract.

4. **Verify before declaring done.**
   - `cd test && make test` — host unit suite.
   - `cd test && make itest` — host integration suite.
   - `./build/verify_build.sh` — ESP32 cross-compile.
   For protocol changes (`Link.cpp`), also run `make loopback`.

4a. **Never delete the `build/` directory.** It is part of
    the project. `build_env.sh`, `verify_build.sh`,
    `arduino-cli-cmd.sh`, `pretty_print.py`,
    `test_pretty_print.py`, and `verify_build/verify_build.ino`
    all live there. If you find `build/` missing in a clone
    or working tree, restore it from git or the last good
    zip BEFORE doing anything else. The cross-compile
    wrapper chain depends on it.

5. **Update `docs/Version.md`.** Newest entry on top. One-line
   description, the fix, the regression test, disclosed
   limitations. **Trim `docs/Version.md` to the last 8
   versions whenever you add a new one.** Older history lives
   in git.

6. **Zip is the deliverable.** Stage the library into a temp
   dir at the archive root (`cd staging && zip -r -y
   /path/to/output.zip .`) — **flat root, no wrapper folder**.
   Strip test build byproducts, `node_modules/`, host-dir
   junk (`$HOME`, `.cache/`, `.npm-global/`). Normalize perms
   (dirs 0755, files 0644, scripts 0755, `library.properties`
   0644). Verify by extracting into a fresh dir and running
   `make test` from the extracted tree. Surface the zip in
   `<deliver-assets>` in your final response.

7. **README example code must match `examples/` and `src/`.**
   Diff line-by-line before zipping. A snippet that doesn't
   compile is a 100% reproducible install failure.

8. **Comments are minimum.** *Why* (not *what*), *which side
   of the wire*, *which test pins this*. No historical
   anchors ("this used to do X"). A senior dev reads the type
   and the function name.

9. **Short names.** `b`, `n`, `i`, `j`, `e`, `ok`, `lv`, `seq`,
   `cb`. Not `m_messageBufferLength`.

10. **Composition over inheritance.** Function-pointer
    callbacks or `unique_ptr<Interface>` over virtual bases.
    Saves a vtable per Link instance (~32B on ESP32). Use
    inheritance only at the user-extension boundary (e.g.
    `IHal`).

11. **Never do RTOS work in a ctor that may run before the
    scheduler is up.** Namespace-scope `PingPong upp(...);` is
    hoisted into `.init_array` and runs before `app_main` /
    `setup()`. A ctor calling `xSemaphoreCreateMutex`,
    `xStreamBufferCreate`, `xTaskCreate`, `xTimerCreate`, or
    even `malloc` can crash the kernel. Safe shapes: Meyers
    singleton via `getInstance()`, or store config in the
    ctor and allocate RTOS primitives in `begin()`.

12. **Logging.** Never log "entering function" / "got N bytes"
    / hot-path chatter. Log: version at startup, wire-op
    results (`ack received`, `retransmit N`), state-change
    causes (`link dropped: 30 s idle`), error resolutions.

13. **No build artifacts in the zip.** No `*.o`, `*.bak`,
    `run_test_*`, `run_loopback*`, `*.gcno`/`*.gcda`,
    `node_modules/`, `compile_commands.json`. Keep only
    `.cpp`, `.h`, `.ino`, `.md`, `.properties`, `.txt`, and
    the scripts under `build/`.

14. **Use `arduino-cli-cmd.sh`, never bare `arduino-cli`.**

14a. **Run `build/pretty_print.py` on every changed `.cpp` /
     `.h` before zipping.** It wraps `clang-format -i` with
     the project's `.clang-format` (which has
     `BreakStringLiterals: false` set) plus a one-time
     string-merge pass for legacy splits. Run:
     ```
     build/pretty_print.py $(find . -name '*.cpp' -o -name '*.h')
     ```
     Pre-existing files that were untouched don't need to be
     re-formatted, but any file you change must leave the
     tree `pretty_print.py`-clean for that file. The
     self-tests live at `build/test_pretty_print.py`.

15. **Smoke-compile a user sketch with every public include.**
    After `verify_build.sh` passes, compile a one-file `.ino`
    that `#include`s every public header the user types.
    Catches include-path / subdir / quoting bugs the
    cross-compile misses.

16. **Every fix gets a regression test that fails when the
    fix is reverted.** Toggle the fix off — the test MUST
    go red. Toggle it back on — green. If the cycle is
    green/green, the test is useless. Don't write tests
    that just call a function and discard the return value.

17. **Unit tests are small, subsecond, and live under
    `test/test_desktop/`.** No `sleep()`, no wall-clock
    busy-waits, no multi-second clock advance. Time-
    dependent behaviour is reached via `MockHal::pumpClock`
    / `runFor`, which advance the simulated clock without
    blocking the host. Anything that cannot stay subsecond
    goes under `test/itest/`.

18. **`make all` runs everything and prints a summary.**
    ```
    === Combined test summary ===
      unit tests      : X passed, Y failed
      itest suites    : X passed, Y failed
      total tests     : X passed, Y failed
      total bytes     : N B (sum of all suite binaries on disk)
      peak memory     : K KiB (largest single-suite resident set)
      total wall time : N ms
    ```
    Each per-suite Makefile prints its own sub-summary first.
    The exit code carries the green/red signal to CI.

19. **Test files mirror the source package.** Under
    `test/test_desktop/al/`, mirror `src/al/` one-for-one.
    `test/test_desktop/al/pingpong/` is the documented
    exception: `Ping`/`Pong` are `#ifdef ARDUINO`, no host
    coverage exists, loopback tests in `al/link/` exercise
    the same protocol.

20. **Pure decision logic separated from I/O.** Protocol
    decisions are pure free functions returning enums
    (`decideArqSlot(ageMs, retxCount, ackRtoMs, maxRetx)`).
    Side effects live in the caller. Table-tested
    exhaustively with no hardware and no mocks.

21. **Time and scheduling are injectable.** Idle-timeout,
    ACK RTO, SWP/LCK stall are reached via
    `MockHal::pumpClock(deltaMs)` and `runFor(targetMs)`.
    Any wall-clock wait or `while(!deadline)` poll bypasses
    the host test suite.

22. **One owner for in-flight state.** Protocol owns the seq
    stamps; facade owns payload storage, sequenced by the
    same mutex. Cache hooks fire INSIDE the link lock. No
    translation tables.

23. **Cheap invariant checks in the host debug build.** After
    every public facade call that mutates state, assert the
    cross-checks under `#ifdef AUTOLINK_HOST_TEST`. Turn
    "fails silently after 3 drops" into "aborts on the drop
    that broke the invariant".

24. **Zip file-list diff.** Before declaring the zip
    done, diff its file inventory against a known-good
    reference (e.g. the user's last working zip):
    ```
    unzip -l <ref>.zip | awk 'NR>3 {print $NF}' | sort > /tmp/old.txt
    unzip -l <new>.zip | awk 'NR>3 {print $NF}' | sort > /tmp/new.txt
    comm -23 /tmp/old.txt /tmp/new.txt   # lost
    comm -13 /tmp/old.txt /tmp/new.txt   # added
    ```
    A non-empty "lost" set means a file the user depends on
    is missing from the zip even if the cross-compile passed.

## Gotchas

- **`portYIELD()`** needs `<freertos/FreeRTOS.h>` at file
  scope, not inside `namespace autolink {
    ... }`.
- **`arduino-cli --library`** (singular). The plural flag
  doesn't recurse into subdirs.
- **`AutoLinkWeb.cpp`** isn't covered by host tests. Any
  change needs a cross-compile to verify.
- **`AutoLink` ctor** first arg is `uart_port_t`, not `int`.
- **`Log::log().info(...)`** — singleton accessor.
- **`Stats`** has `tx`, `rx`, `discCount`, `frameErrs`. No
  `txBps`/`rxBps` on the struct (computed in dashboard JSON).

## Quick workflow

```
1. Read README.md, this file, docs/Version.md.
2. Make the change.
3. Bump AUTOLINK_VERSION + library.properties in lockstep.
4. cd test && make test && make itest.
5. ./build/verify_build.sh.
6. Rule 15 smoke compile.
7. Add a docs/Version.md entry (newest on top, short).
   Trim to the last 8 versions.
8. Sweep comments: strip anything not needed by a senior dev.
9. Rule 24 baseline check.
10. Zip → /workspace/autolink/AutoLink-<version>.zip.
11. Surface in <deliver-assets>.
12. Final response must report, for **unit** and **itest**
    separately and combined:
      - **total test FUNCTIONS passed** (X / Y format — count
        individual `test_*()` functions across every suite,
        not just the number of suite binaries)
      - **total time elapsed** (wall clock, ms or s)
    Plus the zip size and any other metrics the user asked for.

    Useful counts:
      ```
      find test -name '*Test.cpp' | xargs grep -cE           '^(void|static void|int) test_' | awk -F: '{s+=$2} END {print s}'
      ```
    (Currently ~232 unit C++ test functions + ~35 dashboard
    JS tests + ~3 itest smoke tests.)
```

## When you are stuck

- **Compile error in `Link.cpp` on Arduino-ESP32** — check
  `freertos/FreeRTOS.h` is at file scope.
- **Linker error about a `util/` symbol** — you're using
  `--libraries` (plural). Use `--library`.
- **Linker error in production but not in tests** — host
  tests don't compile `AutoLinkWeb.cpp`. Cross-compile.
- **Dashboard times out** — re-check `portYIELD()` at the
  end of `Link::sendMsg` and that the httpd task is at lower
  priority than loopTask.
- **Dashboard renders wrong** — run `make test_dashboard_js`
  in `test/test_desktop/`.
- **Toggle test doesn't fail when fix is reverted** — the
  test doesn't pin the bug. Rewrite it.
