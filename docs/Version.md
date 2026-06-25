# 📅 AutoLink Version History

All releases, most recent first.
## v5.3.70

**coverage_merge.sh and TEST_BINS drift fix**

`test/test_desktop/coverage_merge.sh` carried a
hardcoded `src_for` map naming which `run_test_*`
binaries link each library source. The map had
drifted from `TEST_BINS`: it still listed
`run_test_alink_negotiation` and
`run_test_alink_watchdog` (binaries that no
longer exist) and was missing every suite added
since the map was last touched. Adding a new
suite to `TEST_BINS` did not extend coverage
unless someone also hand-edited the shell
script, and nobody noticed because gcov's
"merged" report silently dropped the missing
.binaries' branches.

Replaced the hardcoded map with a manifest
generator (`coverage_manifest.py`) the Makefile
runs as part of `make coverage`. The generator
scans the per-suite build rules, expands
`$(LINK_SRC)` / `$(AUTOLINK_SRC)` / etc. using
the Makefile's own variable definitions, and
emits a shell-sourceable file listing
`src_for_<basename>="bin1 bin2 ..."` for every
library source plus a `TEST_BINS` echo that
the test-file branch of `coverage_merge.sh`
iterates instead of a hardcoded list. Adding a
new suite to `TEST_BINS` now extends coverage
automatically; no edit to `coverage_merge.sh`
needed.

`coverage_merge.sh` itself: switched
`src_for=()` to `declare -A src_for=()` (the
former was being silently coerced to an
indexed array, turning string keys into
integer indices), and replaced the static
`! -name "..."` prune list with one built
from the same `src_for_*` keys, so adding a
new library source also auto-extends the
keep-list.

Pinned by `test/test_desktop/test_coverage_manifest.py`
— a self-test that fails the moment a binary
in `TEST_BINS` fails to contribute to any
`src_for_<basename>` entry (the exact drift
shape AGENTS.md rule 4 warns about), and
that synthesises a fake Makefile with a
`run_test_zzz` linking `$(AUTOLINK_SRC)` to
prove the new suite shows up in every
relevant `src_for_*` entry. Wire up via
`make test_coverage_manifest`.

**Regression coverage**
- `make test_coverage_manifest` (new). Self-test
  for the manifest generator. Three cases:
  (1) every real `TEST_BINS` binary contributes
  to at least one `src_for_*` entry, (2) a
  synthesised `run_test_zzz` linking
  `$(AUTOLINK_SRC)` shows up in all 11 expected
  `src_for_*` keys, (3) test-tree paths under
  `al/` are filtered out so they cannot shadow
  library sources sharing a basename.

**Disclosed limitations**
None.

---

## v5.3.69

**pretty_print.py: pip install fallback for clang-format (sandbox / minimal containers)**

`_install_clang_format()` previously tried only `apt-get`
(sudo + no-sudo variants) and `brew`. On a stripped
container with no `clang-format` apt package and no
package-source coverage, the script bailed with
`skipped: clang-format not installed` and silently
no-op'd every file.

Added a `pip` fallback: detects `pip` / `pip3`,
tries `--break-system-packages` first (Debian 12+,
Ubuntu 23.04+ enforce PEP 668 by default), then
plain `pip install`, then `--user --break-system-
packages`, then `--user`. After each successful
install the script also looks in `~/.local/bin/`
(prepending it to PATH if found) because a
`--user` install running as root may have placed
the wrapper off-PATH even though `pip` returned
rc=0.

Verified end-to-end from a clean state: removed
both `/usr/local/bin/clang-format` and
`/root/.local/bin/clang-format`, ran
`python3 build/pretty_print.py src/AutoLink.cpp` —
the script self-installed clang-format 22.1.5 and
formatted the file. `build/test_pretty_print.py`:
16 / 16 still PASS. `make test`: 30 / 30 in ~4.3 s.
---

## v5.3.68

**repackage: bumped to a new zip version per the user's
repackaging policy (auto-bump on every re-emit).**

No source / wire / behavioural change vs v5.3.67. The
test-layout housekeeping (8 loose files moved under `al/`,
Makefile build rules updated, `docs/Tests.md` table
corrected) is the v5.3.67 change-set — this entry exists
so the zip on the wire has a fresh trailing segment and
the version contract stays unambiguous for downstream
consumers.

`make test`: 30 / 30 in ~4 s. `make itest`: 3 / 3 in
~40 s.
---

## v5.3.67

**test layout: 8 loose test files moved under al/ (rule 20)**

`test/test_desktop/` had eight `*Test.cpp` files sitting at
the root that violated AGENTS.md rule 20 (test files mirror
the source package). They were all facade-level, facade-
behavioural, or source-level checks that exercise code
anchored in `src/AutoLink.cpp` or read across multiple
`src/al/<sub>/` modules, so each one is placed directly
under `al/` rather than under a single sub:

- `AutoLinkTest.cpp`, `AutoLinkFacadeTest.cpp` — `AutoLink`
  facade suite
- `ClockInjectionTest.cpp`, `SyncModeTest.cpp`,
  `WireSimClosedLoopTest.cpp` — protocol-timer suites
  driven through the facade
- `CompileCheckTest.cpp`, `EspIdfErrorEtiquetteTest.cpp`,
  `VersionFreeSourceTest.cpp` — source-level pins that
  walk the whole `src/` tree

Each `run_test_X` build rule in `test/test_desktop/Makefile`
was updated to point at the new `al/<File>.cpp` path.
`docs/Tests.md` table was updated; the unit-binary count
(`22` → `30`) was also corrected. No source / wire / RAM
impact — every test still compiles and passes against the
relocated path. `make test` clean: 30 / 30 in ~4.4 s.
---

## v5.3.66

**`test/test_desktop/Makefile`: 14 missing `test_X` shorthand targets added; broken `test_autolink` recipe corrected.**

Two regressions in the host-test makefile:

1. `test_autolink` passed `test_alink_web` as a shell
   argument to `./run_test_autolink`. The prereq list
   still forced `run_test_autolink` to build, but the
   recipe body was garbage. Fixed by dropping the
   spurious argument; `test_alink_web` remains an
   independent entry below it.
2. 14 suites had `run_test_X` build rules in `TEST_BINS`
   but no `test_X:` shorthand target and no `.PHONY`
   entry: `alink_arq`, `alink_facade`, `arq_cache`,
   `clock_injection`, `compile_check`,
   `esp_idf_error_etiquette`, `linkdecision`,
   `linkreorder`, `ping_resume_source`,
   `pingpong_structure`, `accessor_structure`,
   `sync_mode`, `version_free_source`,
   `wiresim_closedloop`. Added the standard two-line
   shorthand for each and registered them in `.PHONY`.

No source / wire / RAM impact. `make -n` dry-run
confirms every new target resolves to its existing
`run_test_X` build rule + run command.

---
---

## v5.3.65

**strict comment policy: no version refs in comments**

Rule 8 of AGENTS.md now bans referencing a version
number in any source comment. Swept `src/`, `include/`,
`test/`, `itest/`, `build/`, and `.github/workflows/` —
removed `// v5.3.X:` style anchors and `(v5.3.X)` from
`std::cout` banners across ~25 files. Changelog
(`docs/Version.md`), `library.properties`, and
`idf_component.yml` are exempt — they ARE the version
source of truth. Test JSON fixtures that stringify a
version string (e.g. AutoLinkWebTest) are exempt — that
is test data, not a comment.
---

## v5.3.64

**IHal::lock/unlock: drop false const, reg test pins it**

`IHal::lock()` and `unlock()` were `const`, claiming
the HAL state is unchanged while in fact a mutex
(`FreeRTOS` semaphore on EspHal, `std::mutex` on
MockHal) is mutated. Removed the `const` from the
pure-virtual decls and from all three overrides
(`EspHal`, `MockHal`, `EspHalStub`). Regression
test: `MockHalTest::test_lock_unlock_are_nonconst`
uses `static_assert` on the member-function-pointer
type so re-introducing `const` fails the build, not
just a test at runtime.
---

## v5.3.63

**rename version-named test files**

`LinkV53Test.cpp`, `LinkV531Test.cpp`,
`LinkV531NeverLeaveP1Test.cpp` were named after the
version that introduced the behavior they pin. The
behavior is now permanent, so the filenames should
describe the invariant, not the version. Renamed:

- `LinkV53Test.cpp` → `LinkBaudPreferenceTest.cpp`
  (preferredBaud on lock, re-sweep from preferredBaud,
  errRateWindow drop, baudRetries cleared on reset,
  short-message coalescing).
- `LinkV531Test.cpp` → `LinkSweepPhaseTest.cpp`
  (3-phase sweep transitions, PONG_CMD decode,
  heartbeat-miss drop time, dwell caps, phase banners).
- `LinkV531NeverLeaveP1Test.cpp` →
  `LinkSweepP1GuardTest.cpp` (master and pong
  must not leave P1 until connected; BREAK-in-P1
  restarts at the slowest baud).

Makefile targets, `docs/Tests.md`, and the
`al/pingpong/README.md` unit list were updated in
lockstep. The internal `test_v53_*` / `test_v531_*`
function names are unchanged (no behavior covered,
the names are just historical at this point and
the rename ships the invariant not the version).

No regression test: pure rename, full test suite
must stay green.
---

## v5.3.62

**Test hooks moved off the public API of `Link` and `AutoLink` into friend shim classes (`LinkTestAccessor`, `AutoLinkTestAccessor`) under `test/common/`.**

`Link` had six `test_*` methods on its public surface (`test_markAckedPending`, `test_sendMsgBegin`, `test_sendMsgStillWaiting`, `test_reorderSlotInUse`, `test_reorderSlotLen`, `arqCacheForTest`) and a public `syncAckTimeoutMsForTest`. `AutoLink` had a dozen more: `linkForTest`, `arqCacheForTest`, `arqCacheSizeForTest`, `test_arqCache_put`, `test_arqCache_hasRoom`, `test_arqPoolSize`, `test_arqCache_freeBySeq`, `test_arqCache_retx`, `test_arqCache_findBySeq`, `test_markAckedPending`, `test_sendMsgBeginForTest`, `test_sendMsgStillWaitingForTest`, `syncAckTimeoutMsForTest`, plus three declared-but-never-called (`test_arqFillPoolForTest`, `test_arqEmptyPoolForTest`, `test_arqFillPendingForTest`). All of them were honest internals: a way for the host build to drive the link / ARQ cache / reorder buffer into states the production wire never produces. The leak made the public headers look like a test API — every new caller who read `AutoLink.h` would have to mentally skip ~50 lines of test plumbing before finding the real surface.

### What moved

- **`test/common/LinkTestAccessor.h`** — header-only
  shim, `friend class LinkTestAccessor;` inside
  `Link`. Exposes `markAckedPending`,
  `sendMsgBegin`, `sendMsgStillWaiting`,
  `syncAckTimeoutMs`, `reorderSlotInUse`,
  `reorderSlotLen`, `arqCache`. Builds only under
  `-DAUTOLINK_HOST_TEST`; never reachable from a
  production Arduino sketch.
- **`test/common/AutoLinkTestAccessor.h`** — shim,
  `friend class AutoLinkTestAccessor;` inside
  `AutoLink`. Exposes `arqCacheSize`, `link`,
  `arqCache`, `arqCachePut`, `arqCacheHasRoom`,
  `arqPoolSize`, `arqCacheFreeBySeq`,
  `arqCacheRetx`, `arqCacheFindBySeq`,
  `markAckedPending`, `sendMsgBeginForTest`,
  `sendMsgStillWaitingForTest`,
  `syncAckTimeoutMsForTest`. Wraps a
  `LinkTestAccessor` so the facade never reaches
  into Link's privates directly — it composes
  through the inner shim.
- **`Link`** — all six test methods + the
  `arqCacheForTest()` getter are now in a
  `private:` block. `syncAckTimeoutMsForTest()`
  is gone; the shim reads `cfg.syncAckTimeoutMs`
  via the friend declaration. `arqCache_` stays
  private; the shim exposes the raw pointer
  through its own `arqCache()` getter.
- **`AutoLink`** — all thirteen test methods +
  the three dead-code fillers are now in a
  `private:` block. Production sketches no longer
  see `linkForTest`, `arqCacheForTest`, or any
  `test_*` symbol on the facade.
- **Test call sites** — every host test that
  called `link.test_arqCache_put(...)` /
  `link.test_sendMsgBeginForTest(...)` /
  `b.test_reorderSlotInUse(...)` /
  `a_.linkForTest()->getStats(...)` now
  constructs a shim and calls through it:
  `AutoLinkTestAccessor t(link);
   t.arqCachePut(...)`,
  `LinkTestAccessor t(b); t.reorderSlotInUse(...)`,
  `AutoLinkTestAccessor at(a_);
   at.link()->getStats(...)`. The shim is the
  only path tests reach internals through; no
  test reaches into Link's privates directly
  anymore.
- **`WireSim::bytesTransferredAtoB` /
  `bytesTransferredBtoA` / `pendingCountA` /
  `pendingCountB`** — all four are `const`
  methods; the shim constructors accept
  `const Link&` / `const AutoLink&` so the
  const binding compiles. The
  `const_cast<>` is inside the shim; callers
  see a clean shim that works in const
  context.

### Why

The old shape was honest but noisy. A new
caller reading `include/AutoLink.h` saw
~50 lines of test plumbing mixed into the
public surface, then had to decide which
of those methods were safe to call from
production code. None of them are: every
`test_*` is a way to drive the internals
into a state the wire would never produce
on its own. The boundary was real but
invisible.

The shim makes the boundary explicit.
Production sketches see only the real API
(`begin`, `send`, `recv`, `setMode`,
`setTxDelayMs`, `getStats`, etc.). Host
tests see a single shim class with a
narrow, named API (`arqCachePut` for "put
into the ARQ cache", `sendMsgBegin` for
"start a SYNC-mode send without blocking
on the ACK"). The shim is `friend` of the
production class; the production class
sees the shim only by name. Removing the
friend declaration breaks every test that
constructs a shim — the compile signal
catches a regression immediately.

### Wire format

Unchanged. Public API (production
methods) is byte-identical to v5.3.61.
No header in `include/` moves. The shim
headers live under `test/common/`, gated
by `#ifdef AUTOLINK_HOST_TEST`, so the
Arduino build never sees them.

### Regression coverage

**New source-level pin:**
`test/test_desktop/al/link/TestAccessorStructureTest.cpp`
— 5 test functions reading
`src/al/link/Link.h` and
`include/AutoLink.h` and asserting:

1. The literal `test_markAckedPending` is
   not in the public section of `Link`.
2. `test_sendMsgBegin`,
   `test_sendMsgStillWaiting`,
   `test_reorderSlotInUse`,
   `test_reorderSlotLen`, `arqCacheForTest`
   are all absent from the public section of
   `Link`.
3. `test_arqCache_put`, `test_arqCache_hasRoom`,
   `test_arqPoolSize`, `test_arqCache_freeBySeq`,
   `test_arqCache_retx`, `test_arqCache_findBySeq`,
   `test_markAckedPending`,
   `test_sendMsgBeginForTest`,
   `test_sendMsgStillWaitingForTest`,
   `syncAckTimeoutMsForTest`, `linkForTest`,
   `arqCacheForTest`, `arqCacheSizeForTest`
   are all absent from the public section of
   `AutoLink`.
4. Catch-all regex: no public `test_*` or
   `*_ForTest` method declaration slips past
   the literal list. New hooks added to the
   public surface are caught on the next test
   run.
5. `friend class LinkTestAccessor;` is in
   `Link.h`; `friend class
   AutoLinkTestAccessor;` is in `AutoLink.h`.
   Removing either breaks every host test
   that constructs a shim.

Reverting the fix (putting any of the
`test_*` / `*_ForTest` methods back on
the public surface) fails tests 1, 2, 3
and the catch-all in test 4
simultaneously. Removing the friend
declarations breaks the compile — test 5
won't even reach the assertion.

### Disclosed limitations

- The `WireSim::nodeAForTest()` /
  `nodeBForTest()` accessors stay as-is.
  They're on the test fixture in
  `test/common/`, not on the production
  facade. The user complaint was about
  the production class surface, and the
  fixture's purpose is to hand tests a
  reference to the AutoLink. Renaming
  them would churn every itest for no
  production benefit.
- The shim is header-only and inline.
  The shim's methods are one-liners; an
  out-of-line `.cpp` would add a build
  step and zero perf. If the shim grows
  non-trivial logic, the .cpp split is
  a clean follow-up.
- `ArqCache` already has its own
  `test_*` methods (`testFillPool`,
  `testEmptyPool`, `testFillSlots`,
  `testPut`, `testRetx`) on the public
  surface. The shim reaches them through
  the cache pointer; pushing them
  behind a `friend` is out of scope for
  this change and would touch the
  `ArqCacheTest.cpp` callers.

### Result

- 29 / 29 unit suites (was 28 / 28; 1 new
  suite `run_test_accessor_structure`).
- 3 / 3 itest suites (unchanged).
- `verify_build.sh` not run in this
  environment (no arduino-cli toolchain
  pre-installed; would require
  `build/build_env.sh` to provision).
  The Arduino build does not see the
  shim headers (guarded by
  `#ifdef AUTOLINK_HOST_TEST`) and the
  production public surface shrank, so
  the cross-compile is expected to stay
  clean.
- 0 bytes added to RAM (shims live in
  test headers; production code paths
  unchanged).
- `Link.h` and `AutoLink.h` public
  surface: ~50 lines of test plumbing
  moved off the public side into the
  shim.

---
---

## v5.3.61

**`PingPongBase` split: `logStats` + `resetStatBaseline` are now free functions, and `setupCommon` is gone — replaced by three discrete steps (`initSerial` / `bringUpLink` / `startWebMonitor`) that `Ping::setup` and `Pong::setup` sequence themselves.**

`PingPongBase` was a "shared config" struct that bundled three unrelated concerns: the comms handle, the rolling tx/rx byte-rate baseline used by `logStats`, and the app-lifecycle bootstrap. `setupCommon()` did Serial init + boot log + first blink + link bring-up + version log + WiFi decision + web-monitor start + final blink in one method; `logStats()` formatted the rolling rate inside the same struct that held the `AutoLink` it was measuring.

### What moved

- **`logStats(Log&, tag, AutoLink&, StatBaseline&, echo, mismatch)`**
  — free function in `src/al/pingpong/PingPongBase.h`. Takes
  the comms handle and a `StatBaseline` (the rate-window
  state) by reference. Caller owns the baseline so the
  window survives the path that resets it.
- **`resetStatBaseline(StatBaseline&)`** — free function,
  same header. The no-arg member that previously cleared
  `tStat_/lastTx_/lastRx_` is gone.
- **`StatBaseline { uint32_t tMs; uint64_t lastTx, lastRx; }`**
  — new POD struct. Owned by `Ping` and `Pong` (one member
  each) — the rolling window lives with the loop that resets
  it, not with the comms handle.
- **`initSerial(Log&, debugBaud, role, ssid)`** — free
  function. Log level + `Serial.begin` + boot banner. Step 1
  of the lifecycle.
- **`bringUpLink(Log&, AutoLink&)`** — free function. First
  blink + `comm.begin()` + "link layer up" log + version log
  + final blink. Step 2.
- **`startWebMonitor(Log&, AutoLinkWeb&, role, ssid, password, port)`**
  — free function. The conditional ("only if ssid is set")
  is now the caller's call: `Ping::setup` / `Pong::setup`
  decide whether to invoke it. Step 3.
- **`PingPongBase`** keeps only the truly shared state: the
  `AutoLink` handle, the `AutoLinkWeb` monitor, the WiFi
  config (ssid/password/port), the debug baud, the role
  flag, the log reference, the shared RX buffer, and the
  `wasReady_` flag. The rate-window fields (`tStat_`,
  `lastTx_`, `lastRx_`) and the three old methods are gone.

### Why

The old `setupCommon()` gave the caller no lever to skip
the WiFi step, no way to interleave app init between Serial
up and link up, and no place to plug a custom rate-window
baseline. The new shape lets a sketch:
- skip the web monitor (just don't call `startWebMonitor`),
- reset the rate baseline at any point (just hold a
  `StatBaseline`),
- write the rate into a different sink (call `logStats`
  from any class that has an `AutoLink&`).

Diagnostics no longer hides inside the struct that holds
the link it's measuring. The split mirrors the
`Log::log()` / `Link::phase1ArmMs()` pattern of pure
diagnostics out of the production state holder.

### Wire format

Unchanged. Public API (`Ping` / `Pong` ctors, `setup`,
`loop`, `setPaused`, `setFillMode`, etc.) is
byte-identical. No header in `include/` moves.

### Regression coverage

**New source-level pin:**
`test/test_desktop/al/pingpong/PingPongStructureTest.cpp`
— 7 test functions reading `PingPongBase.h`, `Ping.h`,
`Pong.h` and asserting:

1. `setupCommon` symbol is not a member of `PingPongBase`
   (split into steps).
2. `logStats` / `resetStatBaseline` are not members of
   `PingPongBase`; the struct body does not own
   `tStat_` / `lastTx_` / `lastRx_`.
3. `initSerial` / `bringUpLink` / `startWebMonitor` /
   `StatBaseline` are present at namespace scope.
4. `Ping::setup` calls the three steps in order
   (initSerial -> bringUpLink -> startWebMonitor) and
   does not call `setupCommon`.
5. `Pong::setup` calls the three steps in order and does
   not call `setupCommon`.
6. Both `Ping` and `Pong` call the free `logStats(...)`,
   not `base_.logStats(...)`.
7. Both `Ping` and `Pong` own a `StatBaseline stat_`
   member and call `resetStatBaseline(stat_)`, not
   `base_.resetStatBaseline()`.

Reverting the fix (putting `setupCommon` / `logStats` /
`resetStatBaseline` back as members on `PingPongBase`)
fails tests 1, 2, 4, 5 simultaneously. Reverting just
the logStats split fails tests 2, 6, 7. Reverting just
the setup split fails tests 1, 4, 5.

**Updated source-level pin:**
`test/test_desktop/al/pingpong/PingResumeSourceTest.cpp`
— `test_setPaused_false_clears_pending_table` was
looking for the literal `resetStatBaseline()` call form
(the v5.3.55 baseline-reset member). Now looks for
`resetStatBaseline(stat_)` to match the v5.3.61 free
function with the explicit `stat_` argument. The
ordering invariant (clear before reset) and the
proactive-vs-reactive comment contract are unchanged.

### Disclosed limitations

- `PingPongBase` still holds the comms handle, the web
  monitor, the WiFi config, and the `wasReady_` flag —
  these are legitimately shared between the Ping and
  Pong roles. The split is "diagnostics out, comms stay".
- The header comment in `PingPongBase.h` still references
  the word "setupCommon" once in a historical note ("was
  split into discrete steps"). The structure test scopes
  its `setupCommon`-must-not-appear check to the struct
  body, not the file, so historical text in the comment
  block doesn't trip the pin.

### Result

- 28 / 28 unit suites (was 27 / 27; 1 new suite
  `run_test_pingpong_structure`).
- 3 / 3 itest suites (unchanged).
- `verify_build.sh` clean compile against
  esp32:esp32@3.3.5.
- 0 bytes added to RAM (44008 B unchanged — logStats
  was inline-fitted before, still inline-fitted).
- `PingPongBase.h`: 95 -> 158 LoC (more docs +
  four free-function declarations; structure-test
  coverage is the trade).

---
---

## v5.3.60

**`BreakBeforeBraces: Linux` -> `Attach`. Every opening brace now sits on the same line as its header.**

The Linux-brace rule (function/namespace/class/struct/union
opening brace on its own line) is gone. Function definitions,
control-flow blocks, namespace and class bodies, switch
cases, and lambdas all use the attached form. Combined with
v5.3.59's `AllowShortFunctionsOnASingleLine: All`, the
project's canonical one-liner is `void f() { stmt; }` and the
canonical multi-statement form is `void f() {\n    stmt;\n}`.

The v5.3.59 disclosure about `Log::setSink` (2-stmt body,
brace on header line under `All`) is now the project-wide
shape: every multi-statement function, class, namespace,
if-block, and for/while/switch keeps the brace on the header
line. The Linux-style "opening brace on its own line" no
longer appears anywhere in the tree.

### Files
- `.clang-format` -- `Linux` -> `Attach`
- 74 `.cpp` / `.h` files reformatted by
  `build/pretty_print.py`

### Tests
- 27 / 27 host unit suites pass
- 3 / 3 itest suites pass
- `verify_build.sh` clean compile against
  esp32:esp32@3.3.5
- Rule 15 smoke compile (one-file `.ino` that
  `#include`s every public header) clean

### Disclosed limitations
- Control-flow blocks (`if`, `for`, `while`, `switch`)
  now use the attached form (`if (x) {`). This is the
  full `Attach` style, not a function-only variant --
  clang-format has no option to move only function
  braces. If the user wanted only function-definition
  braces attached, the only way is a custom pass; see
  v5.3.59's options list for the trade-off.

---
---

## v5.3.59

**Single-statement function bodies now collapse to one line via `AllowShortFunctionsOnASingleLine: All`.**

`pretty_print.py` and the project `.clang-format` were
updated so the canonical shape for a one-liner
function is `void name(args) { stmt; }` instead of the
3-line Linux-brace form. Multi-statement bodies keep
the multi-line `BreakBeforeBraces: Linux` form
unchanged; only bodies short enough to fit on one
line collapse, so file-scope diffs are concentrated on
single-call getters, setters, and passthroughs
(`Link::phase1ArmMs`, `Link::flush`, `Link::peek`,
`Log::clearSink`, etc.). The two-statement setter
`Log::setSink` was the only visible side effect on a
non-one-liner function: its opening brace now sits on
the header line because the body itself still has
multiple statements.

The change is one line in `.clang-format`. No source
files needed hand-editing; `build/pretty_print.py` was
run over the full tree to apply it.

### Files
- `.clang-format` -- `Inline` -> `All`
- `build/pretty_print.py` -- docstring updated to
  mention the new behavior; behavior unchanged
- 74 `.cpp` / `.h` files reformatted by
  `build/pretty_print.py`

### Tests
- 27 / 27 host unit suites pass
- 3 / 3 itest suites pass
- `verify_build.sh` clean compile against
  esp32:esp32@3.3.5
- Rule 15 smoke compile (one-file `.ino` that
  `#include`s every public header) clean

### Disclosed limitations
- Two-statement functions whose body cannot fit on one
  line now have the opening brace on the header line
  (the previous Linux-brace style is overridden when
  clang-format considers a body "short"). This is the
  documented behavior of `AllowShortFunctionsOnASingleLine:
---

## v5.3.58

**`EspHal` / `EspBlinkHal` stubs moved out of `include/AutoLink.h` into `test/common/EspHalStub.h`. The public HAL boundary stops leaking into the host build.**

The v5.3.57 header carried two
`#ifdef AUTOLINK_HOST_TEST`-gated
struct definitions for the ESP HAL
types directly inside `AutoLink.h`.
The production header doubled as
the host-side HAL stub, so
`include/AutoLink.h` was the only
file in the tree that contained
working `IHal` impls outside
`src/al/hal/`. Including the
header from a test that constructs
`AutoLink(0, 16, 17, ...)` worked,
but anyone who `#include`d it
without `AUTOLINK_HOST_TEST`
pulled in the real `EspHal.cpp`
via the other branch — and the
host test infrastructure was
coupled to a public header.

### What moved

- **`test/common/EspHalStub.h`** —
  new file. The two no-op
  `IHal`-derived structs (plus
  `EspBlinkHal` shim) live here.
  Self-guards on
  `!AUTOLINK_HOST_TEST` so the
  production build can never pick
  it up by accident.
- **`include/AutoLink.h`** — the
  host branch now forward-declares
  `struct EspHal` instead of
  defining it. The production
  branch still pulls
  `al/hal/EspHal.h` (real ESP-IDF
  impl). The public ctor body is
  declared in the header and
  defined out-of-line in
  `src/AutoLink.cpp` (one body per
  build mode), so the header only
  needs the forward declaration to
  compile.
- **`src/AutoLink.cpp`** — the
  public constructor body for both
  `ARDUINO` and `AUTOLINK_HOST_TEST`
  moved here. The host branch
  includes `EspHalStub.h` via the
  `-I../common` flag the test
  Makefile already sets. Device
  build doesn't see `EspHalStub.h`
  at all.
- **`test/test_desktop/AutoLinkTest.cpp` /
  `AutoLinkFacadeTest.cpp` /
  `al/link/LinkArqTest.cpp`** —
  each got a `#include "EspHalStub.h"`
  immediately after the
  `AutoLink.h` include. They are
  the three unit suites that
  construct the public ctor with
  real `uart_port_t`-style args.
  `WireSim`-driven suites and the
  itest loopbacks use the
  `AutoLink(IHal*, ...)` ctor and
  do not need the stub.

### Regression test

The boundary change is guarded by
the existing 27 unit suites (every
suite that links `AutoLink.cpp`
and exercises the public ctor
covers it). If the forward
declaration is removed, every
host build fails to compile with
`'EspHal' was not declared in this
scope`. If the host ctor body in
`AutoLink.cpp` is removed, the
linker complains about the
undefined `AutoLink::AutoLink(...)`
reference.

### Disclosed limitations

None. Production wire format, ARQ
state machine, and FreeRTOS task
plumbing are untouched. The
forward declaration is the only
new HAL knowledge the public
header carries.
---

## v5.3.57

**ARQ cache moved out of the `AutoLink`
facade into its own `ArqCache` class
behind an `IArqCache` interface.**

The cache state (`pending_[]`,
`arqPool_[]`, `arqPoolUsed_[]`,
`pendingCount_`) used to live on the
facade while the link layer drove
timing and retx writes. Bridging the
two required a 6-pointer callback
chain + 5 static trampolines that
`static_cast<AutoLink*>(ctx)`'d back
to the facade on every cache call.
A retx read in `Link::onTimer` left
the link's lock, walked the chain
back into the facade, then re-entered
the link to call `resendCobsFrame_unlocked`.

### What moved

- **`IArqCache`** (`src/al/link/IArqCache.h`)
  — pure abstract interface
  (`hasRoom / insert / freeBySeq /
  peekForRetx / clearAll / slotInUse /
  size`). Mirrors the `IHal` pattern
  (AGENTS #10: virtual only at the
  user-extension boundary).
- **`ArqCache`** (`src/al/link/ArqCache.{h,cpp}`)
  — concrete impl with the pool, the
  256-slot table, the invariants, and
  the test hooks.

### What `Link` keeps

- A single `IArqCache *arqCache_` raw
  pointer, set via `Link::setArqCache()`.
  Lifetime: the cache must outlive
  the link. `AutoLink` owns the cache
  by value and constructs the link
  first, so dtor order is safe.
- All call sites that used to call
  `arqRetxCallback_(seq, ctx)` /
  `arqAckCallback_(seq, ctx)` /
  `arqCacheHasRoomCallback_(ctx)` /
  `arqCacheInsertCallback_(...)` /
  `arqCacheClearAllCallback_(ctx)`
  now call `arqCache_->hasRoom()` etc.
  directly. Six function pointers,
  the void* ctx, the five `using
  Arq*Callback` aliases, and the two
  setters (`setArqHooks` /
  `setArqCacheHooks` / `setLinkResetHook`)
  are gone.

### What `Link::onTimer` retx looks like now

```
if (hasPendingRetx_ && arqCache_) {
    uint8_t base = pendingRetxBase_;
    hasPendingRetx_ = false;
    if (!arqCache_->slotInUse(base)) {
        // cache miss: log + no-op
    } else if (arqCache_->peekForRetx(base, &buf, &len)) {
        resendCobsFrame_unlocked(base, buf, len);
    } else {
        // keepalive slot: resend 0 bytes
        resendCobsFrame_unlocked(base, nullptr, 0);
    }
}
```

The dead-bool-return "drop the link"
path is removed (always returned
`false` in production; was unreachable).

### What `AutoLink` keeps

- `ArqCache arqCache_;` (by value,
  18 KB owned on the facade).
- `link->setArqCache(&arqCache_);` in
  the ctor. The five static trampolines
  (`arqAckHookTrampoline` /
  `arqRetxHookTrampoline` /
  `arqCacheHasRoomTrampoline` /
  `arqCacheInsertTrampoline` /
  `arqCacheClearAllTrampoline` /
  `linkResetHookTrampoline`) are
  deleted.
- The facade's test hooks
  (`test_arqCache_put` /
  `test_arqCache_hasRoom` /
  `test_arqFillPoolForTest` /
  `test_arqEmptyPoolForTest` /
  `test_arqFillPendingForTest` /
  `test_arqCache_freeBySeq` /
  `test_arqCache_retx` /
  `test_arqCache_findBySeq` /
  `arqCacheSizeForTest`) become
  thin forwarders to `arqCache_`.
  `test_arqHasRoomTrampoline` and
  `test_linkResetHookTrampoline` are
  deleted (no trampolines).

### Itest impact

The itest loopback tests used to
install their own 5-trampoline
`ItestArq` cache so the link's retx
path had somewhere to re-send from.
`loopback_test.cpp` and
`loopback_noise_test.cpp` now use
the production `ArqCache` directly
(`ArqCache g_pingArq;` `ArqCache
g_pongArq;` `ping.setArqCache(&g_pingArq);`).
The `ItestArq` struct and the 5
trampolines are deleted from both
files. ~80 lines of itest code
goes away. The `LinkMessageTest`
chunk-boundary test keeps its
custom `TestCache` (now derived
from `ArqCache` with `hasRoom` /
`insert` / `freeBySeq` overrides)
since it tests chunk accounting, not
the cache layout.

### Wire format

Unchanged. The `chunkCount` arg that
was threaded through the old
`ArqCacheInsertCallback` was always
`1` and always `void`-cast — dropped.

### Regression coverage

- **`test_linkarq_linkreorder_linksweep_present`**
  in `CompileCheckTest.cpp` now
  also asserts `class IArqCache` and
  `class ArqCache` are declared in
  their respective headers. Renamed
  log line to "all five classes".
- **New `test_link_has_no_arq_trampoline_pointers`**
  in `CompileCheckTest.cpp`: source-
  level pin that `Link.h` no longer
  contains any of `ArqAckCallback` /
  `ArqRetxCallback` /
  `ArqCacheHasRoomCallback` /
  `ArqCacheInsertCallback` /
  `ArqCacheClearAllCallback` /
  `LinkResetCallback` /
  `arqAckCallback_` /
  `arqRetxCallback_` /
  `arqCacheHasRoomCallback_` /
  `arqCacheInsertCallback_` /
  `arqCacheClearAllCallback_` /
  `linkResetCallback_` / `arqCtx_` /
  `setArqCacheHooks` /
  `setLinkResetHook`. Re-introducing
  any of those names into `Link.h`
  breaks the test.
- **`ArqCacheTest.cpp`** in
  `test/test_desktop/al/link/` (new
  file): pure unit tests of the
  cache in isolation. 11 test
  functions covering fresh / pool-
  exhausted / slots-full, insert +
  free roundtrip, replace (re-uses
  the pool buf), peek miss,
  keepalive slot, peek-borrow-not-
  retained-across-free, clearAll,
  pool-exhaustion-skip, oversize
  payload reject. The old
  `ArqCacheHasRoomTrampolineTest.cpp`
  is deleted (the trampoline no
  longer exists).
- `AutoLinkFacadeTest.cpp` updated
  to use `arqCacheForTest()` /
  `arqCacheForTest()->clearAll()`
  instead of
  `test_linkResetHookTrampoline`.
  `test_arqCache_retx` semantics
  changed: now returns `peekForRetx`
  hit-miss (was a dead-bool "drop
  request" return).

**Result:**
- 27 / 27 unit suites (was 27 / 27;
  1 new unit suite `ArqCacheTest`,
  1 trampoline suite removed, 2 new
  source-pin tests in
  `CompileCheckTest`).
- 3 / 3 itest suites (unchanged).
- `verify_build.sh` clean compile
  against esp32:esp32@3.3.5.
- RAM: 44008 B (unchanged — cache
  still lives on the facade, just
  behind a vtable now).
- `Link.h`: 309 → 305 LoC.
- `AutoLink.h`: 444 → 320 LoC
  (Pending struct, ARQ_CACHE_*
  constants, arqPool_/pending_/
  arqPoolUsed_, 4 private cache
  methods, 6 static trampolines
  all gone).
- `AutoLink.cpp`: 230 → 14 LoC
  (5 trampolines + cache methods
  moved to `ArqCache.cpp`).
- `ArqCache.cpp`: 196 LoC (new).
---

## v5.3.56

**`Link` god class split into `LinkArq` /
`LinkReorder` / `LinkSweep`.** `Link` is now
a coordinator that owns instances of those
and calls them; each owns its state and
delegates I/O back to `Link` via narrow
hooks.

### What moved

- **`LinkArq`** (`src/al/link/LinkArq.{h,cpp}`)
  — per-cobsSeq ACK/retx tables
  (`ackedPending_[]`, `retxCount_[]`,
  `sentAtMs_[]`, `baseSeq_[]`), `popRetransmitSlot`,
  the SYNC wait-for-ACK helper, and the
  ARQ slot decision (`Hold`/`Retx`/`Drop`).
- **`LinkReorder`** (`src/al/link/LinkReorder.{h,cpp}`)
  — the 256-slot reorder table plus
  `reorderClear_unlocked`,
  `reorderDropExpired_unlocked`,
  `reorderFlushContiguous_unlocked`.
- **`LinkSweep`** (`src/al/link/LinkSweep.{h,cpp}`)
  — sweep phase state (`sweepPhase_`,
  `phase3Baud_`, `phase3Acks_`), the dwell
  table, and the four phase-transition
  methods (`enterPhase1/2/3`, `enterResweep`)
  plus `computeDwells` and `phase1ArmMs`.
  The existing `UtilBaudSweep` (per-baud
  score accumulator) is unchanged.

### What `Link` keeps

- The wire-level state machine: `state`,
  `txSeq`, `rxSeq`, `errs`, `discCount`,
  `frameErrs`, `lastRxMs`, `lastTxMs`,
  `heartbeatPingsMissed_`, `errWindow*`,
  `preferredBaud_`, `baudRetries_`,
  `wasEverOk_`, `emptySweeps`, `swpRxBytes`,
  `rxIdx`, `rxBuf`, `rxMsgLen`, `rxMsgCrc`,
  `lckRetries`, `txBytes`, `rxBytes`, `gaps`,
  `stale`, `lostMsgs`, `pendingRetxBase_`,
  `hasPendingRetx_`, `retxNeeded_`.
- All I/O (`hw.lock/unlock/nowMs/setSpd/
  startTimer/tx/pushAppBuf/sendBreak/...`)
  and the ARQ-cache callback chain.
- The public test hooks
  (`test_reorderSlotInUse`, `test_reorderSlotLen`,
  `test_markAckedPending`,
  `test_sendMsgBegin/StillWaiting`) and the
  `pendingAcks()` / `isAcked()` accessors.

### Why

Link.h/cpp was ~1758 lines doing six
unrelated jobs (baud sweep, ARQ, reorder,
SYNC/ASYNC mode, COBS framing, keepalive).
The state tables were entangled with the
methods that read/mutated them, which made
the protocol changes in v5.3.54 and
v5.3.48 touch a large surface for what
were single-line fixes. After the split,
each concern is in one file under ~125 LoC.

### Wire format

Unchanged. No public API change. The new
classes are internal; nothing in `include/`
moves.

### Regression coverage

All 27 / 27 unit suites + 3 / 3 itest
suites + `loopback_modes` 4 / 4 still pass
verbatim — `LinkReorderTest` exercises
`LinkReorder::slotInUse` / `slotLen` via
the existing `Link::test_reorderSlot*`
forwards; `LinkArqTest` exercises
`LinkArq::pendingCount` via the existing
`Link::pendingAcks()` forward. No test
source changed.

A new source-level pin
(`test_compile_linkarq_linkreorder_linksweep_present`
in `CompileCheckTest.cpp`) reads the three
new header files and asserts each contains
its `class LinkArq` / `class LinkReorder` /
`class LinkSweep` declaration. Toggling
any class out of `LINK_SRC` in
`test/test_desktop/Makefile` doesn't
remove the headers — but renaming the
class or moving it to a different file
breaks the test at the next `make test`.

**Result:**
- 27 / 27 unit suites (was 27 / 27).
- 3 / 3 itest suites (unchanged).
- 4 / 4 `loopback_modes` (unchanged).
- `verify_build.sh` clean compile against
  esp32:esp32@3.3.5.
- `Link.cpp`: 1577 → 1360 LoC.
- `LinkArq.cpp`: 125 LoC,
  `LinkReorder.cpp`: 105 LoC,
  `LinkSweep.cpp`: 125 LoC.
---

## v5.3.55

**`Ping::setPaused(false)` clears the pending
table proactively, before echo matching resumes.**

The reactive recovery in `matchEcho_` (CRC/length
mismatch → `clearQueue_()`) was the only path that
dropped stale in-flight echoes after a pause →
resume transition. That worked, but bumped
`mismatchCount_` once for every resume and lost
one message each time the queue held a pending
slot.

### Fix

`src/al/pingpong/Ping.h::setPaused` now calls
`clearQueue_()` first thing inside the `if (!p)`
branch, before the existing
`resetStatBaseline()` and `tNextSendMs_ = 0`
cleanup. Any echo from the pre-pause window that
arrives after the resume now hits an empty
pending queue and is discarded as "stale echo"
(already a logged and counted path) instead of
matching against a fresh send and producing a
spurious mismatch.

The `matchEcho_` reactive recovery is kept as a
safety net for the CRC/length mismatch case
that isn't pause-related. The two paths are now
distinct: resume → proactive clear; on-wire
corruption → reactive clear.

### Regression test

`test/test_desktop/al/pingpong/PingResumeSourceTest.cpp`
reads `Ping.h` and asserts the `clearQueue_()`
call is present in `setPaused`'s `if (!p)`
branch, ordered before `resetStatBaseline()`,
and that the comment explicitly references the
proactive path. Toggling the call out of
`setPaused` makes the test fail. Ping is
`#ifdef ARDUINO` so there's no host execution
coverage; the test pins the source-level
contract.

`CompileCheckTest.cpp` also gained `Ping.h` in
its `ARDUINO_GUARDED_FILES` list so a syntax
regression in the changed header is caught by
`run_test_compile_check`.

**Result:**
- 27 / 27 unit suites (was 26 / 26).
- 3 / 3 itest suites (unchanged).
- `verify_build.sh` clean compile against
  esp32:esp32@3.3.5.
---
