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
  Subdirectories (`util/`, `pingpong/`) hold headers and a few
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
