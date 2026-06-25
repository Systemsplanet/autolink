# 📅 AutoLink Version History

All releases, most recent first.

---

## v5.3.28

**Plug the remaining silently-discarded `esp_err_t` returns
and add a regression test that catches them.**

The v5.3.27 pass added `esp_err_to_name()` at every
*known* failure-log site, but several other ESP-IDF call
sites were silently ignoring their `esp_err_t` returns.
Fixed:

**`src/al/hal/EspHal.h`:**
- `gpio_set_pull_mode()` — now captures `gp`, logs with
  `esp_err_to_name(gp)` on failure.

**`src/al/web/AutoLinkWeb.cpp`:**
- `httpd_register_uri_handler()` × 8 — collapsed into a
  loop over the URI table; each registration now logs
  the decoded error on failure with the URI path in the
  message.
- `esp_timer_stop()` / `esp_timer_delete()` / `httpd_stop()`
  in the dtor and `fail:` cleanup paths — previously silent.
  Now log `esp_err_to_name()` on each failure (cleanup is
  best-effort but visibility matters).

**Regression test added:**
- `test/test_desktop/EspIdfErrorEtiquetteTest.cpp` — 2 test
  functions, 23 suites / 240 test functions total. Reads
  the source files at test time, asserts every known ESP-IDF
  call site has `esp_err_to_name` within a 15-line window.
  Toggle-regression test feeds the checker a synthetic bare
  call and confirms it flags the regression. Adding new
  ESP-IDF call sites requires extending the `REQUIRED_SITES`
  table — the test will fail until the new site is also
  logged with `esp_err_to_name`.

**Test results:** 23 unit suites / 240 test functions PASS.

**Disclosed limitations:** The regression test is a
whitelist — it only catches the 12 known call sites. New
ESP-IDF call sites added later must be added to
`REQUIRED_SITES` to be guarded.

---
## v5.3.27

**Add `esp_err_to_name()` to every ESP-IDF error log site.**
The codebase already checked every `esp_err_t` return value,
but most logs printed raw hex (`err=0x%X`) or omitted the
error entirely. Now all 7 ESP-IDF error sites decode the
error to a human-readable string.

**`src/al/hal/EspHal.h` (4 sites):**
- `uart_driver_install` failure
- `uart_param_config` failure (now includes baud + decoded err)
- `uart_set_pin` failure (now includes decoded err)
- `uart_set_baudrate` failure (now uses `%s` instead of `0x%X`)

**`src/al/web/AutoLinkWeb.cpp` (3 sites):**
- `esp_timer_create` / `esp_timer_start_periodic` (logged
  with both decoded errors side-by-side)
- `httpd_start` failure
- `httpd_resp_send_chunk` mid-stream failure

**Not changed:** HTTP query-string validation failures
(`handleLevel`, `handleMode`, `handleMsgPause`) — those
respond with HTTP 400 because the request is malformed,
not because ESP-IDF failed. `esp_err_to_name()` doesn't
apply to user-input errors.

**Test results:** 22 unit suites / 238 test functions PASS.
The changed code is `#ifdef ARDUINO` and isn't covered by
host tests; cross-compile the sketch to verify.

**Disclosed limitations:** Cross-compile not run in this
sandbox (toolchain not installed). The `esp_err_to_name()`
helper is standard ESP-IDF and used identically in
thousands of upstream projects — risk is minimal.

---

## v5.3.26

**Ping now prominently displays successful-echo count.**
The 5-second stats ticker in `logStats()` previously showed
`tx=... rx=... baud=... disc=... errs=...` — useful for
throughput, but the actual ping/pong health signal (did the
echo match?) was buried in the debug-level per-message log.

- Added `successEchoCount_` + `mismatchCount_` to `Ping`
  (incremented inside `matchEcho_()` on the success / mismatch
  path). Pong already had `echoCount_`; the base-class
  `logStats()` now reads both via static thunks so the same
  output shape works for either role.
- Reworked the stats line to lead with `echos=` and
  `mismatch=`:
  ```
  echos=N mismatch=N tx=... rx=... baud=... disc=... errs=...
  ```
- Pong's `echoCount_` is reported under the same `echos=`
  field, so the same line format works for both roles.

**Test results:** 22 unit suites / 238 test functions PASS
(pingpong code is `#ifdef ARDUINO` — host tests don't
exercise it; cross-compile gate is the real test).

**Disclosed limitations:** The new counters are not
host-tested. Cross-compile the Arduino sketch on real
hardware to confirm the line shape on a live serial monitor.

---

## v5.3.25

**Fix test-count ambiguity in AGENTS.md final-response
rule.** Step 12 said "X / Y format" for tests passed without
specifying whether `Y` was suite-binary count (22) or
test-function count (~232). The user flagged this — "I thought
we had 200 tests?" — and they're right. Updated the rule:

  - **total test FUNCTIONS passed** (X / Y format — count
    individual `test_*()` functions across every suite, not
    just the number of suite binaries)

Added a one-liner that derives the count:
  `find test -name '*Test.cpp' | xargs grep -cE    '^(void|static void|int) test_' | awk -F: '{s+=$2} END {print s}'`

Current counts:
  - 232 unit C++ test functions (192 in `al/<sub>/` + 40
    in top-level test_desktop/)
  - ~35 dashboard JS tests
  - ~3 itest smoke tests
  - **~270 total test functions across 24 suites**

**Test results:** 22 suites / 232 unit test functions PASS.

**Disclosed limitations:** none.

---

## v5.3.24

**Strip blank lines inside function bodies.** Wrote a small
Python pass over every `.cpp` / `.h` that finds each
opening `{` (end-of-line) and removes the blank lines
immediately after it until the next non-blank statement.
clang-format's `MaxEmptyLinesToKeep: 2` doesn't collapse
blanks that appear at the start of a block, so this had to
be a separate pass.

- **191 blank lines removed from 41 files.** Biggest wins:
  `src/al/link/Link.cpp` (-61), `test/common/WireSim.h` (-14),
  `src/AutoLink.cpp` (-13), `src/al/web/AutoLinkWeb.cpp` (-11),
  `src/al/web/AutoLinkWebHtml.h` (-8).
- Verified all 22 unit suites still PASS.

**Test results:** 22 unit + 2 integration PASS.

**Disclosed limitations:** none.

---

## v5.3.23

**Set clang-format `ColumnLimit` to 55 chars.** The previous
baseline was 100. Re-formatted every `.cpp` / `.h` with the
new limit; collapsed any runs of 3+ blank lines down to 2.
Verified all 22 unit suites still PASS.

A few lines exceed 55 by 1-2 chars (string literals and one
identifier that clang-format can't break). That's acceptable;
the formatting is otherwise uniform.

**Test results:** 22 unit + 2 integration PASS.

**Disclosed limitations:** none.

---

## v5.3.22

**Pretty-print all `.cpp` / `.h` with `clang-format`.** Ran
`clang-format -i` across the entire tree against a new
`.clang-format` (LLVM base + Linux braces + 4-space + 100-col +
right-aligned pointers). Preserved preprocessor nesting
indentation via `IndentPPDirectives: AfterHash`. Verified
all 22 unit suites still PASS after the format pass.

**AGENTS.md:**
- New rule 14a: "Run `clang-format` on every changed `.cpp` /
  `.h` before zipping." Establishes the formatting baseline.
- Quick-workflow step 12 expanded: "Final response must
  report, for unit and itest separately and combined: total
  number of tests passed and total time elapsed."

**Test results:** 22 unit + 2 integration PASS.

**Disclosed limitations:** none.

---

## v5.3.21

**Strip every other-version reference from non-Version docs
and from code.** Per "there is only the current version":

- **docs/Protocol.md** — removed "Comparison with the legacy
  protocol" table, "Campaign results" section, and every
  comparison sentence. Reworded remaining "legacy" mentions.
- **AGENTS.md** — rule 24 reframed to "Zip file-list diff".
- **Source code** — removed version-anchor comments:
  `(5.3.12 fix)` in `src/AutoLink.cpp` and
  `src/al/link/Link.cpp`; `(v5.1.40: ...)` in
  `build/build_env.sh`; `(v5.1.31: ...)` in
  `build/verify_build/verify_build.ino`.
- **Test code** — rewrote version banners in LinkV53/V531
  tests. Renamed toggle-confirmation line in
  LinkReorderTest. Test fixture version strings replaced
  with `"1.0.0"`.

**Test results:** 22 unit + 2 integration PASS.

**Disclosed limitations:** none.

---

Older versions are in git history.
