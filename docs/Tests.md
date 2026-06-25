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
| `build/verify_build/verify_build.ino` | Smaller smoke-compile sketch exercising every public API. |

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

22 unit binaries, organised one-class-per-file. Run via
`cd test && make test`. Each prints a per-suite
`[PASS]/[FAIL] <name> <ms> <bytes> rss=<KiB>` line, then a
summary block.

| Binary | File | What it covers |
|--------|------|----------------|
| `run_test_crc` | `al/util/UtilCrcTest.cpp` | CRC-8 / CRC-16 LUTs, known-answer vectors, single-bit detection |
| `run_test_cobs` | `al/util/UtilCobsTest.cpp` | COBS encode / decode round-trips, 0xFF group boundary, malformed input |
| `run_test_blink` | `al/util/UtilBlinkTest.cpp` | LED async + blocking patterns, restart, cancel, invalid `n` |
| `run_test_framerx` | `al/link/LinkFrameRxTest.cpp` | Frame delivery, splits, bad CRC, keepalive atom, max cobsSeq=0xFE |
| `run_test_baudsweep` | `al/link/LinkBaudSweepTest.cpp` | Baud scoring, threshold fall-back, real cable scenario |
| `run_test_log` | `al/util/LogTest.cpp` | Level filtering, sink registration, context pointer, truncation |
| `run_test_mockhal` | `al/hal/MockHalTest.cpp` | `MockHal` IHal mock: setSpd, sendBreak, TX buffer, app buffer, clock |
| `run_test_autolink` | `AutoLinkTest.cpp` | `AutoLink` facade: construction, state, stats, stream, message, error control, blinkWait, isHealthy |
| `run_test_alink_facade` | `AutoLinkFacadeTest.cpp` | `AutoLink` facade: behavioral |
| `run_test_wiresim_closedloop` | `WireSimClosedLoopTest.cpp` | WireSim 2-node simulator: full OK-state message exchange |
| `run_test_clock_injection` | `ClockInjectionTest.cpp` | pumpClock/runFor, ACK timeout retx, RTO schedule, cobsSeq wraparound |
| `run_test_linkdecision` | `al/link/LinkDecisionTest.cpp` | Pure decision logic: classifyGap, decideArqSlot, decideSwpTick, decideLckTick, decideIdleWatchdog, decideKeepalive, decideAppBuf |
| `run_test_alink_io` | `al/link/LinkIOTest.cpp` | Byte I/O, reliable mode, throughput, stats, README scenario, 254→0 wrap regression guard |
| `run_test_alink_message` | `al/link/LinkMessageTest.cpp` | Message API: round-trip, boundaries, size sweep, CRC reject, corrupt-header resync, 240-chunk ARQ cap, send-rejection error paths |
| `run_test_alink_error` | `al/link/LinkErrorTest.cpp` | Error threshold, lifetime counter, link-failure regression, scattered errors, parser yield |
| `run_test_alink_cobsseq` | `al/link/LinkCobsSeqTest.cpp` | cobsSeq wraparound, classifyGap, gap accounting, app-buffer-full no-op |
| `run_test_alink_arq` | `al/link/LinkArqTest.cpp` | ARQ constants, ACK_TYPE=0xFF, state machine, retx, cache hooks |
| `run_test_alink_web` | `al/web/AutoLinkWebTest.cpp` | Web dashboard protocol parsing, command dispatch |
| `run_test_alink_v53` | `al/link/LinkV53Test.cpp` | Baud-preference + rate-window regression guards |
| `run_test_alink_v531` | `al/link/LinkV531Test.cpp` | 3-phase sweep + asymmetric fast detection |
| `run_test_alink_v531_never_leave_p1` | `al/link/LinkV531NeverLeaveP1Test.cpp` | Phase-1 stuck-peer guard |
| `run_test_linkreorder` | `al/link/LinkReorderTest.cpp` | Hold-on-gap reorder buffer + staleness cap |

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
`coverage/coverage.txt`.

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
`AutoLink` facade — construction, the Stream / state / stats /
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
6. **Add the binary** to `TEST_BINS`, `coverage_merge.sh`'s
   `src_for` map (if it has a more-comprehensive view of any
   source), and the `clean` target.

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
test binary's gcno instead of the canonical source binary, edit
the `src_for` map in `coverage_merge.sh`.
