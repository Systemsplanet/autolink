# 🧪 AutoLink Tests

How to build, run, and extend AutoLink's test suite. The desktop suite
runs natively on your PC (no ESP32 needed); the embedded suite exercises
the real UART peripheral on hardware.

# 📂 Layout

| Path | Purpose |
|------|---------|
| `AutoLink/test/test_desktop/` | Host-side unit + integration tests. 20 binaries, 199 test functions (v5.1.45: wire format break — ACK_TYPE moved 0x33→0xFF, cobsSeq skips 0xFF, LD_SEQ_WRAP=255; merged `LinkDecision.h` table-driven tests; added ARQ chunk-boundary tests; gated dashboard JS test on jsdom). |
| `AutoLink/test/test_embedded/test_embedded.ino` | On-hardware self-loopback smoke test for the `AutoLink` facade. |
| `AutoLink/test/test_desktop/Makefile` | Single entry point for all build / run / coverage modes. |
| `AutoLink/test/test_desktop/coverage_merge.sh` | Helper that assembles the per-source `.gcda` files into a single coverage report. |
| `AutoLink/test/test_desktop/MockHal.h` | Shared `MockHal` ILink mock + `pipe_data()` + `negotiate_to_ok()` helpers. |

# 🚀 Quick Start

## Run every suite (fast, default build)

```bash
cd AutoLink/test/test_desktop
make
```

Output:
```
=== All 20 test suites PASS ===
```

## Run one suite

```bash
make test_alink_error      # ALink error-counter / threshold / parser tests
make test_alink_watchdog   # idle watchdog + keepalive + LCK timeout
make test_util_frame_rx    # the UtilFrameRx suite
make test_mockhal          # the MockHal helper itself
```

Each `test_<name>` target builds the binary if needed, then runs it. A
non-zero exit code means at least one assertion failed.

## Clean

```bash
make clean
```

Removes all binaries, `.gcno`/`.gcda` files, and the `coverage/` tree.

# 🧪 Test Suites

20 binaries, organised one-class-per-file. Counts below are the actual
PASS-line totals on the current code (v5.1.45). Counts move as fixes
land — regenerate via `make all` and count `^PASS` lines per binary.

| Binary | File | Tests | What it covers |
|--------|------|-------|----------------|
| `run_test_crc` | `UtilCrcTest.cpp` | 4 | CRC-8 / CRC-16 LUTs, known-answer vectors, single-bit detection |
| `run_test_cobs` | `UtilCobsTest.cpp` | 4 | COBS encode / decode round-trips, 0xFF group boundary, malformed input |
| `run_test_blink` | `UtilBlinkTest.cpp` | 7 | LED async + blocking patterns, restart, cancel, invalid `n` |
| `run_test_framerx` | `UtilFrameRxTest.cpp` | 15 | Frame delivery, splits, bad CRC, 0x00 0x00 keepalive atom, max cobsSeq=0xFE |
| `run_test_baudsweep` | `UtilBaudSweepTest.cpp` | 12 | Baud scoring, threshold fall-back, real cable scenario |
| `run_test_log` | `LogTest.cpp` | 6 | Level filtering, sink registration, context pointer, truncation |
| `run_test_mockhal` | `MockHalTest.cpp` | 9 | `MockHal` ILink mock: setSpd, sendBreak, TX buffer, app buffer, clock |
| `run_test_alink_io` | `ALinkIOTest.cpp` | 6 | Byte I/O, reliable mode, throughput (v5.1.45: 16000B now passes), stats, README scenario |
| `run_test_alink_message` | `ALinkMessageTest.cpp` | 19 | Message API: round-trip, boundaries, size sweep, CRC reject, corrupt-header resync, 240-chunk ARQ cap, send-rejection error paths |
| `run_test_alink_negotiation` | `ALinkNegotiationTest.cpp` | 3 | SWP/LCK/OK state machine, best-baud scoring, top-down sweep + fast-ack |
| `run_test_alink_error` | `ALinkErrorTest.cpp` | 6 | Error threshold, lifetime counter, link-failure regression, scattered errors, parser yield |
| `run_test_alink_watchdog` | `ALinkWatchdogTest.cpp` | 8 | Idle watchdog, keepalive (3 modes), LCK timeout, peer-death recovery |
| `run_test_alink_cobsseq` | `ALinkCobsSeqTest.cpp` | 14 | cobsSeq wraparound, classifyGap, gap accounting, app-buffer-full no-op |
| `run_test_alink_arq` | `ALinkArqTest.cpp` | 11 | ARQ constants, ACK_TYPE=0xFF (v5.1.45), state machine, retx, cache hooks |
| `run_test_alink_facade` | `AutoLinkFacadeTest.cpp` | 6 | AutoLink facade: behavioral (v5.1.43: structural-pin tests removed) |
| `run_test_alink_web` | `ALinkWebTest.cpp` | 21 | Web dashboard protocol parsing, command dispatch |
| `run_test_autolink` | `AutoLinkTest.cpp` | 13 | `AutoLink` facade: construction, state, stats, stream, message, error control, blinkWait, isHealthy |
| `run_test_clock_injection` | `ClockInjectionTest.cpp` | 8 | pumpClock/runFor, ACK timeout retx (v5.1.45: idleTimeoutMs=300), RTO schedule, cobsSeq wraparound (v5.1.45: merged two wrap tests) |
| `run_test_linkdecision` | `LinkDecisionTest.cpp` | 22 | Pure decision logic (v5.1.41): classifyGap, decideArqSlot, decideSwpTick, decideLckTick, decideIdleWatchdog, decideKeepalive, decideAppBuf |
| `run_test_wiresim_closedloop` | `WireSimClosedLoopTest.cpp` | 4 | WireSim 2-node simulator (v5.1.38): full OK-state message exchange |

The 5 ALink `*` files are split from a single `ALinkTest.cpp` by
functionality area so each file stays small (≤ 250 lines) and easy to
navigate. The shared `MockHal` class and `pipe_data` / `negotiate_to_ok`
helpers live in `MockHal.h`.

# 🔬 Integration Test Modes

The Makefile has two integration modes on top of the default build.

## AddressSanitizer + UBSan — the valgrind equivalent

`valgrind` isn't available everywhere; AddressSanitizer + UBSan catch the
same class of bugs (use-after-free, buffer overflow, leaks, undefined
behaviour) and are gcc-native, so they're an order of magnitude faster.

```bash
make test_asan
```

What it does:
1. Rebuilds every binary with `-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1`.
2. Runs every suite.
3. Reports `ASan + UBSan run complete (no diagnostics = pass)` if clean.

A failed run prints a stack trace pointing at the offending line, e.g.:
```
==12345==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x...
READ of size 1 at 0x...
    #0 0x... in autolink::ALink::onRx(unsigned char const*, int) ALink.cpp:418
```

To run a single suite under ASan with leak detection explicitly on:
```bash
ASAN_OPTIONS=detect_leaks=1 ./run_test_alink_watchdog
```

## gcov line + branch coverage

```bash
make coverage
```

What it does:
1. Rebuilds every binary with `-fprofile-arcs -ftest-coverage -fbranch-probabilities -O0`.
2. Runs every suite (each binary's `.gcda` is written next to the binary).
3. `coverage_merge.sh` picks the canonical `.gcda` per source (one binary
   per source, chosen for the most-comprehensive view) and runs `gcov -b`
   on each.
4. Writes `coverage/coverage.txt` and a `.gcov` per source for line-level
   review.

Sample output (v3.2.2):
```
  UtilCobs.cpp          25/25 lines (100.0%)  18/18 branches (100.0%)
  UtilCrc.cpp            9/9  lines (100.0%)   4/4  branches (100.0%)
  UtilBaudSweep.cpp     15/15 lines (100.0%)  16/16 branches (100.0%)
  Log.cpp               14/14 lines (100.0%)   8/8  branches (100.0%)
  UtilFrameRx.cpp       22/22 lines (100.0%)  22/22 branches (100.0%)
  ALink.cpp            320/462 lines ( 69.3%) 226/340 branches ( 66.5%)
```

The 69% on `ALink.cpp` is real coverage — the protocol core has many
defensive checks and error paths that the test suite doesn't exercise.
`grep '^[[:space:]]*#####' coverage/ALink.cpp.gcov` lists every
uncovered line so you can decide whether to add a test or accept the gap.

# 🛠️ CI Recipes

## Local: all green
```bash
cd AutoLink/test/test_desktop
make clean && make
```

## Local: with sanitizers
```bash
make clean && make test_asan
```

## Local: with coverage
```bash
make clean && make coverage
cat coverage/coverage.txt
```

## Pre-commit hook (drop into `.git/hooks/pre-commit`)
```bash
#!/bin/sh
set -e
cd AutoLink/test/test_desktop
make clean > /dev/null
make > /dev/null
echo "OK: AutoLink tests pass"
```

# 🧰 Adding a Test

1. **Pick the right file.** If you're testing a class that has a
   dedicated suite, add the test to its file. New test for a new
   functionality area of `ALink`? Create a new `<Class><Functionality>Test.cpp`
   in `test_desktop/`.
2. **Match the existing style.** One `test_<thing>()` function per case.
   Each prints a `=== Test: ... ===` header and `PASS` / falls on assert.
   `main()` lists them in execution order.
3. **For protocol tests, use `MockHal.h`:**
   ```cpp
   #include "MockHal.h"   // MockHal + pipe_data + negotiate_to_ok
   using namespace autolink;
   void test_my_thing() {
       std::cout << "\n=== Test: My Thing ===" << std::endl;
       MockHal mHal, sHal;
       AutoLinkConfig cfg;
       ALink a(mHal, true, cfg);
       ALink b(sHal, false, cfg);
       negotiate_to_ok(a, b, mHal, sHal);   // brings pair into State::OK
       // ... exercise a, observe b ...
       std::cout << "PASS" << std::endl;
   }
   ```
4. **For the AutoLink facade**, use `AUTOLINK_HOST_TEST` and the host
   stubs at the top of `AutoLinkTest.cpp` as a template.
5. **Add a build rule** in `Makefile`:
   ```make
   run_test_my_thing: $(LINK_SRC) MyThingTest.cpp
       $(CXX) $(CXXFLAGS) $(LINK_SRC) MyThingTest.cpp -o $@
   test_my_thing: run_test_my_thing
       ./run_test_my_thing
   ```
6. **Add the binary** to `TEST_BINS`, `coverage_merge.sh` `src_for` map
   (if it has a more-comprehensive view of any source), and `clean`.

# 🔌 Embedded Test

`AutoLink/test/test_embedded/test_embedded.ino` is a single-board
self-loopback sketch. Wire **GPIO17 (TX) → GPIO16 (RX)** on the same ESP32,
flash, open the serial monitor. It exercises the `AutoLink` facade —
construction, the Stream / state / stats / error APIs, `blinkWait` paths
(12 facade checks) — without needing a second board.

For a real two-board end-to-end test, flash `examples/PingPong/Ping.ino`
on one ESP32 and `Pong.ino` on the other, then watch the serial
throughput logs.

# 🆘 Troubleshooting

**`make` says `cannot find MockHal.h`** — you're not in `test_desktop/`.
The `MockHal.h` is local to the test directory; the Makefile expects you
to run from there.

**`make coverage` ends with `cannot open notes file`** — a previous
coverage run was interrupted. `make clean` and retry.

**`make test_asan` hangs for >60 s on first run** — the rebuild is slow
because ASan is link-time heavy. Subsequent runs are faster.

**A test fails only under ASan** — that's a real bug. The sanitizer
found a use-after-free, buffer overflow, or UB that the normal build
missed. File an issue with the ASan stack trace.

**Coverage numbers are lower than expected** — check the `Graph:` field in
the relevant `.gcov` file. If it points to a test binary's gcno instead
of the canonical source binary, edit the `src_for` map in
`coverage_merge.sh`.
