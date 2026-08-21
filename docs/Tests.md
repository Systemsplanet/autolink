# AutoLink Tests

Everything below the ESP-IDF boundary runs on the host, because `Link` reaches the
hardware only through `IHal`. That is what makes the protocol testable at all — see
`docs/developer.md` for the principle.

## Layout

| Path | Purpose |
|---|---|
| `test/test_desktop/` | Host **unit** tests. Subsecond each. |
| `test/itest/test_desktop/` | Host **integration** tests (two-Link loopback, noise, loss sweeps). Seconds each. |
| `test/itest/test_embedded/` | On-hardware self-loopback sketch, cross-compiled. |
| `test/common/` | `MockHal.h` (the `IHal` mock, injectable clock, pipe + fault injection), `WireSim.h` (two-node simulator), the friend shims. |
| `test/scripts/` | Helpers, grouped by role: `common/`, `coverage/`, `env/`. |
| `test/Makefile` | Orchestrator: `test`, `itest`, `all`, `clean`. |

The unit tests mirror the source tree: `src/al/<mod>/<Src>.cpp` →
`test/test_desktop/al/<mod>/<Src>Test.cpp`. The one exception is `al/pingpong/`
(`Ping` / `Pong` are `#ifdef ARDUINO`; the loopback tests exercise the same
protocol).

## Running

```bash
cd test
make test     # unit suite
make itest    # host integration suite  (allow 60 s — the CI 30 s ceiling truncates it)
make all      # both, with a combined summary
make clean
```

Each target prints a per-suite `[PASS]/[FAIL] <name> <ms> <bytes> rss=<KiB>` line
and a summary block, and exits non-zero on any failure. One suite at a time:

```bash
make -C test/test_desktop test_crc
make -C test/itest/test_desktop loopback
```

## Sanitizers and coverage

```bash
cd test/test_desktop && make test_asan    # -fsanitize=address,undefined
cd test/test_desktop && make coverage     # gcov line + branch -> coverage/coverage.txt
```

A failure that appears **only** under ASan is a real bug — a use-after-free,
overflow, or UB the normal build happened not to trip.

The coverage merger picks one canonical `.gcda` per source and runs `gcov -b`. It
reads a manifest generated from the Makefile, so a new build rule is enough; the
manifest self-test fails if a suite slips through.

## The rules that matter

- **Every fix gets a regression test that fails when the fix is reverted.** Toggle
  off → red, toggle on → green. **Green/green means the test is useless** — it is
  measuring something other than the bug. This is the single most important rule in
  the suite, and the one that has been violated most often.
- **No wall-clock waiting.** No `sleep()`, no busy-wait on a deadline. Drive time
  through `MockHal::pumpClock` / `runFor`. Anything that cannot stay subsecond goes
  under `test/itest/`.
- **Decisions are pure functions, tested exhaustively as a table.** `decideHealth`,
  `decideArqSlot`, `decideGbnBackoff`, `decideGbnResendCap`,
  `decideGbnDropOnMaxRetx`, `classifyGap` — all take values and return an enum, so
  the table test is the whole story and the runtime test only has to prove the
  caller is wired to the right helper.
- **Reach internals through the friend shims** (`LinkTestAccessor`,
  `AutoLinkTestAccessor`), never by putting a `test_*` hook on the public API.
  `TestAccessorStructureTest` pins that boundary.
- **Don't discard the return value** of the thing you are testing.

## Adding a test

1. Put it in the file that mirrors the source. New functional area of `Link`? New
   `<Class><Area>Test.cpp` under the matching directory. Exception:
   `al/link/field89_1_6/`, `field89_7_12/`, `field91/`, `field92/` hold one
   `.cpp` per pin from a single field-log-driven review round (AL89 through
   AL92) rather than one per functional area — the split points are by which
   review batch found the issue, not by subsystem. `field89_1_6/` and
   `field89_7_12/` additionally share
   `field89_1_6/FieldWedgeFixes89Common.h` via a `-I` flag added only to
   those suites' Makefile rules, and `field89_7_12/FieldWedgeFixes89Main.cpp`
   is the shared entry point for both halves.
2. One `test_<thing>()` per case; `main()` lists them in order. Print a
   `=== Test: ... ===` banner and `PASS`; let `assert` do the failing.
3. Protocol tests use `MockHal.h`:

   ```cpp
   #include "MockHal.h"
   using namespace autolink;

   void test_my_thing() {
       MockHal mHal, sHal;
       AutoLinkConfig cfg;
       ArqCache ca, cb;
       Link a(mHal, ca, /*isMaster=*/true, cfg);
       Link b(sHal, cb, /*isMaster=*/false, cfg);
       negotiate_to_ok(a, b, mHal, sHal);
       // ... exercise a, observe b ...
   }
   ```

   Two-node scenarios that need a real wire (loss, reordering, both timers running)
   use `WireSim.h` instead.
4. Add the build rule and the binary to `TEST_BINS` in
   `test/test_desktop/Makefile`.
5. **Verify the toggle.** Revert the fix, watch the test go red, restore it. If it
   stays green, it is not pinning the bug — rewrite it.

## Embedded

`test/itest/test_embedded/test_embedded.ino` is a single-board self-loopback:
wire **GPIO17 (TX) → GPIO16 (RX)** on one ESP32, flash, watch the serial monitor.
It exercises the facade — construction, byte-stream, state, stats, error control —
with no second board. For a real two-board run, flash `examples/PingPong/Ping.ino`
and `Pong.ino` and watch the throughput logs.

## Troubleshooting

| Symptom | Cause |
|---|---|
| `cannot find MockHal.h` | Wrong directory. Each Makefile expects to be run from its own. |
| `make coverage`: `cannot open notes file` | An interrupted coverage run. `make clean`, retry. |
| Coverage lower than expected | The `Graph:` field in the `.gcov` points at a test binary's gcno instead of the canonical source binary — check the suite's build rule uses the right `$(LINK_SRC)` / `$(AUTOLINK_SRC)`. |
| A toggle test won't go red | The test doesn't pin the bug. Rewrite it. |
