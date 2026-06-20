# AGENTS.md — Working With AutoLink

This file is for AI agents (or any new contributor) working on the
AutoLink ESP32 library. It captures the rules and workflow that
keep the project healthy, plus everything that has bitten me in the
past so you don't have to re-discover it. **Read this file first,
then `docVersion.md`, then the rest of `*.md` in this directory
for context.**

## Project shape

- `src/` — library code (compiled into a static archive for the
  ESP32 Arduino build; also compiled per-file for the host tests).
  Subdirectories (`util/`, `web/`) hold headers and a few
  sources. Both `arduino-cli --library` and the spec-compliant
  builders (ArduinoDroid, Arduino IDE 1.8/2.x, PlatformIO) recurse
  into them.
- `test/test_desktop/` — host-side test suite (C++ + JS). Runs
  without an ESP32. Use `make` to run all suites. **The host
  tests do NOT compile `AutoLinkWeb.cpp`** — they only cover
  `AutoLinkWebCore.cpp`. Any change to the Arduino-only glue file
  needs an actual cross-compile to verify.
- `verify_build/verify_build.ino` — minimal Arduino sketch that
  exercises every public `AutoLink` / `AutoLinkWeb` API. Use it
  to verify an actual ESP32 build before declaring anything done.
- `test/test_desktop/loopback_test.cpp` — host-side loopback that
  wires two `ALink` instances through a host pipe (no hardware).
  Run via `make loopback` in `test/test_desktop/`. Useful for
  end-to-end smoke testing protocol changes without flashing.
- `build/` — persistent build scripts. Re-run them; do not re-derive.
  - `build_env.sh` — installs `arduino-cli` (latest stable) and
    `esp32:esp32@3.3.5`.
  - `verify_build.sh` — cross-compiles `verify_build.ino` against
    the user's FQBN. Uses `--library` (not `--libraries` — the
    plural form skips subdirectories).
- `docVersion.md` — version history. Newest first. Short bullets.
- `library.properties` — the version string the Arduino IDE reads.
  It MUST match `AUTOLINK_VERSION` in `src/AutoLink.h`.
- `AGENTS.md` — this file.

## Hard rules

1. **Use your judgement as a senior ESP32 developer.** Question any
   request that would: break the wire format without a major bump,
   remove an ACK or retransmit path, hide a failure mode, ship
   something that only works on a specific board, or trade a real
   bug fix for cosmetic cleanup. If the user pushes back, push back
   once with the trade-off in plain language, then do what they
   ask.

2. **Read the markdown files first.** `docVersion.md` (newest
   first), `README.md` (Document Index table for the full list),
   this file. They are the source of truth for what was already
   tried, what failed, and what the user already rejected.

3. **Always create a new version** on any code change. Bump
   `AUTOLINK_VERSION` in `src/AutoLink.h` AND `version=` in
   `library.properties` together. The version is the contract; if
   it changes, every user-facing string (`#define` + `library.properties`
   + footer in the embedded HTML) must change in lockstep.

4. **Build the dev environment to test like ArduinoDroid.**
   Run `./build/build_env.sh` once. Then `./build/verify_build.sh`
   cross-compiles the sketch. **Both must pass** before you zip.
   For protocol changes (anything in `ALink.cpp`), also run
   `make loopback` in `test/test_desktop/` — the loopback wires
   two `ALink` instances through a host pipe and shows whether
   the link actually converges. Catches state-machine regressions
   that the unit tests miss.

5. **Update `docVersion.md` with a SHORT list of bugs and
   features.** Newest entry on top. Each entry: one-line
   description, the actual fix, the test that catches a future
   regression, the disclosed limitations. No marketing prose.
   **Always include a "Disclosed limitations" bullet** if the fix
   wasn't verified end-to-end (e.g. "I cannot run on hardware, only
   host + cross-compile").

6. **Output a new zip every time you make changes.** The zip is
   the deliverable. Without a zip, the user cannot install the
   library. Use the user's preferred zip path (e.g.
   `/workspace/autolink/AutoLink-<version>.zip`). Put the zip
   in your final response inside `<deliver-assets>` — the user
   cannot access your filesystem.

7. **Verify the markdown is accurate.** Before zipping, re-read
   `docVersion.md` and check: are the bugs described actually
   fixed in this version? Are the disclosed limitations still
   true? Are the "no change" claims still correct? `README.md`
   and `library.properties` must reflect the same version string.

8. **Use short variable names.** `b`, `n`, `i`, `j`, `e`, `ok`,
   `lv`, `seq`, `cb` are fine. `m_messageBufferLength` is not.
   The reader is a senior developer — they read the type, not
   the name.

9. **Comments are minimum.** A comment should answer a question
   the type system can't: *why* (not *what*), *which side of the
   wire*, *which test pins this*. No historical anchors
   (`v4.X.Y:`). No "this used to do X, now it does Y" — the git
   history has that. If the code is obvious to a senior
   developer, no comment.

9a. **Never copy-paste code.** If you find yourself writing
    `// same as above` or copying a 20-line block from one file to
    another, STOP. Either extract a helper, write a generator, or
    take a different approach. The only acceptable copy-paste is
    a single function call to a shared helper. Two near-identical
    blocks in different files is a bug waiting to happen — they
    will drift.

10. **Never log anything obvious to a senior developer.** Do not
    log inside hot paths. Do not log "entering function", "exiting
    function", "got N bytes". Do log: the version at startup, the
    result of a wire operation (`ack received`, `retransmit N`),
    the cause of a state change (`link dropped: 30 s idle`), the
    resolution of an error.

11. **Never output a zip until verified.** Both the host tests
    (`make` in `test/test_desktop/`) AND the ESP32 cross-compile
    (`./build/verify_build.sh`) must pass. Fix what you broke
    before re-zipping.

12. **Never output a zip containing compiler errors.** If the
    cross-compile reports even one error or warning, the zip is
    bad. The user should never have to debug what you shipped.

13. **Test everything that needs testing.** Host tests cover the
    ARQ state machine, the dashboard core, the dashboard JS, the
    COBS/CRC framing. They do NOT cover: actual ESP32 WiFi,
    `esp_restart`, the UART driver, the httpd task, or the
    Arduino-only `AutoLinkWeb.cpp` glue. So a green host test
    suite is necessary but not sufficient. Cross-compile the
    sketch before declaring done.

14. **Never include temporary build artifacts in the zip.** No
    `.bak` files, no `*.o`, no `run_test_*` / `run_loopback`
    binaries, no `compile_commands.json`, no `*.gcno` / `*.gcda`
    coverage artifacts, no `node_modules/` or `package-lock.json`
    unless they're load-bearing. The zip is the library the user
    installs — it should contain ONLY the library sources, headers,
    examples, docs, and `library.properties`. Before zipping,
    scan the candidate list and remove anything that isn't
    source-or-doc.

    Quick filter for the zip:
    ```
    # Anything that looks like a build artifact:
    *.bak  *.o  *.gcno  *.gcda  compile_commands.json
    run_test_*  run_loopback  run_test_alink_*
    node_modules  package-lock.json
    ```
    Keep: `.cpp .h .ino .md .properties .txt` and the build
    scripts under `build/`. Lesson learned the hard way in
    v5.1.14: an `AutoLinkTest.cpp.bak` was left behind from
    manual editing and shipped in the zip.

15. **Install arduino-cli if it's not installed.** Before running
    `./build/verify_build.sh`, check `command -v arduino-cli`. If
    it returns nothing, run `./build/build_env.sh` to install
    arduino-cli 1.5.1 + esp32:esp32@3.3.5. The script is idempotent
    — if arduino-cli is already on PATH and the ESP32 core is
    installed, it no-ops. Don't skip this and then claim "I can't
    verify on Arduino" — the env build is fast (~30s on a fresh
    machine) and the cross-compile is the only check that catches
    Arduino-only header paths, `#ifdef ARDUINO` typos, and library
    layout breaks. Lesson learned the hard way in v5.1.15/5.1.16:
    I shipped zips with stale `../AutoLinkWeb.h` paths in
    `src/pingpong/` (now flattened into `src/`) and called the host suite "green" without
    noticing the Arduino-side compile would fail. The path was
    caught by a user error message, not by my own test pass.

    **Prefer rule 17's wrapper** for any direct arduino-cli call so
    the install is auto-handled.

16. **Smoke-compile a user sketch with every public include.** After
    `./build/verify_build.sh` passes (rule 4), run the smoke
    compile described in the "Arduino library `src/` layout" gotcha
    below — a single .ino that includes every public header the
    user is expected to type. Catches include-path / subdir /
    quoting bugs that `verify_build.sh` (which uses
    `-DAUTOLINK_HOST_TEST`-style direct includes) does not.
    v5.1.18 shipped with the include path broken for the PingPong
    example; a user `sketch_jun17b.ino` on 2026-06-19 tripped over
    it. The smoke compile takes ~30s and would have caught it.

17a. **Every fix must have a regression test that actually fails
    when the fix is reverted.** The test pattern is:
    ```
    1. Apply the fix.
    2. Run the test. PASS.
    3. Revert the fix (comment it out, set the gate to false, etc).
    4. Run the test. It MUST FAIL.
    5. Re-apply the fix. Run the test. PASS again.
    ```
    If step 4 doesn't fail, the test is useless — it doesn't pin
    the bug. **A green test suite is not the same as a tested fix.**
    This rule exists because the v5.1.30-v5.1.35 series shipped
    three times with tests that "passed" but did not exercise the
    failure mode they purported to cover. The user burned three
    days debugging symptoms of bugs whose tests were never
    toggled.

    Specifically:
    * **Don't** write a test that just calls a function and
      discards the return value. That's not a test, that's
      syntax checking.
    * **Don't** write a test that reads source text and asserts
      it contains a substring. That's grep, not a test. (We do
      use grep-style structural pins for code that can't run on
      host — see `AutoLinkFacadeTest.cpp` — but those are
      documented as such and the pin must still fail when the
      code is commented out.)
    * **Don't** ship a fix without first verifying the test
      catches the bug. Toggle fix off, run, confirm FAIL.
    * **Do** toggle every new fix before declaring done. Budget
      5 extra minutes per fix for the toggle-verify cycle.

18. **Test files mirror the source package they exercise.** The rule
    Never call bare `arduino-cli` from a recipe, Makefile, shell
    script, or AGENTS.md note. The wrapper (`build/arduino-cli-cmd.sh`)
    does three things bare arduino-cli does not:

    1. **Installs on demand.** If `command -v arduino-cli` returns
       nothing, the wrapper delegates to `build/build_env.sh` which
       downloads arduino-cli 1.5.1 + the esp32:esp32@3.3.5 core.
       No more "arduino-cli not found, run build_env.sh first"
       failures on a fresh machine.
    2. **No-args path prints the version.** `bash
       build/arduino-cli-cmd.sh` with no args prints `arduino-cli
       version` — useful as a one-line "is the env ready?" check in
       CI logs, README, or AGENTS.md recipes.
    3. **Single entry point.** If arduino-cli's download URL changes
       again, only `build/build_env.sh` (called by the wrapper)
       needs updating. Every other recipe stays stable.

    Usage:
    ```bash
    bash build/arduino-cli-cmd.sh                                  # version
    bash build/arduino-cli-cmd.sh compile --fqbn esp32:esp32:firebeetle32 \
        --library . --warnings none /tmp/Smoke                     # compile
    bash build/arduino-cli-cmd.sh lib install SomeLib@1.0.0         # install lib
    ```

    Forbidden: calling `arduino-cli` directly, hard-coding the path
    to `/usr/local/bin/arduino-cli`, or assuming arduino-cli is on
    PATH. Use the wrapper everywhere.

18. **Test files mirror the source package they exercise.** The rule
    for `test/test_desktop/`:

    * `test/test_desktop/al/<pkg>/<Test>.cpp` — tests a specific
      `src/al/<pkg>/` package. This is where most tests live.
      `al/protocol/ALink*Test.cpp`, `al/util/UtilCrcTest.cpp`,
      `al/web/AutoLinkWebTest.cpp`, `al/protocol/loopback_test.cpp`
      (protocol-integration test), all match this rule.
    * `test/test_desktop/<File>.cpp` — reserved for tests that
      exercise something at the **top level** of `src/` (the public
      surface) OR shared test infrastructure. Currently:
      - `AutoLinkTest.cpp` — exercises `src/AutoLink.{h,cpp}`
        (the public facade). Stays at top because there's no
        `al/autolink/` package for it (same rule as `src/AutoLink.h`
        being at `src/`, not under `al/`).
      - **`AutoLinkFacadeTest.cpp`** — exercises the AutoLink
        facade's **observable behavior**, not just its syntax. Tests
        here MUST fail when their target bug is reverted (rule 17a).
        Stays at top for the same reason as `AutoLinkTest.cpp`.
      - `MockHalTest.cpp` + `MockHal.h` — shared **test infra**
        used by protocol, util, AND web tests. Moving it under
        any one package would create a back-reference from the
        others. Stays at top.
      - `Makefile`, `coverage_merge.sh`, `dashboard-js-test.js`,
        `package*.json`, `node_modules/` — build / packaging infra,
        not test files.
    * When in doubt: ask "which `src/` file does this test exercise?"
      If it's `src/al/<pkg>/X.cpp`, the test belongs in
      `test/test_desktop/al/<pkg>/`. If it's `src/X.cpp` (public
      surface), the test stays at top. If it exercises shared infra
      like `MockHal.h`, it stays at top.

19. **The AutoLink facade must have behavioral coverage on host.**
    The facade (`src/AutoLink.{h,cpp}`) is the user-facing API.
    On host we can't construct it with a working UART, but we CAN
    exercise:
    * The ARQ cache helpers (`test_arqCache_put`, `arqCacheSizeForTest`,
      `test_arqCache_takeRetxBuffer`).
    * The reset methods (`resetStats`, `resetErrors`, `resetDiag`).
    * The link accessor (`linkForTest`) for inspecting underlying
      state.
    * Structural pins on the source for code that can't be
      exercised at runtime (e.g., gates whose runtime test requires
      UART). See `AutoLinkFacadeTest.cpp` for the established
      pattern.
    Every bug in the facade's ARQ / cache / reset / link-pause
    paths should pin itself to a test in `AutoLinkFacadeTest.cpp`.
    Don't add such tests to `AutoLinkTest.cpp` — that file is for
    smoke / construction tests; behavioral regressions belong in
    the facade test, named after the bug they pin.

19a. **The AutoLink facade must have CLOSED-LOOP coverage across
    link drops on host.** v5.1.35-36 had a class of bugs that the
    unit tests missed: the facade ARQ cache being orphaned by a
    link drop, with the cache-full gate weaponizing the leak into
    a self-deadlock. These bugs only manifest when a real ARQ
    state machine is running under the facade across a re-sweep.
    The unit tests in `AutoLinkFacadeTest.cpp` exercise the cache
    helpers in isolation (bypassing the protocol) and the loopback
    test in `loopback_test.cpp` exercises the protocol in isolation
    (bypassing the facade). Neither met in the middle. v5.1.37
    fixed this by adding `WireSim` — a first-class in-process wire
    model that connects two full `AutoLink` instances back-to-back
    with a pipe, frame-drop, forced drops, and a closed-loop
    `TwoNodeFixture` driving the real send/recv/window logic from
    `UtilPing`/`UtilPong`. `WireSimClosedLoopTest.cpp` is the
    canonical test for any future bug in the facade's interaction
    with the protocol across drops.

    The WireSim is built on a host-only `AutoLink(ILink*,
    isMasterNode, cfg)` injection constructor (gated to
    `AUTOLINK_HOST_TEST`). The ILink is BORROWED, not owned
    (WireSim heap-allocates the MockHals and uses a `NoOpDeleter`
    on the facade's `unique_ptr<ILink>` to avoid double-free). Any
    future bug that involves the closed loop (e.g., "link drops
    when it shouldn't", "cache grows across drops", "bytes stop
    moving after a noise event") MUST be pinned to a test in
    `WireSimClosedLoopTest.cpp`. Don't put such tests in
    `AutoLinkFacadeTest.cpp` — those test the cache in isolation;
    the closed loop is the missing dimension.

19c. **Time and scheduling are injectable; idle-timeout, ACK RTO,
    and SWP/LCK stall are sub-ms deterministic host tests.** Pre-
    v5.1.40 had three time-dependent paths unreachable on host:
    idle watchdog (`cfg.idleTimeoutMs`), ACK retransmit timeout
    (`ACK_RTO_MS`), and SWP/LCK stall retries. Either you waited
    real wall-clock time (not feasible) or you called `onTimer()`
    unconditionally (which doesn't faithfully simulate "5 s of
    idle"). v5.1.40 adds `MockHal::pumpClock(deltaMs)` —
    advances the simulated clock AND fires `onTimer()` only when
    the protocol's scheduled deadline has elapsed. Same
    chokepoint in production: `EspHal::startTimer(ms)` schedules
    a FreeRTOS timer that calls `link->onTimer()` when it fires.
    Both paths are deterministic in their respective contexts.
    `MockHal::runFor(targetMs)` is a convenience that loops
    `pumpClock` until total elapsed. **Any new time-dependent
    behavior MUST be reachable via `pumpClock`/`runFor`** — if
    you add a wall-clock wait or a `while(!deadline)` poll
    without going through the timer scheduling API, the host test
    suite will silently miss it. Companion rule: **`ALink::begin()`
    MUST be called from `AutoLink::begin()` on host too** (was
    gated to ARDUINO pre-v5.1.40). Without it, `MockHal::startTimer`
    is never called, no timer is armed, and host tests can't
    drive the state machine. The change is safe on host because
    `EspHal::begin()` is a no-op there.
    
    The three deterministic clock tests live in
    `ClockInjectionTest.cpp` (added v5.1.40): idle watchdog
    drops link after `cfg.idleTimeoutMs` of simulated silence;
    ACK timeout at `ACK_RTO_MS` triggers retransmit on a
    one-way wire; SWP/LCK stall forces `sendBreak()` after
    `allowedBaudsCount * 2 * cfg.delayMs`. Toggle-verified:
    reverting `pumpClock` to a no-op fails test 1.

19b. **One owner for in-flight state: the protocol owns the seq
    stamps; the facade owns payload storage, sequenced by the same
    mutex.** Pre-v5.1.39 had a two-layer split: the protocol
    tracked `ackedPending_[s]`, `retxCount_[s]`, `sentAtMs_[s]`,
    `baseSeq_[s]`; the facade tracked `pending_[48]`,
    `pendingCount_`, and a `seqToPending_[256]` translation. The
    layers were sequenced by different locks (the protocol's
    link lock vs the facade's implicit single-threaded access).
    This produced a class of races the user identified: gate
    latches because the facade's count drifted from the
    protocol's stamped seqs; cache-insert races seq-stamp;
    drop-clear races cache-insert.

    v5.1.39 collapses the split: the protocol owns ALL
    seq-stamping (gate check, cobsSeq stamping, cache-insert
    callback, drop-clear callback — all under `hw.lock()`). The
    facade owns payload bytes, but only via function-pointer
    callbacks (`ArqCacheHasRoomCallback`,
    `ArqCacheInsertCallback`, `ArqCacheClearAllCallback`) that
    fire INSIDE the protocol's lock. The cache is keyed directly
    on `cobsSeq` (`pending_[256]`, one per seq), so there's no
    translation table and no race on lookup.

    **Any new cache state in the facade MUST be one of: (a)
    read-only from the protocol's perspective, (b) mutated only
    via a callback the protocol invokes under its lock, or (c)
    protected by a lock of its own (avoid this — keep one
    lock). Never add a new field to `pending_[]` that the
    protocol also tracks — that recreates the two-layer split.**
    The `chunks_total` field added in v5.1.39 is set by the
    protocol via the insert callback and read by the retx
    callback — both under the protocol's lock, so it's safe.
    Any future per-seq cache metadata must follow the same
    pattern. The `AutoLink::sendMsg` is now a 2-line wrapper
    around `link->sendMsgEx`; it does NOT touch the cache
    directly. Don't add cache logic to `sendMsg`.

## Gotchas (things that bit me, do not re-discover)

### `portYIELD()` location

`portYIELD()` requires `<freertos/FreeRTOS.h>` to be in scope.
**The include MUST be at file scope**, NOT inside
`namespace autolink { ... }`. C++ system headers with `extern "C"`
blocks can't be nested inside a namespace — the FreeRTOS-Kernel
headers will fail to parse with `expected unqualified-id before
string constant` on every `extern "C" {`. The fix: include
`freertos/FreeRTOS.h` at the top of `ALink.cpp`, before the
namespace opens.

### `arduino-cli --library` vs `--libraries`

Use the **singular `--library`** flag. The plural `--libraries`
flag is for legacy/multi-arg use and does **not** recurse into
subdirectories. The error looks like:

```
undefined reference to `autolink::UtilBaudSweep::configure(...)`
```

…even though the source file is right there at `src/util/UtilBaudSweep.cpp`.
The fix is one character: `--library` instead of `--libraries`.
The single-arg form does the right thing per the Arduino library
spec 1.5 (compiles all .cpp files in the library root and all
subdirectories).

This is a real gotcha because every example on the internet
shows `--libraries`. It's wrong for this case.

### Arduino library `src/` layout — subdirs OK, flat shims required

The Arduino Library Spec 1.5 allows subdirs of `src/` and says
they should be searchable for include resolution. Arduino IDE 2.x
implements this. arduino-cli 1.5.1 (used by ArduinoDroid) does
NOT — it adds `src/` to the include path but does not recurse.

The library layout that works under both:

```
src/
  AutoLink.h                 # flat, user-facing
  PingPong.h                 # FLAT SHIM that #includes "pingpong/PingPong.h"
  util/                      # internal helpers, used by library .cpp files
    Log.h
    UtilPing.h
    ...
  web/                       # internal helpers, used by library .cpp files
    AutoLinkWeb.h
    ...
  pingpong/                  # canonical home of the PingPong example headers
    PingPong.h
```

User sketches include from the **flat** names:
```cpp
#include "AutoLink.h"     // works under both arduino-cli and IDE 2.x
#include "PingPong.h"     // works under both
```
NOT the subdir names — `<pingpong/PingPong.h>` only resolves
under Arduino IDE 2.x.

Library .cpp files include via the subdir when needed:
```cpp
// in src/PingPong.h (the flat shim):
#include "pingpong/PingPong.h"   // relative to the shim's location
```
This works under both because relative-path include from inside
the library's own source tree resolves naturally.

**The smoke-compile rule (rule 16):** before zipping, compile a
one-file sketch with every public include the user is expected
to type. The flat syntax MUST compile under arduino-cli.
```bash
mkdir -p /tmp/Smoke && cat > /tmp/Smoke/Smoke.ino <<'EOF'
#include "AutoLink.h"
#include "PingPong.h"
void setup(){} void loop(){}
EOF
bash build/arduino-cli-cmd.sh compile --fqbn esp32:esp32:firebeetle32 \
    --library . \
    --warnings none /tmp/Smoke
```

### `AutoLinkWeb.cpp` is not covered by the host tests

The host test suite (`AutoLinkWebTest.cpp`) tests
`AutoLinkWebCore.cpp` (the host-testable format/parse helpers),
NOT `AutoLinkWeb.cpp` (the Arduino-only glue with WiFi, NVS, httpd,
esp_timer). **Any change to `AutoLinkWeb.cpp` will pass the host
tests and still fail to compile on the actual ESP32 target.**
A previous bug — an orphan C++ block of deleted `handleReset`
function body left at file scope — survived several versions
because of this. Always cross-compile.

### The `AutoLink::AutoLink()` constructor signature

First argument is `uart_port_t`, not `int`. In sketches:

```cpp
AutoLink link((uart_port_t)1, /*rx=*/16, /*tx=*/17, /*isMaster=*/true, cfg);
```

The cast is required because `uart_port_t` is an enum on the
Arduino-ESP32 SDK and the integer literal `1` won't auto-promote.

### `AutoLinkConfig` field names (real, not guessed)

```cpp
AutoLinkConfig cfg;
cfg.maxMsg        = 1024;   // largest send/recv payload
cfg.idleTimeoutMs = 5000;   // watchdog: drop + re-sweep after N ms idle
cfg.ledPin        = 9;      // status LED
cfg.errThreshold  = 20;     // frame errors before drop
cfg.reliableMode  = true;   // ACK every chunk
cfg.allowedBauds  /  cfg.allowedBaudsCount
cfg.streamBufferSize / cfg.txBufferSize  // 0 = auto-size from maxMsg
cfg.pingSamplesPerBaud / cfg.minAcceptRate
cfg.fastBaudLock  = true;
```

**There is no `appBuf`, `appBufSize`, or `keepaliveMs`** — I
shipped a broken verification sketch once that used these. The
facade auto-sizes the stream buffer from `cfg.maxMsg`.

### `Log::info` is an instance method, not static

Use `Log::getLog().info(...)`, not `Log::info(...)`. The
singleton accessor is `Log::getLog()`.

### `Stats` struct field names

```cpp
Stats st;
link.getStats(st);
st.tx;          // cumulative TX bytes
st.rx;          // cumulative RX bytes
st.discCount;   // OK->SWP transitions
st.frameErrs;   // bad CRC, malformed COBS, oversize
```

There is no `txBps` / `rxBps` on the struct — those are
computed in the dashboard JSON (`AutoLinkWebCore.cpp`).

### `Log` namespace

`Log` is in `namespace autolink`. Most sketches do
`using namespace autolink;` to avoid the prefix.

### v4→v5 wire format break

v5 broke the wire format cleanly — no v4 backwards compat. If
the user asks to "support v4 again", push back: the v5 ARQ
guarantees per-message delivery, which is what makes the link
useful. Re-introducing v4 dual-mode adds two code paths and
doubles the surface area for bugs.

## The loopback test

`test/test_desktop/loopback_test.cpp` wires two `ALink` instances
together through a host-side pipe. No hardware. Used to smoke-test
protocol changes before flashing to ESP32. **Wired into `make all`**
— every full build runs a 10-second loopback as the last step.

```
cd test/test_desktop
make                    # runs all 16 C++ suites + 63 JS tests + 10s loopback
make loopback           # explicit 30s run
make loopback_quick     # 5s run
make loopback_verbose   # 30s + full library debug logging
./run_loopback 60       # raw binary, 60s
```

**What you'll see:** state transitions at startup (SWP -> LCK -> OK),
per-second TX/RX rates, ARQ retransmits, frame errors, final stats.

**Expected output (best-case loopback, no noise):**
- 1 disconnect during initial negotiation (normal — both sides
  enter SWP, sweep bauds, then fast-ack lock)
- 0–2 frame errors per 10s (host pipe timing artifact; the host
  pipe has no UART frame boundary, so a frame can be split across
  two `onRx()` calls and the parser can occasionally mis-align.
  Real hardware with UART atomic frame delivery does not have this.)
- 99%+ message delivery

**Exit codes:** 0 = link OK + data; 1 = OK but no data; 2 = never OK.
CI-friendly.

**When to use:** any time you change `ALink.cpp` (state machine,
ARQ, framing, negotiation), even a small one. The host unit tests
cover the API but not the full state machine in flight — the
loopback is the cheapest end-to-end check.

## Quick workflow (TL;DR)

```
1. Read docVersion.md, README.md, this file.
2. Make the change.
3. Bump AUTOLINK_VERSION + library.properties in lockstep.
4. Run `make` in test/test_desktop/. Fix anything that fails.
5. Run `./build/verify_build.sh`. Fix anything that fails.
6. Update docVersion.md (newest entry on top, short bullets,
   disclosed limitations).
7. Re-read docVersion.md, README.md, this file for accuracy.
8. Zip → /workspace/autolink/AutoLink-<version>.zip.
9. Surface the zip in <deliver-assets> in your response.
```

## When you are stuck

- **Compile error in `ALink.cpp` on Arduino-ESP32:** Check the
  include order — `freertos/FreeRTOS.h` must be at file scope,
  not inside `namespace autolink { ... }`.
- **Linker error about `UtilBaudSweep::resetAll` (or any util/
  symbol):** You're using `--libraries` (plural). Use
  `--library` (singular). See the gotcha above.
- **Linker error in production but not in tests:** The host test
  suite doesn't compile `AutoLinkWeb.cpp` — only
  `AutoLinkWebCore.cpp`. Any change to the Arduino-only file
  needs an actual cross-compile via `./build/verify_build.sh`.
- **Compilation succeeded but the dashboard times out:** Re-check
  the `portYIELD()` at the end of `ALink::sendMsg` is intact and
  that the httpd task is at a lower priority than loopTask. The
  `/logs` and `/stats` endpoints need the httpd task to get
  scheduled between UART TX-FIFO drain events.
- **Dashboard renders wrong in the browser:** Run
  `cd test/test_desktop && make test_dashboard_js` to drive the
  embedded JS through jsdom. The C++ tests can't catch DOM/CSS
  bugs.
- **"Many versions in one session":** This happens. The wire
  format hasn't changed since v5.0.0; later versions are
  dashboard UI polish + ARQ robustness fixes. Always disclose
  this in the version entry so the user knows.
