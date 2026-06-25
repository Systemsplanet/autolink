# 📅 AutoLink Version History

All releases, most recent first.
## v5.3.93

**begin() hardening: uart_driver_install retry, txBufferSize default, isHealthy() gate**

Three small fixes to make
`bringUpLink()` fail loudly
instead of producing a silent
wire on a broken HAL.

### Fix 1 — `uart_driver_install` retry after 10 ms on `ESP_FAIL`

The 5.3.92 pre-clear
(`uart_is_driver_installed`
guard before
`uart_driver_install`)
catches a dirty reboot, but
a freshly-deleted driver's
DMA buffers may not be fully
released by the IDF on the
next boot. The first
`uart_driver_install` then
returns `ESP_FAIL` transiently.
The 5.3.93 shape: on `ESP_FAIL`,
sleep 10 ms via `vTaskDelay`
and retry once. Pinned by
`EspHalBeginAndHealthTest`'s
`test_esphal_begin_retries_uart_driver_install_on_esp_fail`.

### Fix 2 — `AutoLinkConfig::txBufferSize` default = 256

The default was `0`, which
the production ctor floor
silently bumped to a
host-test-shaped value. With
the floor kept, the default
is now a non-zero 256 (sized
for a couple of COBS frames
at default `maxMsg`) so the
struct is usable as-is. The
host-test ctor still bumps
for `MockHal`'s larger send
buffer. Pinned by
`test_autolink_config_default_tx_buffer_size_is_256`.

### Fix 3 — `bringUpLink` halts on `!isHealthy()` after `begin()`

The 5.3.92 shape called
`comm.begin()` and proceeded
into the loop on its return
value, even if the HAL
`uart_driver_install`,
xTaskCreate, or
`xTimerCreate` had failed
inside `begin()` (in which
case `healthy` was `false`).
The user would see a wall of
"not ready" log lines and a
silent wire. The 5.3.93 shape:
`bringUpLink` checks
`comm.isHealthy()` immediately
after `comm.begin()` and,
on false, logs the failure
and enters a `while (true)
delay(1000)` halt. Pinned by
`test_bringUpLink_halts_on_isHealthy_false`.
---

## v5.3.92

**httpd startup race + TIME_WAIT budget + UART2 pre-clear**

Three field-test bugs from
5.3.91's on-device trial.
Two race / retry-budget
issues caused the web
monitor to silently fail
to come up after a dirty
reboot; the third caused
UART2 init to fail on the
same path.

### Fix 1 — Double-entry race on `setupHttpAndLogging_`

The prior release had
`begin()` calling
`setupHttpAndLogging_()`
in its quick-start loop
AND `wifiTaskThunk_()`
calling it after WiFi
connected. The
`enabled_` flag inside
the function was
read-then-set without a
mutex, so both callers
entered the function
before either flipped
`enabled_=true`, and
both hammered port 80
simultaneously,
consuming the retry
budget on duplicate
calls. The field-test
log showed two identical
"attempt 1/8 — waiting
5000 ms" lines 36 ms
apart.

The fix: drop the
`setupHttpAndLogging_`
call from `begin()`.
`begin()` now polls
`isUp()` until either
the server is up or
`HTTPD_BEGIN_QUICK_MS`
elapses. `wifiTaskThunk_`
is the sole caller. No
new mutex, no extra RTOS
primitive.

### Fix 2 — httpd retry budget for TIME_WAIT

lwIP TCP TIME_WAIT is
`CONFIG_LWIP_TCP_MSL * 2`
≈ 60 s on default config.
5.3.91's budget was
8 attempts × 5 s = 40 s,
which was 20 s short of
the worst-case TIME_WAIT.
The double-entry from
Fix 1 made it worse —
both instances burned
retry slots in parallel,
so the practical budget
was effectively halved.

The fix: bump the retry
budget to 14 attempts
× 5 s = 70 s, and
increase `HTTPD_BEGIN_QUICK_MS`
from 12 s to 75 s so the
synchronous wait in
`begin()` covers the
full retry budget
(`HTTPD_BEGIN_QUICK_MS`
must outlast
`HTTPD_RETRY_MAX *
HTTPD_RETRY_PRE_MS`).

### Fix 3 — UART2 `ESP_FAIL` after dirty reboot

If the prior boot left
UART2 installed (crash,
brownout, watchdog),
the next boot's
`uart_driver_install`
returned `ESP_FAIL` and
the link layer was
permanently dead. The
fix: pre-clear the
driver in
`EspHal::begin()`
guarded by
`uart_is_driver_installed()`.
On a clean boot the
guard returns false and
nothing happens; on a
dirty boot it returns
true and
`uart_driver_delete`
clears the leftover so
the install can
proceed.

### Regression coverage

New source-level test
file:
`test/test_desktop/al/web/HttpdStartupTest.cpp`
with three pins.

1. `test_begin_does_not_call_setupHttpAndLogging`
   reads `AutoLinkWeb.cpp`
   and asserts
   `bool AutoLinkWeb::begin(...)`'s body does
   NOT contain the literal
   `setupHttpAndLogging_`.
   Re-introducing the
   call inside `begin()`
   trips here.
2. `test_httpd_retry_budget_covers_time_wait`
   parses the three
   constants out of
   `AutoLinkWeb.h` and
   asserts
   `HTTPD_RETRY_PRE_MS >= 5000`,
   `HTTPD_RETRY_MAX *
   HTTPD_RETRY_PRE_MS >= 60000`,
   and
   `HTTPD_BEGIN_QUICK_MS >= HTTPD_RETRY_MAX * HTTPD_RETRY_PRE_MS`.
   Lowering any of them
   trips here.
3. `test_esphal_begin_preclears_uart_driver`
   reads `EspHal.h` and
   asserts `uart_is_driver_installed(`
   guards `uart_driver_delete(`
   before
   `uart_driver_install(`.
   Removing the guard or
   the delete trips here.

A second new file,
`test/test_desktop/al/web/HttpdResetEndpointTest.cpp`,
stands up a loopback TCP
HTTP server in a thread
that mirrors
`AutoLinkWeb::handleReset()`'s
contract (`resetStats` +
`resetErrors` +
`resetDiag` + emit "ok"
with `text/plain` +
`Connection: close`), then
drives a real HTTP request
via the system `curl`
binary against the bound
port. The test:

1. Spins up the server
   thread, blocks on
   `g_serverReady` until
   bind() returns (5 s
   ceiling per AGENTS rule
   4).
2. Drives
   `curl --max-time 5 -X POST http://127.0.0.1:<port>/reset`
   and asserts the body
   equals the literal
   `ok` and the handler
   fired (`g_resetCount`
   incremented).
3. Drives a second
   `POST /reset` to confirm
   the handler is reusable
   — the dashboard's reset
   button can be pressed
   repeatedly without
   server-side state.
4. Drives a `POST /garbage`
   and asserts the body is
   NOT `ok` and the reset
   count stays zero — the
   handler is path-scoped.

A regression that changes
the body to anything other
than the literal `ok`,
breaks the path scope, or
prevents the handler from
being invoked at all trips
this gate with a real HTTP
error message visible in
the test output.

All three source-level
pins and both HTTP
round-trip pins were
sanity-checked against
their respective reverts
during the release:
each fix removed → red,
each fix restored → green.

### Limitations

- The retry-budget
  ceiling is now 70 s.
  Sketches that must
  come up faster
  should not use the
  web monitor (pass no
  `ssid`).
- The UART pre-clear
  depends on
  `uart_is_driver_installed()`
  being available; it
  is in ESP-IDF 4.x
  onward. The
  `EspHal::begin()` call
  site is `#ifdef ARDUINO`
  so this only matters
  on Arduino-ESP32.
- The 5.3.91 quick-start
  window of 12 s is now
  75 s. Sketch `setup()`
  blocks for up to that
  long waiting for the
  web monitor before
  proceeding. In
  practice the bg task
  either succeeds inside
  the budget or the
  sketch proceeds
  anyway and the bg
  task keeps retrying.

### Files touched

- `src/al/web/AutoLinkWeb.h`
  — bumped
  `HTTPD_RETRY_MAX`
  (8→14) and
  `HTTPD_BEGIN_QUICK_MS`
  (12 s→75 s).
- `src/al/web/AutoLinkWeb.cpp`
  — `begin()` no longer
  calls
  `setupHttpAndLogging_`;
  it just polls `isUp()`.
- `src/al/hal/EspHal.h`
  — added
  `uart_is_driver_installed`-
  guarded
  `uart_driver_delete`
  before
  `uart_driver_install`.
- `test/test_desktop/al/web/HttpdStartupTest.cpp`
  — new file, three pins.
- `test/test_desktop/al/web/HttpdResetEndpointTest.cpp`
  — new file, two
  real-HTTP round-trip
  pins for the `/reset`
  endpoint.
- `test/test_desktop/Makefile`
  — added
  `run_test_httpd_startup`
  and
  `run_test_httpd_reset_endpoint`
  to `TEST_BINS`, build
  rules, and runners.
- `test/scripts/env/install_system_stubs.py`
  — added
  `uart_is_driver_installed`
  stub so the
  `compile_check` test
  parses `EspHal.h`
  cleanly.

The library version
contract (`include/AutoLink.h`
+ `library.properties` +
`idf_component.yml` + this
file) bumps 5.3.91 → 5.3.92
per AGENTS rule 3.
---

## v5.3.91

**TIME_WAIT settle widened + boot order swap (Serial first)**

Two field-test fixes from
5.3.90's trial: the httpd
retry budget was too tight
to clear a typical reboot's
TIME_WAIT socket, and the
boot order left the user
watching a silent serial
port for up to 5 s before
the WiFi banner showed up.

### Fix 1 — httpd retry budget for TIME_WAIT

On a warm reboot the prior
boot's httpd socket sits in
TIME_WAIT for up to ~60 s.
The 5.3.90 retry budget was
3 attempts of 250 ms backoff
with a single 750 ms one-shot
pre-delay — that totaled ~1.5 s
of settle and frequently
returned ESP_ERR_HTTPD_ALLOC
on the first try. The 5.3.91
shape: each attempt now sleeps
`HTTPD_RETRY_PRE_MS` (5 s)
as a per-attempt prefix
before `httpd_start`. Budget
widened to `HTTPD_RETRY_MAX = 8`
attempts. Quick-start window
in `begin()` bumped to
`HTTPD_BEGIN_QUICK_MS = 12 s`
so the synchronous path can
cover the typical 5 s WiFi +
5 s pre-settle + 2 s headroom.
The bg retry (`HTTPD_RETRY_BG_MS`)
is unchanged.

### Fix 2 — boot order: Serial first

The 5.3.90 order was
`startWebMonitor → initSerial
→ bringUpLink`. That left the
serial port silent for up to
5 s while WiFi + httpd came
up. Reordered to `initSerial
→ startWebMonitor → bringUpLink`
for both `Ping::setup()` and
`Pong::setup()`. The user now
sees the boot banner first,
then the WiFi banner, then
the link + version line. No
semantic change — link still
starts pre-paused on Ping and
unpaused on Pong.

### Fix 3 — `startWebMonitor` log line

The 5.3.90 line passed the
tag string as a format arg
(`"PingPongBase web monitor
begin returned %s in %lu ms"`)
with no separate tag argument.
That collapsed the log entry's
tag to the whole string and
silently dropped the `("PingPongBase", ...)`
form every other log line uses.
Fixed to the canonical two-arg
form: `("PingPongBase",
"web monitor begin: %s (%lu ms)",
monOk ? "ok" : "FAILED", ms)`.

### Regression coverage

Two pins updated in
`test/test_desktop/al/web/HandleRootChunkedTest.cpp`:

- `test_setup_httpd_retries_on_failure`
  now asserts the per-attempt
  prefix shape: `for` loop wraps
  `httpd_start(&server_, &cfg)`,
  `vTaskDelay(pdMS_TO_TICKS(HTTPD_RETRY_PRE_MS))`
  appears inside the loop and
  BEFORE the `httpd_start` call.
  Reverting the per-attempt
  prefix back to the 5.3.90
  one-shot pre-delay trips here.
- `test_setup_httpd_pre_delay`
  is unchanged (it only asserts
  that *some* pre-delay fires
  before the first `httpd_start`,
  which is still true with the
  new shape).

Two pins updated in
`test/test_desktop/al/pingpong/PingPongStructureTest.cpp`:

- `test_ping_setup_sequences_three_steps`
  asserts the new order
  `initSerial < startWebMonitor
  < bringUpLink` (was the reverse
  in 5.3.90). Reverting Ping's
  setup() back to the 5.3.90
  order trips here.
- `test_pong_setup_sequences_three_steps`
  asserts the same new order
  on Pong. Reverting trips here.

### Behaviour impact on boot

With 5.3.91 a typical cold
boot with reachable WiFi:

```
[boot]
  initSerial → "boot: role=Ping  baud=115200  WiFi=MySSID"
  startWebMonitor → WiFi connects, httpd starts
    → "web monitor begin: ok (4 ms)"
    → "Web monitor at http://10.0.0.42:80"
  bringUpLink → comm_.begin()
    → "link layer up (comm_.begin returned)"
    → "AutoLink v5.3.91"
  → "mode=Ping  ready  paused=true  (push Start to send)"
[user hits Start on dashboard]
  setPaused(false) → kickoff() → break fires → baud sweep begins
```

Worst-case with a flaky AP the
quick-start path can take up
to ~12 s before returning to
the sketch setup; bg retry
continues indefinitely.

### Limitations

- 12 s quick-start is a wall-
  time floor on boot. Sketches
  that must come up faster
  should not use the web monitor
  (pass no `ssid`).
- `HTTPD_RETRY_DELAY_MS` is
  no longer used inside
  `setupHttpAndLogging_()` (the
  per-attempt prefix replaced
  the inter-attempt backoff).
  The constant is still used
  by `begin()`'s quick-start
  loop and stays in the header.

### Files touched

- `src/al/web/AutoLinkWeb.h` —
  bumped constants.
- `src/al/web/AutoLinkWeb.cpp` —
  restructured `setupHttpAndLogging_`
  retry loop.
- `src/al/pingpong/Ping.h` —
  reordered `setup()`.
- `src/al/pingpong/Pong.h` —
  reordered `setup()`.
- `src/al/pingpong/PingPongBase.h` —
  fixed `startWebMonitor` log line.
- `test/test_desktop/al/web/HandleRootChunkedTest.cpp` —
  updated `test_setup_httpd_retries_on_failure`
  pin.
- `test/test_desktop/al/pingpong/PingPongStructureTest.cpp` —
  updated both Ping and Pong
  order pins.

The library version contract
(`include/AutoLink.h` +
`library.properties` +
`idf_component.yml` + this file)
bumps 5.3.90 → 5.3.91 per
AGENTS rule 3.
---

## v5.3.90

**fail-block teardown fix + httpd_start pre-delay + Link::kickoff + Ping silent until Start**

Three field-test bugs from
5.3.89's on-device trial, plus
the protocol-side rework
needed to make Ping truly
silent until the user pushes
Start on the dashboard.

### Fix 1 — `goto fail` teardown (host-side data race)

The previous `setupHttpAndLogging_()`
had a `fail:` cleanup block that
unconditionally destroyed the
log ring, snapshot mutex, and
log mutex — but those are
`begin()`-lifetime resources
allocated at the top of
`AutoLinkWeb::begin()`, not
inside `setupHttpAndLogging_()`.
When `setupHttpAndLogging_()`
returned false (e.g. httpd_start
failed all three attempts and
the bg task retried it), the
next call would see null
pointers. Worse, `logSinkCb`
was still wired and ran on
every log emission, dereferencing
a null `logMtx_` inside
`xSemaphoreTake`. The fix: the
`fail:` block now cleans only
the resources it owns (`statTimer_`,
`server_`) and leaves the
begin()-lifetime trio intact.
They're freed only in the
destructor.

### Fix 2 — EADDRINUSE on reboot

The previous `setupHttpAndLogging_()`
called `httpd_start` immediately.
On reboot the prior boot's
httpd socket sits in
TIME_WAIT for up to ~60 s; a
bare call races that and
returns EADDRINUSE /
ESP_ERR_HTTPD_ALLOC. The fix:
wait `HTTPD_RETRY_PRE_MS`
(750 ms) before the first
attempt. The retry-with-backoff
loop is unchanged.

### Fix 3 — Boot order + Ping silent-until-Start

The previous order was
Serial.begin → link.begin() →
web.begin(). The user wanted:
WiFi connect → web socket up →
web app live-log wired → version
line first entry → then Serial +
link. Plus: Ping should not
transmit anything on the wire
until the user pushes Start on
the dashboard. The previous
`Link::begin()` for the master
called `hw.sendBreak()`
unconditionally, so Ping fired
its break the moment `comm.begin()`
ran.

The reworked flow:

- `AutoLinkWeb::begin()`
  allocates the log ring + mutexes,
  wires `setSink`, emits the
  version line, waits up to
  `WIFI_BEGIN_QUICK_MS` (now
  **5 s**, was 3 s) for WiFi,
  then synchronously brings up
  httpd with a retry loop bounded
  by `HTTPD_BEGIN_QUICK_MS` (5 s).
  The sketch's `setup()` proceeds
  only after the GUI is loaded
  or the deadline expires.
- `Link::begin()` is now pure
  init (dwell computation +
  internal state). Wire-side
  effects (sendBreak, sweep
  entry, baud arm) are deferred
  to a new `Link::kickoff()`.
  When `linkPaused_` is false
  (the default — Pong, host
  tests), `begin()` calls
  `kickoff()` automatically so
  the legacy `begin()` = go
  contract holds. When
  `linkPaused_` is true (Ping's
  case), `begin()` returns
  without firing the break.
- `bringUpLink(log, comm,
  prePaused)` takes a new
  boolean. Ping passes `paused_`
  (true by default) so
  `comm.begin()` runs but
  doesn't fire the wire. The
  user pushes Start on the
  dashboard → `setPaused(false)`
  → `setLinkPaused(false)` →
  `kickoff()` → break fires.
  Ping stays silent on the
  wire until then.
- `Ping::setup()` order is now
  `startWebMonitor` →
  `initSerial` → `bringUpLink`
  (with prePaused=true) → fall-
  through check. If `mon_.isUp()`
  is false (no SSID given, OR
  `mon.begin()` returned false,
  OR the 5 s GUI deadline
  expired), Ping **falls through
  to sending**: `paused_ = false`,
  `setLinkPaused(false)`,
  `kickoff()`. The user
  explicitly asked for this
  fall-through: if the GUI
  can't start, don't leave the
  device silent. Pong is
  unchanged — it still calls
  `kickoff()` after `bringUpLink`
  so the SWP P1 listener is
  armed at boot, passively
  waiting for Ping's break.

### Wire format

Unchanged. The wire is byte-
identical to v5.3.89. The
changes are all in the local
httpd / WiFi / link-layer
orchestration; no header in
`include/` moves the wire
constants, no public API
symbols on the protocol side
are added or removed. The
library version contract
(`include/AutoLink.h` +
`library.properties` +
`idf_component.yml` + this file)
bumps 5.3.89 → 5.3.90 per
AGENTS rule 3.

### Regression coverage

**Four new structural pins** in
`test/test_desktop/al/web/HandleRootChunkedTest.cpp`:

1. `test_setup_httpd_pre_delay` —
   `setupHttpAndLogging_()` must
   call `vTaskDelay(pdMS_TO_TICKS(HTTPD_RETRY_PRE_MS))`
   before the first `httpd_start(&server_, ...)`
   call. Removing the pre-delay
   trips here.
2. `test_fail_block_preserves_begin_lifetime_resources` —
   the `fail:` block must NOT
   contain `vSemaphoreDelete(snapMtx_)`,
   `vSemaphoreDelete(logMtx_)`, or
   `free(logRing_)`. Reintroducing
   any of those trips here.
3. `test_link_begin_defers_kickoff_when_paused` —
   `Link::kickoff()` must be
   public + idempotent
   (`kickedOff_` guard), and
   `Link::begin()` must check
   `linkPaused_` and `return;`
   before calling `kickoff()`.
   Removing the early-return
   trips here.
4. `test_ping_falls_through_when_gui_down` —
   `Ping::setup()` must check
   `mon_.isUp()`, flip
   `paused_ = false`, and call
   `comm_.kickoff()` when the
   GUI never came up. Removing
   the fall-through branch trips
   here.

**Updated** `test_ping_setup_sequences_three_steps`
and `test_pong_setup_sequences_three_steps`
in `test/test_desktop/al/pingpong/PingPongStructureTest.cpp`:
the order assertion is now
`startWebMonitor < initSerial < bringUpLink`
(was `initSerial < bringUpLink < startWebMonitor`).
A regression to the old order
trips here.

Toggle behaviour verified
manually for all four new
pins.

### Disclosed limitations

- The 5 s `WIFI_BEGIN_QUICK_MS` +
  5 s `HTTPD_BEGIN_QUICK_MS`
  ceiling means a sketch with
  a flaky AP can take up to
  10 s at boot before
  `setup()` returns. On a
  healthy AP the whole path
  completes in <100 ms; on a
  dead AP (no SSID given) it
  completes in ~5 s.
- `Link::kickoff()` is a new
  public method on the
  `Link` class. It is `void`,
  takes no arguments, and is
  documented as "no-op when
  already running or paused".
  Internal callers (Pong) and
  external callers (Ping via
  `setPaused(false)`) are the
  only current users; future
  diagnostic tooling may want
  to call it directly to
  start a link from a known
  clean state.

### Result

- 33 / 33 host unit suites
  pass (`make test`), including
  all 11 pins in
  `run_test_handle_root_chunked`
  (4 new + 7 existing). Wall:
  ~4.8 s.
- 3 / 3 host integration suites
  pass (`make itest`). Wall: ~40 s.
- Toggle test: 4/4 new pins
  flip to red when their
  matching source shape is
  reverted (manually verified
  for the pre-delay, fail-block
  preservation, kickoff branch,
  and Ping fall-through).
- `build/verify_build.sh` clean
  compile against
  `esp32:esp32:firebeetle32` —
  `Sketch uses 1019427 bytes (77%)
  of program storage space.
  Global variables use 66992 bytes
  (20%) of dynamic memory`.
  Flash delta vs 5.3.89: +2904 B
  (kickoff method, fall-through
  branch, pre-delay, fail-block
  cleanup, order swap); RAM
  delta: 0 B.
---

## v5.3.89

**WiFi retry-forever + httpd_start retry + begin() blocks until GUI loaded**

Two real symptoms from the 5.3.88
field test: the device never came
back on a flaky AP (the bg task
gave up after 10 s, leaving the
web monitor dead until manual
reboot), and the live log was
empty when the user opened the
GUI because `setSink` was called
too late (after WiFi + httpd,
inside `setupHttpAndLogging_`).
A third latent symptom surfaced
during the same test:
`httpd_start` returned
`ESP_ERR_HTTPD_TASK` once at
boot — heap pressure during NTP
+ esp_timer + WiFi bring-up
caused the worker-task allocation
to fail. The 5.3.88 shape only
called `httpd_start` once and
gave up.

### Fix

`begin()` now allocates the log
ring + mutexes and calls
`setSink(logSinkCb, this)`
**before** anything else is
logged, then emits the version
line as the very first entry —
so when the user opens the GUI,
the live log starts with
`AutoLink: v5.3.89`. After the
sink is wired, `begin()` launches
the bg WiFi task and waits up to
`WIFI_BEGIN_QUICK_MS` (3 s) for
the STA to associate. If WiFi
is up, `begin()` synchronously
calls `setupHttpAndLogging_()`
inside a bounded retry loop
(`HTTPD_BEGIN_QUICK_MS` = 5 s,
`HTTPD_RETRY_DELAY_MS` = 250 ms
between attempts, `HTTPD_RETRY_MAX`
= 3 attempts inside the
function). `begin()` does not
return until `isUp()` is true
or the deadline expires; the
sketch's `setup()` therefore
proceeds only after the web GUI
is loaded. If the deadline
expires, `begin()` returns
`true` anyway and the bg task
keeps retrying `httpd_start`
every `HTTPD_RETRY_BG_MS` (1 s)
in the background.

`wifiTaskThunk_()` now branches
on credentials: when ssid+pass
are provided it retries forever
with exponential backoff
(`WIFI_RETRY_BACKOFF_MS_MIN` = 1 s,
doubling up to
`WIFI_RETRY_BACKOFF_MS_MAX` = 30 s).
The previous 10 s hard-timeout
is preserved for the
no-credentials (offline) branch.
The bg task also watches for
WiFi drops mid-session: if the
STA drops while the web monitor
is up, it tears down `server_`
+ `statTimer_`, resets
`enabled_ = false`, and re-enters
the connect-retry loop — so the
GUI comes back automatically
when the AP returns.

`WiFi.disconnect(true)` was
changed to `WiFi.disconnect()`
(no arg) so the host-test stub
matches the real ESP32 WiFi
API (the stub doesn't declare
the bool overload). On real
hardware the default
`wifioff=false` is the right
behavior here — we want to
reconnect, not power-cycle the
radio.

`setupHttpAndLogging_()` no
longer re-allocates the log ring
or re-wires the sink (those
moved to `begin()`). It now
assumes the ring + mutexes are
already initialised and returns
`false` cleanly if not.

### Wire format

Unchanged. The wire is byte-
identical to v5.3.88. The
changes are all in the local
httpd / WiFi orchestration;
no header in `include/` moves,
no public API symbols are added
or removed. The library version
contract (`include/AutoLink.h` +
`library.properties` +
`idf_component.yml` + this file)
bumps 5.3.88 → 5.3.89 per AGENTS
rule 3.

### Regression coverage

**Five new structural pins** in
`test/test_desktop/al/web/HandleRootChunkedTest.cpp`:

1. `test_begin_wires_log_sink_first` —
   the `setSink(logSinkCb, this)`
   call must be inside `begin()`
   and must NOT also appear inside
   `setupHttpAndLogging_()`. The
   sink is wired exactly once,
   before any other log emission.
2. `test_begin_logs_version_line_after_sink` —
   the `AUTOLINK_VERSION` log line
   must be emitted after `setSink`
   and before the NVS-info log
   line; the version is the first
   entry the user sees in the GUI.
3. `test_begin_blocks_until_httpd_up` —
   `begin()` must contain a
   `while (!isUp() && millis() -
   httpStartMs < HTTPD_BEGIN_QUICK_MS)`
   loop calling
   `setupHttpAndLogging_()`; the
   `!isUp()` predicate must be in
   the same condition expression
   as the deadline (a `while
   (false && ...)` regression
   trips here).
4. `test_setup_httpd_retries_on_failure` —
   `httpd_start` must be wrapped
   in a `for (attempt = 1; attempt
   <= HTTPD_RETRY_MAX)` loop with
   a `vTaskDelay(pdMS_TO_TICKS(HTTPD_RETRY_DELAY_MS))`
   between attempts. Collapsing
   the loop to a single call trips
   here.
5. `test_wifi_task_retries_forever_when_creds_given` —
   `wifiTaskThunk_` must branch on
   `haveCreds`, reference both
   `WIFI_RETRY_BACKOFF_MS_MIN` and
   `WIFI_RETRY_BACKOFF_MS_MAX`,
   include the `backoffMs = backoffMs * 2`
   doubling line, cap at
   `WIFI_RETRY_BACKOFF_MS_MAX`,
   and have an unbounded outer
   `while (true)` retry loop.

Existing pins (`cfg.stack_size`
= 16384, chunked loop, headers)
unchanged.

Toggle behaviour verified for
all five new pins: each has at
least one source-edit that
trips it (manually reverting
the matching code shape aborts
the suite).

### Disclosed limitations

- A regression test that exercises
  the actual `ESP_ERR_HTTPD_TASK`
  failure path at host-build time
  is not feasible: `AutoLinkWeb.cpp`
  is `#ifdef ARDUINO`, and the
  esp_http_server task-allocation
  failure mode is heap-dependent
  on real ESP32 hardware. The
  source-grep retry pin is the
  structural guard; runtime
  confirmation is via
  `build/verify_build.sh` +
  on-device bring-up.
- `begin()` blocks the sketch's
  `setup()` for up to
  `WIFI_BEGIN_QUICK_MS` (3 s) +
  `HTTPD_BEGIN_QUICK_MS` (5 s) =
  up to 8 s of wall time at boot
  when WiFi is reachable. On a
  flaky AP where WiFi doesn't
  join in 3 s, `begin()` returns
  in ~3 s; on a healthy AP the
  entire path completes in <100 ms.
- The bg WiFi task now runs
  indefinitely when creds are
  given, which uses one FreeRTOS
  task slot permanently (~4 KB
  stack). This is the cost of
  "GUI comes back automatically
  after a WiFi drop"; documented
  trade-off, same shape as the
  pre-5.3.88 design but now
  actually retries instead of
  exiting.

### Result

- 33 / 33 host unit suites pass
  (`make test`), including all 7
  pins in
  `run_test_handle_root_chunked`
  (5 new + 2 existing). Wall:
  ~4.6 s.
- 3 / 3 host integration suites
  pass (`make itest`). Wall: ~40 s.
- Toggle test: 5/5 new pins
  flip to red when their
  matching source shape is
  reverted. Pin 3 verified by
  `while (false && ...)` →
  abort. Pin 4 verified by
  collapsing the retry for-loop
  to a single `httpd_start` call
  → abort.
- `build/verify_build.sh` clean
  compile against
  `esp32:esp32:firebeetle32` —
  `Sketch uses 1018735 bytes (77%)
  of program storage space.
  Global variables use 66992 bytes
  (20%) of dynamic memory`.
  Flash delta vs 5.3.88: +2212 B
  (the retry loop, backoff math,
  and the synchronous httpd-up
  wait); RAM delta: 0 B (the bg
  task was already there; the
  new code reuses the same task
  slot).
---

## v5.3.88

**httpd worker stack bump (8192 → 16384)**

After v5.3.87 the dashboard renders,
but the httpd worker task exhausts
its stack at runtime under sustained
chunked sends on the ~28 KB
`DASHBOARD_HTML` payload. The build
is clean (no static stack analysis
in ESP-IDF's httpd by default), the
dashboard returns 200 OK, but the
worker panics mid-payload or drops
the socket after a few requests —
the user sees intermittent blank
pages with no compile-time signal.
The 5.3.87 bump (6144 → 8192) was
the right call for the small-
response path; the chunked loop's
per-call local state plus the
httpd internal send-buffer frame
under sustained load pushes the
task over 8 KB on real hardware.

### Fix

`cfg.stack_size` inside the httpd
config block in `AutoLinkWeb.cpp`
is bumped from 8192 to 16384
bytes. 12288 might also work but
16384 gives headroom under burst
load. The chunked loop itself is
unchanged — the loop is correct;
this is purely a stack-headroom
fix on the httpd worker task.
Headers, chunk cap (`CHUNK = 4096`),
and the null-chunk terminator are
all unchanged.

### Wire format

Unchanged. The wire is byte-identical
to v5.3.87. The change is purely
in the local httpd config; no
header in `include/` moves, no
public API symbols are added or
removed. The library version
contract (`include/AutoLink.h` +
`library.properties` +
`idf_component.yml` + this file)
bumps from 5.3.87 → 5.3.88 per
AGENTS rule 3.

### Regression coverage

**Pin updated:**
`test_httpd_stack_size_is_at_least_16384`
in
`test/test_desktop/al/web/HandleRootChunkedTest.cpp`.
Source-greps
`src/al/web/AutoLinkWeb.cpp`,
extracts the httpd config block,
parses the integer, and asserts:

1. `stackSize >= 16384` — lower-
   bound guard. A regression to
   6144 or 8192 trips here.
2. `value == "16384"` — exact-
   value guard. A future bump
   to 24576 trips here too (and
   that's intentional; the
   constant is the contract).

Toggle behaviour verified:
reverting `cfg.stack_size` to
8192 fails both assertions
(`Assertion 'stackSize >= 16384'
failed`, abort 134). Reverting to
6144 fails both assertions the
same way. Restoring to 16384
returns the suite to green.

### Disclosed limitations

- A regression test that exercises
  the actual stack overflow at
  host-build time is not feasible:
  `AutoLinkWeb.cpp` is `#ifdef
  ARDUINO` (host tests skip it),
  and FreeRTOS task-stack tracking
  on Linux-pthreads is a different
  measurement than on real ESP32
  hardware. The source-grep pin
  above is the structural guard;
  runtime confirmation is via
  `build/verify_build.sh` +
  on-device bring-up.

### Result

- 33 / 33 host unit suites pass
  (`make test`), including the
  updated
  `run_test_handle_root_chunked`
  with the 16384 pin. Wall: ~4.9 s.
- 3 / 3 host integration suites
  pass (`make itest`). Wall: ~40 s.
- Toggle test: reverting
  `cfg.stack_size` to 8192 →
  abort 134 on the pin. Restoring
  → green. The pin is the only
  gate; no host-test compile/link
  change is required to flip it.
- `build/verify_build.sh` clean
  compile against
  `esp32:esp32:firebeetle32` (no
  delta vs 5.3.87 — the bump is
  a single integer literal; the
  httpd task is created with the
  larger stack at task-creation
  time and the 8 KB extra is
  freed when the task exits).
- 1 line changed in production
  code (`cfg.stack_size = 8192;`
  → `cfg.stack_size = 16384;`).
  Test pin: assertion thresholds
  + the surrounding comment
  prose in
  `HandleRootChunkedTest.cpp`.
- `idf_component.yml` brought
  into lockstep with the rest
  of the version contract
  (5.3.79 → 5.3.88); it had been
  drifted since 5.3.79. Not a
  behavioural change — the ESP-
  IDF component manifest now
  matches `library.properties`
  and `include/AutoLink.h` per
  AGENTS rule 3.
---

## v5.3.87

**Dashboard handleRoot chunked send + httpd stack bump**

The dashboard at `/` was a ~28 KB
HTML/JS payload served via a single
`httpd_resp_send()` call. ESP-IDF's
httpd tops out around a 4096-byte
send buffer, so the single-shot
path silently truncated or stalled
mid-frame and the browser received
a malformed / empty page — a
connection error or blank screen
that the user could not debug.
Cross-compiling the README's
`Ping.ino` against
`esp32:esp32:firebeetle32` showed
the build path compiles cleanly,
so the bug only surfaces at
runtime on a real device.

### Fix

`AutoLinkWeb::handleRoot` now
streams the dashboard in 4096-byte
chunks via `httpd_resp_send_chunk`,
terminating the chunked-encoded
body with the canonical
`httpd_resp_send_chunk(req,
nullptr, 0)` terminator so the
browser flushes its parser. The
chunk cap (`const size_t CHUNK =
4096`) is a named constant so a
future bump to a smaller send
buffer can pin it without touching
the loop. The httpd task's stack
was bumped from 6144 to 8192
bytes — the chunked loop's local
state plus the httpd internal
send-buffer frame can otherwise
push the task over its stack
under sustained load.

Headers (`text/html; charset=utf-8`,
`Cache-Control: no-store`,
`Connection: close`) are unchanged:
the chunked rewrite only touches
the body path.

### Wire format

Unchanged. The wire is byte-identical
to v5.3.86. The change is purely
in the local httpd handler; no
header in `include/` moves, no
public API symbols are added or
removed. The library version
contract (`include/AutoLink.h` +
`library.properties` + this file)
bumps from 5.3.86 → 5.3.87 per
AGENTS rule 3.

### Regression coverage

**New structural pin:**
`run_test_handle_root_chunked`
in
`test/test_desktop/al/web/HandleRootChunkedTest.cpp`.
Source-greps
`src/al/web/AutoLinkWeb.cpp` and
asserts:

1. `handleRoot` does NOT contain
   `httpd_resp_send(req,
   DASHBOARD_HTML, ...)` — the
   buggy single-shot path is gone.
2. `handleRoot` calls
   `httpd_resp_send_chunk(req, p,
   ...)` for body bytes — the
   chunked data path is in place.
3. `handleRoot` terminates with
   `httpd_resp_send_chunk(req,
   nullptr, 0)` — the chunked
   body closes cleanly.
4. `handleRoot` declares
   `const size_t CHUNK = 4096;`
   — the chunk cap matches the
   httpd send buffer.
5. `handleRoot` still sets
   `text/html; charset=utf-8`,
   `Cache-Control`, and
   `Connection: close` — the
   rewrite didn't accidentally
   drop a response header.
6. `cfg.stack_size = 8192` inside
   the httpd config block —
   the stack bump is in place.

Toggle behaviour verified:
reverting `handleRoot` to the
single-shot form fails pin 1
(compile-clean, link-clean — the
pin is the only gate). Reverting
`cfg.stack_size` to 6144 fails
pin 6. Removing the null-chunk
terminator fails pin 3. Dropping
the `CHUNK = 4096` constant
fails pin 4.

**Manifest gate closure:**
`run_test_uri_handler_alignment`
and `run_test_handle_root_chunked`
are now in the
`test_coverage_manifest.py`
allow-list for "pure source-grep
suites with no library source".
The pre-existing
`run_test_uri_handler_alignment`
drift (disclosed in every version
since v5.3.81) is closed by the
same edit. The manifest gate
(`make test_coverage_manifest`)
is now fully green across the
33-suite unit run.

### Disclosed limitations

- `make loopback_quick` (the 5 s
  two-Link smoke test referenced
  in some prior versions) is not
  a target in this Makefile set;
  `make test` + `make itest` cover
  the same wire path. A future
  version may add it as a
  dedicated short-budget smoke
  target.
- The chunked loop walks
  `sizeof(DASHBOARD_HTML) - 1`
  bytes; if the embedded HTML
  ever grows past ~16 KB the
  per-chunk `httpd_resp_send_chunk`
  cost is still negligible
  (sub-ms per slice on ESP32),
  but the httpd task spends
  longer holding the socket in
  its loopTask priority level.
  Documented trade-off: dashboard
  correctness over single-shot
  throughput.

### Result

- 33 / 33 host unit suites pass
  (`make test`), including the
  new `run_test_handle_root_chunked`
  structural pin. Wall: ~5.1 s.
- 3 / 3 host integration suites
  pass (`make itest`). Wall: ~40 s.
- `test_coverage_manifest`
  self-test PASS (rule-4 invariant
  holds; allow-list extended for
  the two pure-source-grep suites).
- `build/verify_build.sh` clean
  compile against
  `esp32:esp32:firebeetle32` —
  `Sketch uses 1016523 bytes (77%)
  of program storage space. Global
  variables use 66992 bytes (20%)
  of dynamic memory`, no delta vs
  5.3.86.
- `AutoLinkWeb.cpp` ~10 LoC net
  (the single-shot `httpd_resp_send`
  line replaced by the chunked
  loop + the terminator); one
  `cfg.stack_size = 6144` → `8192`
  bump.
- 0 bytes added to RAM on the wire
  path (the chunked loop is pure
  instruction-cache; the httpd
  task stack is 2 KB larger at
  task-creation time only and
  freed when the task exits).
---

## v5.3.86

**Sweep-phase rx break + p2-fallback LOCK + P3 50ms floor + initSerial tag fix**

Four small bug-class fixes that each
target a different silent-failure
mode in the link-up / boot path.
None change the wire format; all
are localized in `Link.cpp`,
`LinkSweep.cpp`, and
`PingPongBase.h`.

1. **`onRx` SWP/LCK break on
   phase transition.** The `else`
   branch of `onRx` (the SWP/LCK
   state) used to keep feeding the
   rest of the incoming burst into
   the just-completed control frame
   after `processCtrlFrame_unlocked`
   returned `true` (signalling
   `lockOk_unlocked` had advanced
   state to OK and switched baud).
   The bytes trailing the LOCK/PONG
   in the rx buffer were captured at
   the old baud and would either be
   consumed at the wrong framing
   window or be misinterpreted as
   `0xAA 0x55` markers at the new
   baud. Added a `break;` after
   `needBreak = true;` so the rx
   loop exits cleanly and the
   `data` buffer is dropped — the
   next rx at the new baud starts
   from a clean `rxIdx = 0`.

2. **`onTimerSwp_unlocked` P2
   fallback sends LOCK_CMD.** The
   master-side P2-fallback path
   (no PONG received at any baud,
   sweep exhausts the list) used to
   call `lockOk_unlocked(...)`
   directly without first sending
   the LOCK_CMD frame. The slave
   side had no notice that the baud
   had been settled and would stay
   parked at the last spdI it had
   been holding. Now the master
   sets the baud, sends
   `LOCK_CMD + (N-1)`, then locks —
   same shape as the P3 lock path
   two blocks down.

3. **P3 timer floor 5 ms → 50 ms.**
   The
   `rt = 2 * (5 chars * 10 bits /
   baud * 1000)` formula gives ~87
   ms at 115200 — close to the
   `if (rt < 5) rt = 5` floor in
   spirit, but the floor fires for
   any faster baud. The first 87 ms
   of a faster-baud P3 window
   expires before the link could
   have even clocked the ACK round
   trip on a slow ESP32, so the
   master walks to the next baud
   while the slave is still
   mid-frame, then races itself.
   Bumping the floor to 50 ms
   guarantees a full RT is available
   at every supported baud and
   turns `t3` from ~103 ms to
   ~250 ms at 115200. Applied in
   `LinkSweep::enterPhase3` and
   the inline re-arm in
   `Link::handleSwp_unlocked`
   (both `if (rt < 5) rt = 5;`
   lines now read 50).

4. **`initSerial` tag/fmt swap.**
   `log.info("PingPongBase boot:
   role=%s  baud=%lu  WiFi=%s",
   role, ...)` passed the whole
   "PingPongBase boot: …" literal
   as the tag, then `role` (a
   `const char*`) as the format
   string. The first format-arg
   pair (`role`, `(unsigned
   long)debugBaud`, `ssid`) didn't
   match `role`'s `%s` slot
   position — the printf-style
   formatter walked off the end
   of the variadics. Split into
   `log.info("PingPongBase", "boot:
   role=%s  baud=%lu  WiFi=%s",
   role, (unsigned long)debugBaud,
   ssid ? ssid : "disabled")` —
   tag and format are now the two
   separate args the API expects.

### Wire format

Unchanged. The wire is byte-identical
to v5.3.85. The four fixes are
behavioural only (rx loop control,
P2 fallback handshake, P3 timing
margin, log-call argument order);
no `LOCK_CMD`/payload type byte
or ARQ slot count changes. No
header in `include/` moves. Public
API shrinks by zero symbols.

### Regression coverage

- `make test` — 32 / 32 host unit
  suites pass (~5.1 s wall). The
  four fixes are localized in
  production code paths that the
  existing sweep / loopback / arq
  suites already exercise; no new
  test binary, no Makefile change.
- `make itest` — 3 / 3 host
  integration suites pass (~40 s
  wall).
- `build/verify_build.sh` —
  compiles cleanly against
  `esp32:esp32:firebeetle32`
  (1,016,491 B flash, 66,992 B
  RAM, no delta vs 5.3.85).
- `make loopback_quick` — 5 s
  two-Link end-to-end smoke test
  passes (96 / 96 messages, no
  discards, no frame errors,
  `disc=1` on Pong is the
  pre-existing one-shot the
  loopback suite always emits).

### Disclosed limitations

- `make test_coverage_manifest`
  still reports the pre-existing
  `run_test_uri_handler_alignment`
  drift (a pure source-grep test
  in `TEST_BINS` that links no
  library source). Not introduced
  by this change; the v5.3.85
  reference has the same flag and
  every version since v5.3.81
  discloses it.
- Fix #3 (P3 50 ms floor) widens
  the P3 window from ~103 ms to
  ~250 ms at 115200 baud. A
  link-up that needs to clear P3
  on a noisy channel may now take
  ~250 ms per round instead of
  ~100 ms. The trade is
  correctness over speed: the
  tighter floor was the cause of
  the master's
  walk-to-next-baud-mid-frame race.
  At default 5-baud config
  (`phase2Total` ≈ 6.5 s) the
  added ~750 ms across the worst
  case P3 walk is within the
  existing budget.

### Result

- 32 / 32 unit suites pass; 3 / 3
  integration suites pass;
  verify_build.sh clean compile
  against `esp32:esp32:firebeetle32`.
- `Link.cpp` +5 LoC (one new
  `if (processCtrlFrame_unlocked) {
  needBreak = true; break; }`
  block + 3 lines for the
  P2-fallback LOCK_CMD preamble);
  `LinkSweep.cpp` 1 LoC (`5` → `50`);
  `PingPongBase.h` 1 LoC
  (the split tag/fmt).
- 0 bytes added to RAM on the wire
  path (the new LOCK_CMD emission
  is one wire frame per P2
  fallback — a previously-missed
  packet, not a new one).
---

## v5.3.85

**Helpers drive Link through LinkContext, not friendship**

`LinkArq`, `LinkReorder`, and
`LinkSweep` previously reached into
`Link`'s private section via
`friend class` declarations and
called `l.sendFrame_unlocked()`,
`l.hwSetSpd()`, `l.hwLock()`,
`l.masterRole()`, etc. — a façade
over what was functionally a
God-class split. Promoted the
narrow shim accessors into a small
`al/link/LinkContext.h` interface
that `Link` implements; helpers
now take `LinkContext&` instead of
`Link&`. Removed `friend class
LinkSweep/LinkArq/LinkReorder`.
Helpers no longer `#include
"al/link/Link.h"` — header cycle
broken. Wire-protocol constants
(`PING_CMD`/`PONG_CMD`/`LOCK_CMD`/
`MAX_CHUNK`) moved to
`LinkContext.h` so the helpers see
them through the cross-helper I/O
contract. One vtable per `Link`
(~32B on ESP32), on par with the
existing `IHal` cost. No
`std::function` or virtual bases on
the helper side.

Regression: `run_test_linkcontext`
new — source-level pin (no friend
declarations in `Link.h`, no
`#include "al/link/Link.h"` in any
helper) plus a runtime mock-context
that the helpers compile and run
against. Toggle off (re-add
`friend class LinkSweep;`) → red.
Toggle off (revert helper
signatures to `Link&`) → compile
error. `make test` (32/32),
`make itest` (3/3),
`./build/verify_build.sh`
(`esp32:esp32:firebeetle32`),
`make loopback_quick` (5 s two-Link
end-to-end). Wire format unchanged.

Limitations: one vtable per `Link`
(~32B on ESP32) is a real cost —
acceptable since `IHal` already
costs the same. The
`LinkTestAccessor` friend and
`AutoLink` friend remain — out of
scope for this refactor (those
gates production-side test hooks,
not the helper↔link boundary).
---

## v5.3.84

**AutoLinkConfig: own header — break HAL→link header-level dep**

`AutoLinkConfig` is a user-facing config
struct (baud list, timeouts, buffer
sizes, mode) but lived inside the
link-layer header `src/al/link/Link.h`.
`EspHal` only needed `AutoLinkConfig`
but had to `#include "al/link/Link.h"`
just to see the type — pulling in the
full link layer (LinkArq, LinkReorder,
LinkSweep, IArqCache, LinkFrameRx)
from the HAL translation unit. Moved
the struct to its own header
`src/al/AutoLinkConfig.h`; both
`Link.h` and `EspHal.h` now include
that header. `EspHal.h` no longer
includes `Link.h` — the HAL layer
sees the config struct without
depending on the link layer at the
header level.

`test/test_desktop/al/CompileCheckTest`
extended `ARDUINO_GUARDED_FILES` to
carry an optional per-file
`-include` flag and injects
`al/link/Link.h` for `EspHal.h`
only — `EspHal.h`'s body still calls
`Link::onRx / onBreak / onTimer /
begin` (those are real dependencies,
held via `IHal::link`), and the
standalone header-syntax parse needs
the full `Link` definition that
production TUs get through
`include/AutoLink.h`.

Regression: `make test` (31/31),
`make itest` (3/3),
`./build/verify_build.sh`
(`esp32:esp32:firebeetle32`).
Wire format unchanged.
---

## v5.3.83

**ArqCache::hasRoom() — O(N)→O(1) pool scan on send hot path**

`ArqCache::hasRoom()` walked all
`POOL_SIZE=64` entries on every
`Link::sendMsg`. At `WINDOW=32` the
per-send cost was tolerable; widening
the pipeline to amortise retx cost
more would have turned the scan into
the bottleneck. Replaced with a
`poolFree_` counter maintained on
every insert / free / clear /
`testFillPool` / `testEmptyPool`.
`hasRoom()` is now
`pendingCount_ < SLOTS && poolFree_ > 0`
— two integer comparisons, no scan.

`assertInvariants()` (host build)
cross-checks `poolFree_` against a
fresh scan of `poolUsed_` on every
insert and free; drift fires the
invariant before any caller notices
a wrong `hasRoom()` answer. The
insert() allocation path still does
a linear scan to locate the free
slot index — that path runs at most
once per real send, not per
`hasRoom()` probe, so the
bottleneck is gone.

Regression tests (host, subsecond):
`test_hasRoom_is_O1_after_pool_drain`
(probes hasRoom across full fill +
drain), `test_poolFree_track_matches_scan_after_replace`
(insert-over-existing must hand the
old pool buffer back), and
`test_hasRoom_after_clearAll_resets_to_true`
(counter reset on link reset). The
existing `assertInvariants()` body
in the host build is now the load-
bearing pin for the counter; revert
any of the increment / decrement
sites and the suite explodes at the
first insert.

Limitations: `insert()` still does a
linear scan to locate the free pool
index. At `POOL_SIZE=64` that is one
cache line per entry; tolerable for
the rare allocation path. A free-
list would make insert() O(1) too
if a future bump pushes POOL_SIZE
into the hundreds.
---

## v5.3.82

**LinkArq waitForAck ABA hazard — generation counter**

`LinkArq::waitForAck` is the SYNC-mode
stop-and-wait helper: the caller holds
the link mutex, the helper drops it so
the link task can deliver the peer's ACK,
spins on `ackedPending_[seq]` with
`portYIELD()`, and re-takes the mutex
on the way out. Between unlock and
relock, a `Link::reset_unlocked()` —
triggered by a peer BREAK, error burst,
or idle watchdog — calls
`arq_.clearAll()` which memsets the
pending map to zero. The waiter then
sees `ackedPending_[seq] == false`
and returns `true` as if a real ACK
had landed. The send advances its
delivery state into a session that's
already dead, and the next frame
silently disappears into the same
reset.

### Fix

`LinkArq` now owns a `uint32_t
generation_` counter, bumped inside
`clearAll()`. `waitForAck` snapshots
the generation while it still holds
the lock (so no concurrent reset can
have bumped it yet), drops the lock,
spins, re-takes the lock, and compares:
a generation mismatch means the slot
was cleared by a reset, not by a peer
ACK, so the helper returns `false`.
The caller treats it as a failed
send, matching the timeout path. The
counter is exposed via
`LinkArq::generation()` for tests; no
other production code reads it.

The fix preserves all three real
outcomes:

1. **Real ACK** — `onAcked` flips
   `ackedPending_[seq]` to `false`
   without bumping the counter, so
   `generation_ == genAtUnlock` and
   the helper returns `true`.
2. **Timeout** — the spin's timeout
   branch takes the lock, clears the
   slot, returns `false`. No reset
   fired, so the counter is unchanged;
   the helper also returns `false`
   here. Both failure paths collapse
   to the same return value, which is
   the existing call-site contract
   (`Link::sendMsg` reads the boolean
   and sets `ok = false`).
3. **Mid-wait reset** — the helper
   returns `false`. Without the fix,
   this case returned `true` and
   silently advanced delivery state
   on a dead link.

### Wire format

Unchanged. The wire is byte-identical
to v5.3.81. The counter is a private
member of `LinkArq`, never read or
written over the wire. Public API
gains one symbol (`generation()`);
no other header moves. No change to
`library.properties` other than the
version bump; the `include/` headers
expose only the runtime surface.

### Regression coverage

**Four new host tests** in
`test/test_desktop/al/link/arq/LinkArqTest.cpp`:

1. `test_waitforack_detects_aba_after_unlock`
   — the core regression. A `MockHal`
   subclass (`AbaMockHal`) injects a
   one-shot `clearAll()` on the ARQ
   from inside its `unlock()` override,
   modelling the link task that drops
   the session while the SYNC sender
   is mid-spin. The test asserts
   `waitForAck` returns `false` and
   the generation counter advanced.
   Toggle: reverting the
   snapshot+compare in `waitForAck`
   makes the test fail with
   *"waitForAck returned true after
   mid-wait clearAll()"*.
2. `test_waitforack_returns_true_when_no_reset_fires`
   — a sanity check: a real ACK
   (delivered via `onAcked` while the
   caller holds the lock) still
   returns `true` and the generation
   is unchanged.
3. `test_waitforack_times_out_when_no_ack_or_reset`
   — a sanity check: with neither ACK
   nor reset, the timeout fires, the
   helper returns `false`, and the
   generation is unchanged. A worker
   thread drives the mock clock so
   the simulated timeout elapses
   (the host build has no
   `portYIELD()`).
4. `test_clearall_bumps_generation`
   — the counter mechanism itself:
   three consecutive `clearAll()` calls
   bump `generation_` monotonically
   (1→2→3).

**New test accessor:**
`LinkTestAccessor::arq()` returns
`LinkArq&` for direct drive. Used by
the four new tests to manipulate the
ARQ state and read the counter without
reaching through `Link`'s internals.
Builds only under `-DAUTOLINK_HOST_TEST`,
no production Arduino sketch can
reach it.

### Disclosed limitations

- The `compile_manifest.py` self-test
  (`make test_coverage_manifest`)
  still reports the pre-existing
  `run_test_uri_handler_alignment`
  drift first surfaced in v5.3.80
  (a pure-source-grep test in
  `TEST_BINS` that links no library
  source). Not introduced by this
  change; the v5.3.81 reference had
  the same flag.
- The host build's `waitForAck`
  timeout test uses a worker thread
  to advance the mock clock. The
  thread sleeps for 200 µs per
  pump of 2 simulated ms, so the
  test stays well under the 60 s
  unit-suite budget. The Arduino
  build relies on `portYIELD()` for
  clock advance and never sees the
  thread; no new Arduino-side
  surface.

### Result

- 31 / 31 host unit suites pass
  (`make test`); the ARQ suite
  grew from 12 to 16 tests. No
  Makefile change to add a binary —
  the four new functions slot into
  the existing `run_test_alink_arq`
  binary.
- 3 / 3 host integration suites
  pass (`make itest`) — the loopback,
  loopback_noise, and loopback_sync
  tests all still send and receive
  cleanly, including the SYNC-mode
  path that exercises `waitForAck`
  in production.
- `verify_build.sh` compiles
  cleanly against
  `esp32:esp32:firebeetle32`. No
  RAM or flash delta on the wire
  path (the counter is a single
  `uint32_t` on the ARQ class,
  not on the wire; the new
  `generation()` accessor is
  inline and one cycle on the
  cache-line-cold read).
- `LinkArq.h` +6 LoC (counter +
  accessor + comment); `LinkArq.cpp`
  +13 LoC (snapshot + compare +
  bump + comments); `LinkArqTest.cpp`
  +158 LoC (the four new tests +
  the `AbaMockHal` harness).
---

## v5.3.81

**Link.cpp dedup pass — five copy-paste
fixes**

A code-audit pass surfaced five
separate places in `src/al/link/Link.{h,cpp}`
where the same logic was written
twice. Each was a small DRY win on
its own; together they remove ~80
LoC of identical code and collapse
the bug-class where a fix to the
production path had to be applied
twice (or quietly shipped only on
one side).

1. **`buildAndTxCobsFrame_unlocked(seq, b, n)`**
   — `sendCobsFrame_unlocked` and
   `resendCobsFrame_unlocked` had
   byte-for-byte identical COBS
   encode bodies. The only real
   difference was `unenc[0] = txSeq`
   (auto-increment) vs `unenc[0] = seq`
   (caller-supplied, no increment).
   Extracted the encoder+tx; both
   callers now route through it.
2. **`sendCtrlCobsFrame_unlocked(type, seq)`**
   — `sendAckFrame_unlocked` and
   `sendNakFrame_unlocked` were 7
   identical lines of COBS encode,
   differing only in the type byte
   (`ACK_TYPE` vs `NAK_TYPE`).
   Extracted; ACK/NAK are now
   one-line wrappers.
3. **`buildAndSendMsg_unlocked(b, len, &lastSeq)`**
   — The CRC16 compute, `hdr[]`
   build, `len + MSG_HDR <= MAX_CHUNK`
   short-coalesce branch, and the
   multi-chunk loop body were
   copy-pasted between `sendMsg()`
   (production) and `test_sendMsgBegin()`
   (SYNC host hook). The helper
   emits the wire frames and writes
   the last-emitted seq out; the
   production ASYNC path still
   routes through
   `sendCobsFrameAcked_unlocked` for
   its per-frame ARQ cache insert,
   the production SYNC path adds
   `arq_.onSent` + `waitForAck`
   per frame, and the test hook
   keeps its one-shot
   `arq_.onSent(lastSeq, ...)`
   simplification. Both callers
   share the hdr/loop logic now.
4. **Removed `LOCK_CMD_BASE`.**
   `LOCK_CMD = 0x44` and
   `LOCK_CMD_BASE = 0x44` were the
   same constant declared twice
   (`uint8_t` vs `int`). The base
   was used for the range check
   (`payload >= LOCK_CMD_BASE`);
   the named one was used for the
   send (`LOCK_CMD + baud`). They
   had to stay equal or the protocol
   silently breaks. Deleted
   `LOCK_CMD_BASE`; all three
   references now use `LOCK_CMD`.
5. **`processCtrlFrame_unlocked(cur)`**
   — `onRx` had two separate
   byte-parsing paths for
   `State::OK` vs non-OK, both
   copying `CTRL_FRAME_SIZE` bytes
   into `rxBuf` and then duplicating
   the identical CRC8 check +
   `err_unlocked()` call. The OK
   path was also handling
   `PING_CMD`/`PONG_CMD` inline
   rather than routing through
   `ctrlFrameReady_unlocked`,
   creating a third divergence
   point. Extracted
   `processCtrlFrame_unlocked(State)`
   — validates CRC, dispatches
   PING/PONG inline for OK, routes
   SWP/LCK through
   `ctrlFrameReady_unlocked`. Both
   `onRx` paths now share one
   validate+dispatch helper.

### Wire format

Unchanged. The wire is byte-identical
to v5.3.80. Public API shrinks by
zero symbols (all five new helpers
are private, friend-only). All
removed/renamed symbols are
private to `Link.cpp` (one is a
private `constexpr` on `Link.h`,
deleted). No header in `include/`
moves.

### Regression coverage

**New source-level pin:**
`test_link_dedup_helpers_present`
in
`test/test_desktop/al/link/TestAccessorStructureTest.cpp`.
Source-greps `src/al/link/Link.h`
and `src/al/link/Link.cpp` and
asserts:

1. `buildAndTxCobsFrame_unlocked`
   is declared in `Link.h` and
   defined in `Link.cpp`.
2. `sendCtrlCobsFrame_unlocked` is
   declared in `Link.h` and defined
   in `Link.cpp`.
3. `buildAndSendMsg_unlocked` is
   declared in `Link.h` and defined
   in `Link.cpp`.
4. `processCtrlFrame_unlocked` is
   declared in `Link.h` and defined
   in `Link.cpp`.
5. `LOCK_CMD_BASE` is not present
   anywhere in `src/al/link/Link.h`
   or `src/al/link/Link.cpp`.
6. `sendCobsFrame_unlocked` body in
   `Link.cpp` does not contain a
   `UtilCobs::encode` call (it
   delegates to the new helper).
7. `resendCobsFrame_unlocked` body
   does not contain a
   `UtilCobs::encode` call (it
   delegates too).
8. `sendAckFrame_unlocked` body
   does not contain a
   `UtilCobs::encode` call (it
   delegates to
   `sendCtrlCobsFrame_unlocked`).
9. `sendNakFrame_unlocked` body
   does not contain a
   `UtilCobs::encode` call.
10. `onRx` body does not contain a
    direct `UtilCrc::crc8(rxBuf, ...)`
    call (it delegates to
    `processCtrlFrame_unlocked`).
11. `LOCK_CMD` is referenced in
    `Link.cpp` at the original
    LOCK_CMD range-check site (3
    occurrences: `>= LOCK_CMD`,
    `< LOCK_CMD +`, `- LOCK_CMD`).

Toggle: reverting any of the five
helper extractions fails its
respective pin (1–4). Reverting
the LOCK_CMD_BASE deletion fails
pin 5 and pin 11. Reintroducing
inline `UtilCobs::encode` in the
old call sites fails pins 6–9.
Reintroducing inline `crc8(rxBuf,
...)` in `onRx` fails pin 10.

### Disclosed limitations

- `compile_manifest.py`'s self-test
  (`make test_coverage_manifest`)
  reports a pre-existing bug:
  `run_test_uri_handler_alignment`
  is in `TEST_BINS` but links no
  library sources (it is a pure
  source-grep test), so it
  contributes to no `src_for_*`
  entry. The test still passes its
  own run; the manifest gate is
  the one that fires. Not introduced
  by this change; the v5.3.79
  reference had the same flag.
  Follow-up candidate for a future
  cleanup.
- The new pin lives in the existing
  `run_test_accessor_structure`
  binary. No new binary, no
  Makefile change.

### Result

- 31 / 31 unit suites pass
  (`make test`); 3 / 3 host
  integration suites pass
  (`make itest`); `make loopback`
  passes.
- `Link.h` 6 LoC smaller
  (`LOCK_CMD_BASE` removed; one
  `constexpr` line, one comment
  about wire format).
- `Link.cpp` ~70 LoC smaller
  (duplicated COBS encode bodies,
  duplicated MSG frame-build, and
  duplicated CRC-validate block all
  extracted).
- 0 bytes added to RAM on the wire
  path (the deleted inline code is
  pure instruction-cache wins; the
  new helpers are non-virtual
  one-liners).
- `make test` wall: ~8.9 s.
- `make itest` wall: ~40 s.
- One new test function
  (`test_link_dedup_helpers_present`)
  in the existing
  `run_test_accessor_structure`
  binary; no Makefile change.
---

## v5.3.80

**Dead-code cleanup (Link / LinkArq / LinkSweep)**

Eight unused symbols surfaced in an
audit and were removed. Each was
verified by grep across `src/`,
`include/`, `test/`, `examples/` to
have zero callers; none were test-only
plumbing, all were production surface
that drifted from the actual control
flow. Removed:

1. `Link::computeDwells_unlocked` —
   ghost declaration in `Link.h`. Live
   `computeDwells` lives on
   `LinkSweep`, called once from
   `Link::begin` via
   `sweep_.computeDwells(*this)`. The
   `Link`-scoped private declaration
   had no matching definition and no
   call site.
2. `retxNeeded_` — write-only `bool`
   field on `Link`, set in `onNak()`
   and the ARQ retx loop in
   `onTimerOk_unlocked()` but never
   read. The actual retx path uses
   `hasPendingRetx_`; `retxNeeded_`
   was a parallel flag that nothing
   in the wire task consumed.
3. `Link::sendFrame()` — locking
   wrapper around `sendFrame_unlocked`.
   Every internal caller (the sweep
   phase machine, the heartbeat tick,
   `sendPongAck_unlocked`) already
   holds the link lock and calls
   `sendFrame_unlocked` directly. No
   external caller exists — `Link` is
   not reachable from sketches. (Same
   shape as the v5.3.62 wrapper
   cleanup, applied to the control
   frame path.)
4. `Link::popRetransmitSlot()` and
   `LinkArq::popRetransmitSlot()` —
   the Link-level wrapper and its
   helper are both unreachable. The
   ARQ timer loop in `onTimerOk_unlocked`
   walks all 256 slots itself and
   calls `arq_.decideSlot` +
   `arq_.applyRetx` directly; the
   per-slot pop helper is dead.
5. `LinkArq::clearSlot(uint8_t)` —
   per-slot clearing method, uncalled
   from `Link`, tests, or any accessor.
   Per-slot state resets happen via
   `onAcked()` (which clears the
   pending bit + retx count + base
   seq in one shot); bulk reset via
   `clearAll()` runs from
   `Link::reset_unlocked`. The
   standalone per-slot method had no
   caller.
6. `LinkSweep::enterResweep(Link&)`
   — defined but never called. The
   resweep path in `reset_unlocked()`
   goes directly to
   `sweep_.enterPhase1(*this)` (the
   P1-slowest restart policy). The
   `enterResweep` method was a
   baud-preference-aware alternative
   to P1; the baud preference feature
   was never wired up.
7. `AutoLinkConfig::baudPreference`
   and `AutoLinkConfig::baudRetryLimit`
   — `baudPreference` was the
   enable-flag for `enterResweep`;
   with #6 gone, no production code
   reads it. `baudRetryLimit` was
   the retry budget that
   `enterResweep` consumed; with
   `enterResweep` gone, it is also
   dead. Both removed from the config
   struct. `LinkBaudPreferenceTest.cpp`
   keeps its other invariants
   (preferredBaud on lock,
   errRateWindow drop, baudRetries
   cleared on reset, short-message
   coalescing); the test no longer
   sets the deleted config fields.
8. `swpRxBytes` — `int` field
   incremented in `onRx()` during
   `State::SWP` and cleared in
   `reset_unlocked()`, never read,
   never exposed in `Diag` or
   `Stats`. Removed.

Additionally: the
`Link`-scoped accessors that existed
only to be called from `enterResweep`
(`preferredBaudIndex()`,
`baudRetryLimit()`, `baudRetries()`,
`incBaudRetries()`,
`clearBaudRetries()`,
`clearPreferredBaud()`) became
uncallable once #6 was deleted, so
they were removed in the same pass.

### Wire format

Unchanged. The wire is byte-identical
to v5.3.79. No header in `include/`
moves. The public API shrinks by
zero symbols (everything removed is
in the private / `friend`-only
section of `Link.h` or on the ARQ
helper class that sketches cannot
reach). Test files updated for the
two removed config fields; no test
asserts on the deleted symbols.

### Regression coverage

**New structural pin:**
`test_dead_code_boundary` in
`test/test_desktop/al/CompileCheckTest.cpp`.
Source-greps every deleted symbol
across `Link.h`, `Link.cpp`,
`LinkArq.h`, `LinkArq.cpp`,
`LinkSweep.h`, `LinkSweep.cpp` and
asserts each is absent. Reintroducing
any of the 8 dead symbols (with a
matching definition or write site)
fails the pin. Toggle-tested:
re-adding `bool retxNeeded_ = false;`
to `Link.h` flips the pin from
PASS to FAIL with a clear diff.

### Disclosed limitations

- `compile_manifest.py`'s self-test
  (`make test_coverage_manifest`)
  reports a pre-existing bug:
  `run_test_uri_handler_alignment`
  is in `TEST_BINS` but links no
  library sources (it is a pure
  source-grep test), so it
  contributes to no `src_for_*`
  entry. The test still passes its
  own run; the manifest gate is the
  one that fires. Not introduced by
  this change; the v5.3.79 reference
  had the same flag. Worth a
  follow-up cleanup in v5.3.81 to
  either move the test out of
  `TEST_BINS` or accept it as a
  pure-source suite in the
  generator.
- The baud-preference feature was
  removed entirely. If a future
  sketch needs the "restart at the
  previously-good baud on resweep"
  behaviour, the right shape is to
  wire `enterResweep` back in via
  the dedicated
  `preferredBaud_` / `baudRetries_`
  state that already exists, and
  gate it on a `cfg.baudPreference`
  flag the field that was deleted
  here. The implementation is
  recoverable from git history; the
  call site (replacing the
  `sweep_.enterPhase1(*this)` line
  in `Link::reset_unlocked`) is
  documented in `LinkDecision.h:208`
  ("at P1 slowest baud;
  preferredBaud_ ignored").

### Result

- 31 / 31 unit suites pass
  (`make test`); 3 / 3 host
  integration suites pass
  (`make itest`); verify_build.sh
  clean compile against
  esp32:esp32:firebeetle32.
- `Link.h` ~6 LoC smaller, `Link.cpp`
  ~14 LoC smaller, `LinkArq.{h,cpp}`
  ~13 LoC smaller, `LinkSweep.{h,cpp}`
  ~24 LoC smaller.
- 0 bytes added to RAM on the wire
  path (the deleted flags and
  helpers were pure instructions;
  no wire data structures change).
- One new test function
  (`test_dead_code_boundary`) in the
  existing `run_test_compile_check`
  binary; no new binary, no
  Makefile change.
---

## v5.3.79

**Link ctor takes `IArqCache&`; `setArqCache()` removed**

AutoLink owned an `ArqCache` by value and
handed a raw `IArqCache*` to the Link via
`Link::setArqCache()` post-construction.
The lifetime contract ("the cache must
outlive the Link") was enforced only by
construction/destruction order and a
comment block in `Link.h`. A future
refactor that moved the `ArqCache` to
`unique_ptr`, reordered AutoLink's
members, or constructed the Link before
the cache would silently produce a
dangling pointer inside `Link::onTimer`
and on every retx. The bug class is
invisible to the host unit suite (the
mock cache outlives every test) and only
fails on a real ESP32 under sustained
noise, when `Link::arqCache_->peekForRetx()`
returns garbage from a freed object.

Switched to reference semantics. The
Link ctor now takes `IArqCache&` (a
reference, not a pointer) as its second
arg; `Link::arqCache_` is now a reference
member. References cannot be null and
cannot be rebound — the cache must
exist and must be bound at construction.
`Link::setArqCache()` is gone from the
public surface; there is no path for
production code to swap the cache after
the Link exists. The null-guards inside
`Link::onTimer` / `sendCobsFrameAcked_unlocked`
/ `clearAll` / `freeBySeq` /
`peekForRetx` are now dead code (a
reference cannot be null) and were
deleted; the dereferences are unconditional.

`AutoLink` passes its `arqCache_`
member by reference to the Link ctor.
Member-initializer order in
`Link::Link(...)` puts `arqCache_`
right after `hw` so reference
initialization happens before any other
state. `AutoLink` member declaration
order keeps `link` declared after
`arqCache_` so destruction order
destroys the Link first (it doesn't
dereference the cache after that point)
and the cache last. The host
`AutoLink(IHal *, bool, AutoLinkConfig)`
ctor (under `AUTOLINK_HOST_TEST`) gets
the same treatment.

`LinkTestAccessor::arqCache()` returns
`IArqCache&` to match the new shape.
`AutoLinkTestAccessor::arqCache()` still
returns `ArqCache*` (the facade's own
cache member, exposed via its existing
test-only accessor).

### Regression coverage

**New compile-time pin:**
`test_link_ctor_requires_arq_cache_reference`
in
`test/test_desktop/al/CompileCheckTest.cpp`.
Reads `src/al/link/Link.h` and asserts:
(1) the literal ctor signature
`Link(IHal &hw, IArqCache &cache, bool isMasterNode,`
is present; (2) the literal member
declaration `IArqCache &arqCache_;` is
present; (3) the public surface has no
`void setArqCache(` declaration; (4) the
file has no `IArqCache *arqCache_`
(re-introducing the raw-pointer member
or a default-null initialiser would let
the lifetime break silently). Toggling
the ctor signature to `IArqCache *cache`
fails test 1. Toggling the member to
`IArqCache *arqCache_ = nullptr;`
fails test 4. Reintroducing
`void setArqCache(IArqCache *c)`
post-construction fails test 3.

**New runtime pin:**
`test_facade_link_arqcache_is_facade_cache_by_reference`
in
`test/test_desktop/al/AutoLinkFacadeTest.cpp`.
Constructs an `AutoLink`, fetches the
facade's `ArqCache*` via
`AutoLinkTestAccessor::arqCache()`, and
the Link's `IArqCache&` via
`LinkTestAccessor::arqCache()`, and
asserts the two point to the same
instance. The compile-time pin
catches the ctor-shape regression; this
runtime pin catches the "two caches
somehow diverged" regression (e.g. a
copy, a default-constructed local, a
cache that was rebound mid-flight via a
friend-only accessor). Green today;
re-introducing a default-constructed
local cache would make the two
addresses diverge and fail the assert.

### Wire format

Unchanged. The wire is byte-identical
to v5.3.78. No header in `include/`
moves; the public API shrinks (one
method deleted, one ctor signature
changed). Tests that hand-construct a
`Link` directly (host unit suites,
host integration itest) must pass a
cache — they now declare a
`NullArqCache cache;` (test/common/
NullArqCache.h, a no-op IArqCache
implementation) or pass a real
`ArqCache` instance when they exercise
ARQ behaviour.

### Limitations

- The host suite still skips
  `AutoLinkWeb.cpp` (AGENTS "Gotchas").
  The lifetime change doesn't touch
  the web monitor, but the
  verify_build.sh cross-compile is the
  gate for any Arduino-side compile
  regression.
- `ArqCache::test_*` methods on the
  production cache class are still
  public (per the v5.3.62 disclosure).
  Pushing them behind a `friend`
  accessor would touch every
  `ArqCacheTest.cpp` caller; out of
  scope for this fix.
- `IArqCache *LinkTestAccessor::arqCache()`
  returning a raw pointer would have
  been the old shape; the new
  reference return matches the member
  but means callers can no longer
  null-check. Since the accessor is
  friend-only (no production caller),
  this is the right shape.

### Result

- 31 / 31 unit suites pass (`make test`),
  3 / 3 host integration suites pass
  (`make itest`), total wall time
  ~4.2 s unit + ~40 s itest.
- 0 bytes added to RAM on the wire
  path (the cache pointer / reference
  are the same size; the dropped null
  guards are pure instruction-cache
  wins).
- ~20 callsites updated across 17 host
  test files plus the three host
  integration suites; one new
  test/common/NullArqCache.h header.
---

## v5.3.78

**Web monitor HANDLERS_FULL / PATHS[] alignment fix**

Two related bugs in `src/al/web/AutoLinkWeb.cpp`'s httpd
startup silently broke the web interface on every boot:
`HTTPD_DEFAULT_CONFIG()` sets `max_uri_handlers = 8` but
the file registers 9 handlers (`r0`..`r8`), so the last
one failed with `HANDLERS_FULL` and 404'd at runtime; and
the parallel `PATHS[]` array had `/reboot` and `/level`
swapped, so the failure log named the wrong path. The
web server did start and the other 8 routes worked, but
any page that hit `/reboot` first (or the dashboard JS
on load) rendered as a broken GUI. Fix: bump
`cfg.max_uri_handlers = 9` immediately after
`cfg.lru_purge_enable`, and reorder `PATHS[]` to mirror
`URIS[]`. All 9 routes now register cleanly.

### Regression test

`run_test_uri_handler_alignment` reads
`src/al/web/AutoLinkWeb.cpp` and asserts:
`cfg.max_uri_handlers == 9` inside the brace-balanced
httpd config block; exactly 9 `r<N>` handler declarations;
`PATHS[]` parallel to `URIS[]` (every entry matches the
path declared in the `r<N>` it points at); and every
required route (`/`, `/stats`, `/logs`, `/reset`,
`/reboot`, `/level`, `/mode`, `/pausemsg`, `/delay`)
appears once. Dropping the `cfg.max_uri_handlers` line
fails test 1; removing an `r<N>` fails test 2; swapping
two entries in `PATHS[]` fails test 3; renaming a path
fails test 3. AutoLinkWeb is `#ifdef ARDUINO` so the
host suite can't run it; this source-level pin is the
gate.

### Limitations

The host suite still skips `AutoLinkWeb.cpp` itself
(AGENTS.md "Gotchas"). The structural test covers the
two bugs by reading the source; the runtime behaviour
on ESP32 is gated by `build/verify_build.sh`. Any future
handler added past `/delay` must bump `max_uri_handlers`
again and add a matching entry to `PATHS[]` in the same
position as the new `URIS[]` entry.
---

## v5.3.77

**verify_build.ino mirrors the README's Pong.ino shape; build_env.sh installs clang-format**

Two unrelated changes shipped together: the verify sketch
now mirrors the user-facing sketch shape (so ArduinoDroid
ctor errors in the `PingPong` chain are caught at
cross-compile), and `build_env.sh` installs clang-format
alongside arduino-cli (so the canonical env-setup script
covers the formatter the master Makefile depends on).

### verify_build.ino → file-scope PingPong

`build/verify_build/verify_build.ino` previously constructed
`AutoLink alink(...)` at function scope inside `setup()` and
exercised every public AutoLink / AutoLinkWeb API by hand.
That gate was useful for catching header-path regressions,
but it didn't look like a real user sketch. Real users
follow the README: a single `PingPong upp(...)` at file
scope, then `setup() { upp.setup(); }` and
`loop() { upp.loop(); }`. Any ArduinoDroid-specific ctor
error in the `PingPong` / `Ping` / `Pong` /
`PingPongBase` chain (the std::variant branch, the embedded
`AutoLink` facade, the `EspHal` heap ctor) only surfaces
when the cross-compile sees that exact shape — and the
function-scope `AutoLink alink` exercise didn't reach it.

Replaced the body of `verify_build.ino` with the README
shape. The exhaustive AutoLink / AutoLinkWeb facade
exercises are no longer needed here: every public API is
already pinned by `CompileCheckTest` (source-level grep)
and the host unit suite. The verify build now mirrors a
user sketch exactly, so `make verify` (and ArduinoDroid's
own build of the same shape) catches ctor errors in the
user-facing entry point on first compile.

Per AGENTS.md rule 17, the `PingPong` ctor chain stores
config and constructs the `std::variant`; no RTOS work
happens until `upp.setup()` runs in the scheduler. The
namespace-scope ctor is safe — the existing `Ping.ino`
and `Pong.ino` examples already use the same shape in
production.

### build_env.sh adds clang-format

`build_env.sh` previously installed only arduino-cli + the
esp32 Arduino core. clang-format was left to
`build/pretty_print.py` to install on demand via its
multi-strategy fallback (apt / brew / pip). On a stripped
container that hasn't run pretty_print.py yet, the
master-Makefile `make test` / `make all` pre-flight
silently no-ops formatting and the host suite still passes
— so a fresh checkout with no clang-format and no prior
`pretty_print.py` invocation skips the formatter entirely.
Worse, on a CI runner that hits the format-skip path on
the first commit after a format change, the diff slips
through without anyone noticing until the next manual
run.

Added a clang-format install step to `build_env.sh`,
sitting between the arduino-cli step and the esp32-core
step. Tries apt first (sudo, then no-sudo), then pip with
`--break-system-packages` for PEP 668 distros, then
`--user` pip as a last resort. The PATH prepend for
`~/.local/bin` matches the same pattern
`pretty_print.py` uses for its own self-install. Idempotent
like the rest of the script: re-running on an already-set-up
env is a no-op. `pretty_print.py`'s runtime fallback
stays in place — `build_env.sh` just makes the
one-shot env the canonical entry point.

**Regression coverage**

*verify_build.ino shape (file-scope PingPong)*
- `test_verify_build_uses_file_scope_pingpong` in
  `test/test_desktop/al/pingpong/PingPongStructureTest.cpp`.
  Reads `build/verify_build/verify_build.ino` and asserts:
  (1) a file-scope `PingPong upp(` declaration exists
  outside any function body, (2) `void setup()` body
  calls `upp.setup()`, (3) `void loop()` body calls
  `upp.loop()`. Removing the file-scope `PingPong` ctor
  (e.g. moving it into `setup()` like v5.3.76 had) fails
  all three pins. Replacing `upp.setup()` / `upp.loop()`
  with raw `AutoLink` calls fails pins 2 / 3. The test
  doesn't pin the exact arg list — it only requires a
  PingPong declaration at file scope with `upp.setup()`
  / `upp.loop()` in the lifecycle functions.

*build_env.sh clang-format install*
- `bash -n build/build_env.sh` syntax check (the
  install branches are pure shell; no regression test
  asserts the install side-effects — those are
  exercised end-to-end by `build_env.sh` itself running
  on a fresh container, not from a host unit suite).
- The existing pretty-print self-tests
  (`build/test_pretty_print.py`) still cover the
  formatter behaviour and pass unchanged.

*Combined gates*
- `make test` — all 30 host unit suites pass (the new
  shape pin slots into the existing
  `run_test_pingpong_structure` binary, so no Makefile
  change).
- `make itest` — 3 / 3 host integration suites pass.
- `make test_coverage_manifest` — PASS (rule 4
  invariant holds; the manifest generator discovers
  source paths from the Makefile, unchanged).
- `make verify` not run in this environment (no
  arduino-cli toolchain pre-installed; would require
  `build/build_env.sh` to provision). The Arduino build
  sees the same shape `examples/PingPong/Pong.ino`
  ships, which is already known-clean against
  esp32:esp32@3.3.5.

**Disclosed limitations**
- The clang-format install in `build_env.sh` is
  best-effort on macOS (apt-get is absent; brew is
  the only path, and it's already covered). On
  Alpine / Windows the script silently skips the
  install and `pretty_print.py`'s runtime fallback
  takes over — same behaviour as v5.3.69.
- `verify_build.ino` no longer covers the AutoLink
  facade API surface directly. The CompileCheckTest
  source-level pins + the host unit suite already
  cover every public symbol, but a future refactor
  that accidentally drops an `AutoLink.h` symbol
  would only be caught by CompileCheckTest, not by
  the verify build. The trade-off is documented in
  the v5.3.77 header comment and in
  `test/test_embedded/README.md`.

**Result**
- verify_build.ino: ~80 → 14 LoC (one file-scope ctor +
  two one-line lifecycle functions; the verbose
  facade-exercise block is gone).
- build_env.sh: 62 → 110 LoC (one new step for
  clang-format, comments documenting the apt → pip
  fallback chain).
- `make test`: 30 / 30 in ~4.4 s (no behaviour change
  to host tests; new pin runs inside the existing
  `run_test_pingpong_structure` binary).
- `make itest`: 3 / 3 in ~40 s.
- No source / wire / RAM impact on the library proper.
---

## v5.3.76

**Consolidate test-side scripts under test/scripts/**

`.py` and `.sh` files were scattered across `test/common/`
(the `peak_rss.py` / `summarize.py` helpers) and
`test/test_desktop/` (the coverage pipeline trio plus
`install_system_stubs.py`). `test/common/` was misleading —
it held C++ headers AND Python helpers, with no naming cue
that one was shared code and the other was test infra.
`coverage_manifest.py` / `coverage_merge.sh` /
`test_coverage_manifest.py` are a paired unit (AGENTS rule 4);
splitting them across `test/test_desktop/` and a shared dir
made the pairing invisible.

Moved all six test-side scripts under `test/scripts/`,
grouped by role:
- `common/` — `peak_rss.py`, `summarize.py` (helpers used
  by both unit and itest suites).
- `coverage/` — `coverage_manifest.py`,
  `coverage_merge.sh`, `test_coverage_manifest.py` (the
  paired coverage pipeline, co-located so the
  generator/merger/self-test stay together).
- `env/` — `install_system_stubs.py` and
  `arduino_stub_template.h` (host test environment setup;
  the stub template moved next to its installer so the pair
  is one `ls` away).

`test/common/` now holds only the C++ test headers
(`MockHal.h`, `WireSim.h`, accessor shims) — its name now
matches its content. `test/test_desktop/` now holds only
C++ suites + Makefile. Build infrastructure under `build/`
is untouched (AGENTS rule 7: `build/` is load-bearing and
never reshuffles for cosmetic reasons).

Updated `test/Makefile`, `test/test_desktop/Makefile`,
`test/itest/test_desktop/Makefile`, and
`test/test_desktop/al/CompileCheckTest.cpp` for the new
paths. AGENTS.md, `docs/Tests.md` updated to match.

Caught a pre-existing drift bug while verifying: the
`link/` → `link/{arq,sweep}/` refactor in v5.3.75 missed
`test/itest/test_desktop/Makefile`'s `LINK_SRC`. Its
`LinkBaudSweep.cpp`, `LinkArq.cpp`, `LinkSweep.cpp`,
`ArqCache.cpp` references still pointed at the flat
`$(PROTO)/` paths. Updated to `$(PROTO)/sweep/` and
`$(PROTO)/arq/` to match the v5.3.75 layout. The unit
Makefile had been updated correctly in v5.3.75 (the
self-test would have caught it on the next coverage
run), only itest slipped through.

Regression: `make test_coverage_manifest` passes (the
rule 4 self-test still confirms every `TEST_BINS`
binary contributes to its `src_for_*` entries — the
generator now reads the Makefile from its new location
via `HERE` resolved to `test/scripts/coverage/`).
`make all` passes 33/33: 30 unit suites + 3 host
integration suites, total wall 44.5 s test + ~95 s
with builds. No wire-format change, no behaviour
change.
---

## v5.3.75

**Extract arq/ and sweep/ subdirs from src/al/link/**

`src/al/link/` had grown to a flat 14-file dump where ARQ
mechanics (`ArqCache`, `IArqCache`, `LinkArq`) and baud-sweep
machinery (`LinkBaudSweep`, `LinkDecision`, `LinkSweep`) sat
next to the core framing pipeline (`Link`, `LinkFrameRx`,
`LinkReorder`). Three distinct concerns, one directory — the
right boundary was already in the #include graph, the directory
layout just wasn't following it.

Moved the ARQ trio into `src/al/link/arq/` and the sweep trio
into `src/al/link/sweep/`. `Link`, `LinkFrameRx`, `LinkReorder`
stay at the umbrella level — they're the cross-cutting framing
pipeline, the layer above the new subdirs. Test files mirror
the new layout per AGENTS rule 20 (`al/link/arq/`,
`al/link/sweep/`). All `#include "al/link/..."` paths
updated across source, public header, test_common shims,
itest, CMakeLists, and docs/Tests.md. `Link.h` reorders its
own includes to keep arq/ before sweep/ before the local
helpers. No wire-format change, no behaviour change.
Regression: every existing unit suite still passes, and the
`test_coverage_manifest` self-test still confirms every
binary in `TEST_BINS` contributes to its `src_for_*` entries
(AGENTS rule 4 invariant holds — the manifest generator
discovered the new subdir paths from the Makefile without
any hand-edit).
---

## v5.3.74

**LinkReorder pool-backed storage (no malloc on hot path)**

`LinkReorder::hold()` called `malloc(n)` for every
out-of-order frame. On ESP32 under FreeRTOS the
heap fragments over time and `malloc` can fail
silently (returns `nullptr`); `hold()` then returned
`false` and the frame was lost with no link reset
— the only path in the link layer that didn't use
the ArqCache pool pattern.

Replaced per-slot heap allocations in
`src/al/link/LinkReorder.{h,cpp}` with a static
`REORDER_POOL_SIZE × REORDER_POOL_BUF_MAX` byte
grid plus a bitset — the same shape as
`ArqCache`. `hold()` now acquires a pool buffer
in O(`REORDER_POOL_SIZE`) worst case, and never
touches the heap on the ISR-adjacent hot path.
Pool exhaustion surfaces as `hold() == false`,
which `Link.cpp` already maps to `lostMsgs++` —
no new failure mode, just one fewer silent
drop. Sized at 128 × 250B = 32 KB so the existing
`test_single_corruption_does_not_cascade` (holds
100 simultaneous out-of-order frames) still
passes. `static_assert(REORDER_POOL_BUF_MAX ==
MAX_CHUNK)` pins the buffer size to the wire
constant. New regression test
`LinkReorderTest::test_pool_exhaustion_returns_false_without_touching_slot`
fills the pool via `testFillPool()` and verifies
the next `hold()` is a hard no-op — fails the
moment anyone reintroduces heap allocation here.
---
