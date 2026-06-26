# 🧪 AutoLink Tests

The host unit suite runs natively on your PC (no ESP32 needed).
The host integration suite exercises two `Link` instances over a
host pipe for multi-second end-to-end runs. The embedded
integration suite cross-compiles a sketch against the real ESP32
toolchain.

# 📂 Layout

| Path | Purpose |
|------|---------|
| `test/test_desktop/` | Host **unit** tests. Small, subsecond. Run via `cd test && make test`. |
| `test/itest/test_desktop/` | Host **integration** tests (loopback + noise). Multi-second. Run via `cd test && make itest`. |
| `test/itest/test_embedded/test_embedded.ino` | On-hardware self-loopback smoke. Cross-compiled via `cd test/itest/test_embedded && make`. |
| `test/test_desktop/Makefile` | Unit build / run / coverage entry. |
| `test/itest/test_desktop/Makefile` | Integration build / run entry. |
| `test/Makefile` | Top-level orchestrator: `make test`, `make itest`, `make all`. |
| `test/common/MockHal.h` | Shared `MockHal` IHal mock + `pipe_data()` + `negotiate_to_ok()` helpers. |
| `build/verify_build/verify_build.ino` | ESP32 smoke-compile sketch mirroring the README's Pong.ino shape (file-scope `PingPong upp(...)` + minimal `setup()` / `loop()`). Catches ArduinoDroid ctor errors in the user-facing entry point. |

# 🚀 Quick Start

```bash
cd test
make test                  # unit suite — ~2 s
make itest                 # host integration suite — ~35 s
make all                   # both, plus combined summary
```

Each `make` target prints a summary block (tests passed, total
bytes, peak RSS) and exits non-zero on any failure.

## Run one suite

```bash
make -C test/test_desktop test_crc
make -C test/test_desktop test_alink_error
make -C test/itest/test_desktop loopback
```

## Clean

```bash
make -C test clean
```

Removes all binaries, `.gcno`/`.gcda` files, and the `coverage/`
tree.

# 🧪 Unit Suites

30 unit binaries, organised one-class-per-file. Run via
`cd test && make test`. Each prints a per-suite
`[PASS]/[FAIL] <name> <ms> <bytes> rss=<KiB>` line, then a
summary block.

| Binary | File | What it covers |
|--------|------|----------------|
| `run_test_crc` | `al/util/UtilCrcTest.cpp` | CRC-8 / CRC-16 LUTs, known-answer vectors, single-bit detection |
| `run_test_cobs` | `al/util/UtilCobsTest.cpp` | COBS encode / decode round-trips, 0xFF group boundary, malformed input |
| `run_test_blink` | `al/util/UtilBlinkTest.cpp` | LED async + blocking patterns, restart, cancel, invalid `n` |
| `run_test_framerx` | `al/link/LinkFrameRxTest.cpp` | Frame delivery, splits, bad CRC, keepalive atom, max cobsSeq=0xFE |
| `run_test_baudsweep` | `al/link/sweep/LinkBaudSweepTest.cpp` | Baud scoring, threshold fall-back, real cable scenario |
| `run_test_log` | `al/util/LogTest.cpp` | Level filtering, sink registration, context pointer, truncation |
| `run_test_mockhal` | `al/hal/MockHalTest.cpp` | `MockHal` IHal mock: setSpd, sendBreak, TX buffer, app buffer, clock |
| `run_test_autolink` | `al/AutoLinkTest.cpp` | `AutoLink` facade: construction, state, stats, stream, message, error control, blinkWait, isHealthy |
| `run_test_alink_facade` | `al/AutoLinkFacadeTest.cpp` | `AutoLink` facade: behavioral |
| `run_test_wiresim_closedloop` | `al/WireSimClosedLoopTest.cpp` | WireSim 2-node simulator: full OK-state message exchange |
| `run_test_clock_injection` | `al/ClockInjectionTest.cpp` | pumpClock/runFor, ACK timeout retx, RTO schedule, cobsSeq wraparound |
| `run_test_linkdecision` | `al/link/sweep/LinkDecisionTest.cpp` | Pure decision logic: classifyGap, decideArqSlot, decideSwpTick, decideLckTick, decideIdleWatchdog, decideKeepalive, decideAppBuf |
| `run_test_alink_io` | `al/link/LinkIOTest.cpp` | Byte I/O, reliable mode, throughput, stats, README scenario, 254→0 wrap regression guard |
| `run_test_alink_message_roundtrip` | `al/link/LinkMessageRoundtripTest.cpp` | Message API: round-trip, boundaries, size sweep, 240-chunk ARQ cap |
| `run_test_alink_message_corrupt` | `al/link/LinkMessageCorruptTest.cpp` | Corrupt-message detection: CRC reject, no-resync clear, payload bit-flips |
| `run_test_alink_message_resync` | `al/link/LinkMessageResyncTest.cpp` | Resync from corrupted MSG_HDR: oversize L, dropped bytes, false-boundary reject, multichunk loss |
| `run_test_alink_message_edge` | `al/link/LinkMessageEdgeTest.cpp` | Edge cases: zero-byte send, recv buffer too small, empty buffer, app buf null, resetDiag, send-rejection |
| `run_test_alink_error` | `al/link/LinkErrorTest.cpp` | Error threshold, lifetime counter, link-failure regression, scattered errors, parser yield |
| `run_test_alink_cobsseq` | `al/link/LinkCobsSeqTest.cpp` | cobsSeq wraparound, classifyGap, gap accounting, app-buffer-full no-op |
| `run_test_alink_arq` | `al/link/arq/LinkArqTest.cpp` | ARQ constants, ACK_TYPE=0xFF, state machine, retx, cache hooks |
| `run_test_alink_web` | `al/web/AutoLinkWebTest.cpp` | Web dashboard protocol parsing, command dispatch |
| `run_test_handle_root_chunked` | `al/web/HandleRootChunkedTest.cpp` | handleRoot's chunked-send contract + httpd stack size 16384 |
| `run_test_web_begin_lifecycle` | `al/web/WebBeginLifecycleTest.cpp` | setSink-before-httpd, version-line first log, begin() blocks until httpd up, fail-block preserves lifetime resources |
| `run_test_web_httpd_retry` | `al/web/WebHttpdRetryTest.cpp` | setupHttpAndLogging_ retry budget, wifiTaskThunk_ retries forever, pre-delay |
| `run_test_link_begin_defer` | `al/web/LinkBeginDeferTest.cpp` | Link::kickoff deferral when paused; Ping falls through to kickoff when GUI is down |
| `run_test_alink_baud_preference` | `al/link/sweep/LinkBaudPreferenceTest.cpp` | Baud-preference + rate-window regression guards |
| `run_test_alink_sweep_phase` | `al/link/sweep/LinkSweepPhaseTest.cpp` | 3-phase sweep + asymmetric fast detection |
| `run_test_alink_sweep_p1_guard` | `al/link/sweep/LinkSweepP1GuardTest.cpp` | Phase-1 stuck-peer guard |
| `run_test_linkreorder` | `al/link/LinkReorderTest.cpp` | Hold-on-gap reorder buffer + staleness cap |
| `run_test_sync_mode` | `al/SyncModeTest.cpp` | SYNC mode: stop-and-wait send, zero ARQ slots, recovery after wire drop, ACK timeout |
| `run_test_compile_check` | `al/CompileCheckTest.cpp` | Source-level compile-check for ARDUINO-gated code (EspHal, AutoLinkWeb, PingPongMain, Ping) |
| `run_test_esp_idf_error_etiquette` | `al/EspIdfErrorEtiquetteTest.cpp` | Source-level audit: every ESP-IDF call site has paired `esp_err_to_name` log |
| `run_test_version_free_source` | `al/VersionFreeSourceTest.cpp` | Source-level pin for rule 9: no hard-coded X.Y.Z outside the version contract |

The 5 `Link*` files are split from the original monolithic
`LinkTest.cpp` by functionality area so each file stays small
and easy to navigate. The shared `MockHal` lives at
`test/common/MockHal.h`.

# 🔬 Integration Test Modes

The unit `Makefile` has two integration modes on top of the
default build.

## AddressSanitizer + UBSan — the valgrind equivalent

```bash
cd test/test_desktop && make test_asan
```

Rebuilds every binary with
`-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1`
and runs them. Catches use-after-free, buffer overflow, leaks,
undefined behaviour — gcc-native, an order of magnitude faster
than valgrind.

## gcov line + branch coverage

```bash
cd test/test_desktop && make coverage
```

Rebuilds every binary with `--coverage`, runs them so `.gcda`
files are written, then `coverage_merge.sh` picks the canonical
`.gcda` per source (one binary per source, chosen for the most
comprehensive view) and runs `gcov -b` on each. Output is in
`coverage/coverage.txt`. The generator, merger, and its self-test
live co-located under `test/scripts/coverage/`.

# 🔌 Host Integration Suites

Run via `cd test && make itest`. Each runs for seconds and exits
0 on success:

| Binary | What it covers |
|--------|----------------|
| `run_loopback` | Two-Link end-to-end on a host pipe. Default 30 s; configurable via `make loopback` / `make loopback_quick` / raw binary args. |
| `run_loopback_noise` | Same with stochastic frame drops + intermittent BREAKs. ~5 s. |

# 🔌 Embedded Integration Suite

`test/itest/test_embedded/test_embedded.ino` is a single-board
self-loopback sketch. Wire **GPIO17 (TX) → GPIO16 (RX)** on the
same ESP32, flash, open the serial monitor. It exercises the
`AutoLink` facade — construction, the byte-stream / state / stats /
error APIs, `blinkWait` paths — without needing a second board.

For a real two-board end-to-end test, flash
`examples/PingPong/Ping.ino` on one ESP32 and `Pong.ino` on the
other, then watch the serial throughput logs.

# 🧰 Adding a Test

1. **Pick the right file.** If you're testing a class that has a
   dedicated suite, add the test to its file. New test for a new
   functionality area of `Link`? Create a new
   `<Class><Functionality>Test.cpp` in the appropriate
   `test/test_desktop/al/<sub>/` directory.
2. **Match the existing style.** One `test_<thing>()` function
   per case. Each prints a `=== Test: ... ===` header and `PASS`
   / falls on assert. `main()` lists them in execution order.
3. **For protocol tests, use `MockHal.h`:**
   ```cpp
   #include "MockHal.h"   // MockHal + pipe_data + negotiate_to_ok
   using namespace autolink;
   void test_my_thing() {
       MockHal mHal, sHal;
       AutoLinkConfig cfg;
       Link a(mHal, true, cfg);
       Link b(sHal, false, cfg);
       negotiate_to_ok(a, b, mHal, sHal);   // brings pair into State::OK
       // ... exercise a, observe b ...
       std::cout << "PASS" << std::endl;
   }
   ```
4. **For the AutoLink facade**, use `AUTOLINK_HOST_TEST` and the
   host stubs at the top of `AutoLinkTest.cpp` as a template.
5. **Add a build rule** in `Makefile`:
   ```make
   run_test_my_thing: $(LINK_SRC) MyThingTest.cpp
       $(CXX) $(CXXFLAGS) $(LINK_SRC) MyThingTest.cpp -o $@
   test_my_thing: run_test_my_thing
       ./run_test_my_thing
   ```
6. **Add the binary** to `TEST_BINS`. The coverage merger
   (`test/scripts/coverage/coverage_merge.sh`) reads a manifest
   generated from the Makefile, so adding a build rule is
   enough — the manifest self-test
   (`test/scripts/coverage/test_coverage_manifest.py`) will
   fail if the new suite slips through.

# 🆘 Troubleshooting

**`make` says `cannot find MockHal.h`** — you're not in the
right directory. Each `Makefile` expects you to run from its
own directory (`test/test_desktop/`, `test/itest/test_desktop/`,
or `test/`).

**`make coverage` ends with `cannot open notes file`** — a
previous coverage run was interrupted. `make clean` and retry.

**`make test_asan` hangs for >60 s on first run** — the rebuild
is slow because ASan is link-time heavy. Subsequent runs are
faster.

**A test fails only under ASan** — that's a real bug. The
sanitizer found a use-after-free, buffer overflow, or UB that
the normal build missed.

**Coverage numbers are lower than expected** — check the
`Graph:` field in the relevant `.gcov` file. If it points to a
test binary's gcno instead of the canonical source binary, check
that the suite's build rule references the right `$(LINK_SRC)`
or `$(AUTOLINK_SRC)` variable (the manifest generator derives
the `src_for` map from those rules).
