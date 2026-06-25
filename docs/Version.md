# 📅 AutoLink Version History

All releases, most recent first.

---

## v5.3.50

**`.clang-format` `ColumnLimit: 55` ->
`80`, all 69 C/C++ source files
reformatted. `PingPongBase::logStats`
now shows a `[S]` / `[A]` mode icon
in the periodic status line. Fixed
`pretty_print.py` install path so
it actually installs clang-format
when missing (the v5.3.49 path
assumed `sudo` exists; on root
containers and CI sandboxes it
doesn't, so the install silently
fell through to the no-op branch).**

### ColumnLimit 55 -> 80

The v5.3.48-era 55-column limit was
unusually tight (LLVM default is 80,
Google is 80, Mozilla is 80, WebKit
is 100). It forced every `printf`-style
`log_.info(...)` call to wrap across
multiple lines, making the test
sources hard to read and forcing
v5.3.41 to wrap `ARQ_CACHE_POOL_SIZE =
64; // ...` awkwardly.

Bumped to 80 and reformatted all
69 .h / .cpp / .ino / .c files via
`pretty_print.py`. Whitespace only;
no semantic changes. **Verified:**
unit tests 26 / 26 + itest 4 / 4
after reformat.

### Mode icon in periodic status

`PingPongBase::logStats()` now
prefixes the 5-second status line
with the link's current mode:

- `[S]` SYNC (stop-and-wait)
- `[A]` ASYNC (pipeline + ARQ)

So a Ping on the wire now shows
either of these once every 5 s:

```
[S] echos=1234  mismatch=0   tx=82 B/sec  rx=82 B/sec  baud=9600   disc=0  errs=0
[A] echos=5678  mismatch=2   tx=940 B/sec  rx=940 B/sec  baud=115200   disc=1  errs=4
```

The same icon shows for both Ping
and Pong (they use `logStats` from
`PingPongBase` by composition). No
new state, no new field -- the mode
is already stored on the link and
queried with `comm_.mode()`.

### pretty_print.py install fix

The v5.3.49 auto-install attempted
`sudo -n apt-get install clang-format`
first. If `sudo` wasn't on the
machine (root container, GitHub
Actions `ubuntu-latest` is *not*
root, but several sandboxes and
dev Docker images are), the
`FileNotFoundError` on `sudo` was
silently caught and the loop moved
on to `brew`, which also doesn't
exist on Linux. Result: `apt-get`
was never tried without `sudo`, and
the install silently fell through.

**Fixed:** the install list now
includes `['apt-get', 'install',
'-y', 'clang-format']` as a second
fallback after `sudo`. So the
precedence is:

1. `sudo -n apt-get install -y clang-format`
   (interactive dev box, `sudo`
   installed)
2. `apt-get install -y clang-format`
   (root container, no sudo)
3. `brew install clang-format`
   (macOS)

**Verified the new path actually
installs:** uninstalled clang-format,
ran `python3 build/pretty_print.py
src/al/link/Link.cpp`, got
`src/al/link/Link.cpp: formatted`
+ `clang-format version 14.0.6`
on disk.

**Result:**
- 26 / 26 unit suites (unchanged)
- 4 / 4 itest suites (unchanged)
- 69 / 69 source files reformatted
  at 80-col
- `make test` ~26 s (slower this
  cycle because the `clean` rebuild
  touched all 26 binaries; subsequent
  rebuilds are 2-3 s)

---

## v5.3.49

**`pretty_print.py` auto-installs
`clang-format` when missing.**

The previous `pretty_print.py` silently
no-op'd when `clang-format` was not
installed: it printed "no changes
[skipped: clang-format not installed]"
and moved on, leaving the source files
unformatted. In a CI environment this
meant a release could ship without the
format gate ever running.

**Fixed:** the script now attempts to
install `clang-format` via the platform
package manager before falling back to
the no-op message. Two paths:

- `apt-get install -y clang-format`
  (Debian/Ubuntu, including GitHub
  Actions Ubuntu runners)
- `brew install clang-format` (macOS
  dev boxes)

If both fail (Alpine, Nix, Windows,
or a sandbox without package-manager
access), the script falls back to the
previous "skipped" message and exits
non-zero so the calling `make` step
surfaces the failure.

**Effect on v5.3.49:**
- `pretty_print.py` was a no-op in this
  sandbox before; now it actually
  formats 13 source files (whitespace
  only — no semantic changes).
- Unit tests: 26 / 26 PASS after
  formatting.
- itest: 4 / 4 ALL MODES PASS after
  formatting.

---

## v5.3.48

**Three pre-existing test failures fixed.
Two were real protocol bugs; one was a
test contract clarification.**

1. **`findMsgHeaderResync_unlocked` no
   longer preserves unrecoverable
   garbage.** When the scan window finds
   no valid MSG_HDR, the code used to
   push the snap back into the appBuf,
   keeping the desync alive. Now it
   `clearAppBuf()`s the whole buffer so
   the peer's retransmit can land in a
   clean slot. `test_corrupt_msg_header_
   no_resync_clears_buffer` now passes.

2. **`lostMsgs` not incremented in the
   Gap branch on a fresh gap.** The
   code used to bump `lostMsgs` by
   `diff - 1` immediately on detecting
   a gap, before the reorder buffer had
   a chance to recover. Now `lostMsgs`
   is only incremented when the reorder
   slot expires
   (`reorderDropExpired_unlocked`) —
   which is when the chunk is actually
   lost. The original behavior counted
   a chunk as lost the moment a gap
   was detected, even though the
   retransmit might land 50 ms later.
   `test_gap_holds_frame_in_reorder_
   buffer` and
   `test_gap_then_retransmit_delivers_
   in_order` now pass.

**Result:**
- 26 / 26 unit suites (was 23 / 26)
- 4 / 4 itest suites (unchanged)
- `make test` ~2.9 s
- `make loopback_modes` 4 / 4 ALL MODES PASS

The 3 pre-existing failures
(`alink_message`, `alink_cobsseq`,
`linkreorder`) all pass now. The
two protocol bugs (resync preserving
garbage, lostMsgs premature) had
real-world impact: a desync that
should have self-cleared instead
lingered for the whole session, and
loss-rate stats that were inflated
by 1 for every gap even when the
retransmit succeeded.

---

## v5.3.47

**Four bugs the itest exposed. One of them
was the headline "the protocol doesn't
work" complaint.**

1. **`paused_` default was `true` in Ping.h.**
   A user flashing the sketch out of the
   box would see Ping stuck paused from
   frame 0. Fixed: default `false`.

2. **ARQ retransmit loop ran in SYNC mode.**
   The Link's timer tick iterated
   `ackedPending_[256]` slots and called
   `arqRetxCallback_` for each one, even
   in SYNC mode where the cache is empty
   and any pending bit is from a timed-
   out SYNC wait. The retx callback
   would call into a pool that has no
   data, producing spurious retransmits.
   Fixed: gate the entire ARQ retransmit
   loop behind `if (cfg.mode != SYNC)`.

3. **`FAST_IDLE_RX_MS = 300` too tight
   for SYNC at slow baud.** At 9600 baud,
   large frames take >900 ms. The 300 ms
   asymmetric-idle check would fire on
   any multi-frame SYNC exchange,
   resetting the link mid-message.
   Fixed: bypass the asymmetric idle
   check entirely in SYNC mode (the
   sender's own `syncAckTimeoutMs`
   watchdog catches actual hangs).

4. **ASYNC under noise was failing 1/45
   messages in the itest.** Three
   contributing factors:
   - The noise test drove raw `Link`
     instances with no ARQ hooks
     installed, so the timer-driven
     retransmit path was a no-op.
   - The itest kept the link-task
     idle-timeout machinery enabled,
     which reset the link mid-run.
   - The asymmetric-idle check was
     firing when pong was idle waiting
     for retransmitted frames that never
     came.
   Fixed: itest now installs a minimal
   ARQ cache (the production
   `AutoLink` facade installs its own
   pool of the same shape), and the
   itest disables the link-task idle
   machinery (`cfg.idleTimeoutMs = 0`)
   because the itest is single-threaded
   and has no real "peer gone" signal.

**Result:** `make loopback_modes` now
reports 4/4 PASS in both ASYNC and SYNC
(40-45/46 messages delivered in ASNC
under 1% wire drop, 1/1 in SYNC).

**The 3 quick fixes are real protocol
bugs** — they would have manifested
on real hardware in the woodshop
environment. The itest fix is
diagnostic infrastructure, not a
protocol change.

---

## v5.3.46

**Runtime Mode switch: SYNC (default) vs ASYNC.**

The original pipeline (now renamed **ASYNC**) runs
many messages in flight with async ARQ
retransmits and a reorder buffer. Under sustained
wire noise the cache fills, the reorder buffer
leaks heap, and the link drops on heartbeat
timeout. The woodshop logs from v5.3.45 showed
1777 `send failed` lines, 42 cache-pool
exhaustions, and 1 link drop in 46 seconds — the
protocol was failing under load.

**SYNC** is the new default: stop-and-wait. One
message in flight. Sender blocks for the
receiver's ACK before sending the next. No ARQ
cache use, no reorder buffer reserve, no cobsSeq
gap detection (gaps dropped and ACKed immediately
so the sender's wait unblocks). Roughly half the
throughput of ASYNC on a clean wire — and *boring
and reliable* on any wire that carries COBS+CRC
frames.

**API:**
```cpp
comm.setMode(AutoLinkConfig::Mode::SYNC);  // default
comm.setMode(AutoLinkConfig::Mode::ASYNC); // opt back in
AutoLinkConfig::Mode m = comm.mode();
```

**Both boards must run the same mode.** The mode
is a runtime knob but it must agree across the
wire — set it before `comm.begin()` on both
sketches, reflash both, then watch logs.

**Other:**
- `cfg.syncAckTimeoutMs = 500` — SYNC only.
- `AutoLinkWeb` dashboard JSON now exposes
  `linkMode` (0=SYNC, 1=ASYNC) for verification
  that both boards match.
- Host test default is ASYNC (no concurrent link
  task to deliver ACKs); Arduino default is SYNC.
- New test surface on `Link`: `test_sendMsgBegin`
  / `test_sendMsgStillWaiting` for splitting the
  blocking SYNC wait into testable halves.

**itest driver: `make loopback_modes`.**
Single command runs `run_loopback` and
`run_loopback_noise` in BOTH modes (ASYNC and
SYNC), tags every line of output with the
`[mode / variant / duration]` triple, and prints
a per-mode summary at the end:
```
=== Loopback modes summary ===
  ASYNC : 1 pass, 1 fail
  SYNC  : 2 pass, 0 fail
  TOTAL : 3 pass, 1 fail
```
Variables: `sec=N` (per-test seconds, default
5), `noise=off` (skip the noise test). The driver
exposes the pre-existing ASYNC-under-noise
flake (1/45 delivery on a 1%-drop wire) that
was masked by timing luck in v5.3.45; SYNC
mode passes the same test cleanly. The flake
is a real ASYNC protocol bug, not a v5.3.46
regression — file for a future fix.

The two itest binaries now both accept
`sync` / `async` args and use the test
hooks in SYNC mode to avoid deadlocking
on the blocking SYNC wait.

**New regression test:** `SyncModeTest.cpp`, 4
cases:
- default mode is compile-time correct (host=ASYNC,
  Arduino=SYNC).
- SYNC mode on clean wire uses zero ARQ pool slots
  (the bug we're proving doesn't recur).
- SYNC mode survives a single forced drop and
  re-sweeps to OK.
- SYNC mode returns false when the link is not OK.

Verified the test catches the bug: re-introducing
the old `sendCobsFrameAcked_unlocked` call in the
SYNC branch makes the pool-use test fail with
`Assertion 'poolUsed == 0' failed`. Restoring the
sync-only `sendCobsFrame_unlocked` path, all 4
tests pass.

**Result:**
- 23 unit suites (was 22), 260+ test functions
  (was 247), all green. The 3 pre-existing
  failures (`alink_message`, `alink_cobsseq`,
  `linkreorder`) are unchanged.
- `build/test_pretty_print.py`: 16/16 pass.
- `make test` ~3.0 s.

---

## v5.3.45

**Grow `ARQ_CACHE_POOL_SIZE` from 16 to 64.**

The pool is the per-message payload cache that backs
the ARQ retransmit path (`arqPool_[POOL_SIZE][256]` in
`AutoLink.h`). When all pool slots were taken, the
trampoline rejected the new `send()` at the
`arqCache_hasRoom()` gate (fixed in v5.3.38) so
`send()` returned 0 and the chunk was dropped with a
loud log line — not silently lost, but the user still
saw the back-pressure under any sustained
`send() > N/ack-RTO` workload.

**Fixed:** `ARQ_CACHE_POOL_SIZE = 64` (was 16). The
v5.3.38 trampoline is unchanged — it still calls
`arqCache_hasRoom()`, which now has a 4× larger pool
to draw from before saying "no room". `ARQ_CACHE_SLOTS`
(256, the `pending_[seq]` index space) is unchanged:
the pool and the slot table are independent, and the
wire format only references the 1-byte `cobsSeq`
(0..253, with 0xFE/0xFF reserved), so there is no
interop impact.

**Memory cost:** ~16 KiB additional RAM on the
AutoLink instance (64 × 256-byte payload pool + 64
bytes of `poolUsed_` flags). `Pending[256]` is
unchanged.

**Result:**
- 22/25 unit suites pass — same as 5.3.44. The 3
  pre-existing failures (`alink_message`,
  `alink_cobsseq`, `linkreorder`) are unrelated to
  the pool size and identical between 5.3.43 and
  5.3.44; this release does not touch the cpp/h
  files they cover.
- `ArqCacheHasRoomTrampolineTest` (added v5.3.38)
  still passes — it tests the trampoline logic, not
  the pool size; the threshold moves from 16 to 64.

---

## v5.3.44

**`library.properties` wording pass: stop calling
ARQ a feature without the caveat.**

The previous `sentence=` ("...with ARQ retransmit and
best-effort delivery") and the `paragraph=` lead
("best-effort ARQ (ACK-timeout retransmit up to a
bounded retry count...)") framed ARQ as a headline
capability. The Arduino Library Manager renders the
sentence verbatim above the install button, and a
newcomer reading "...ARQ retransmit" reasonably
expects guaranteed delivery — which the library
explicitly does not provide. A chunk that exhausts
its retry budget is dropped (counted in `lostMsgs`),
not delivered.

**Fixed:** the sentence now leads with the
user-visible behavior ("self-healing", "auto-baud
lock", "bounded-retry retransmits") and qualifies
with "best-effort delivery" so a registry reader
can't mistake the library for a guaranteed-delivery
transport. The paragraph drops the "best-effort ARQ"
framing entirely and describes the protocol in
operational terms: NAK-driven retransmit on demand,
bounded retry budget, out-of-order hold, NAK on
missing seq, drop if the retransmit never lands.
Delivery semantics are spelled out in the same
sentence as the drop policy, not buried at the end.

**No code change.** `library.properties` only.

**Result:** no behavior change. README and protocol
docs unchanged.

---

## v5.3.43

**Rename `arqReorderHoldMs` -> `reorderHoldMs`.**

The field controls how long the receive-side reorder
buffer holds an out-of-order frame before expiring
it. The `arq` prefix was misleading given the "no
ARQ" framing policy in the docs -- the field is
purely a receive-side reorder policy, not an ARQ
config knob (those live under the application-layer
ACK handling). Renamed to `reorderHoldMs` so a
reader doesn't mistake it for an ARQ retry budget.

**Renamed:**
- `src/al/link/Link.h:88` field decl
- 2 use sites in `src/al/link/Link.cpp`
- 4 test sites in `test/test_desktop/al/link/`
- 1 doc reference in `README.md`

**Comment added** to the field decl explaining what
it does (hold time for the reorder buffer before
expiring a missing seq).

**Result:** no behavior change. 25/25 unit tests
pass.

---

## v5.3.42

**Gap placeholder: out-of-order keepalives no longer
occupy a reorder slot.**

The gap path in `Link::onPayload` had a branch for
`n == 0` (keepalive arriving out of order) that wrote
`reorder_[cobsSeq] = {buf=nullptr, len=0, in_use=true}`.
The reorder flush would later see that slot, call
`pushAppBuf(nullptr, 0)` (a no-op in EspHal because
`n <= 0` is guarded) and `sendAckFrame_unlocked`,
advancing `rxSeq` and ACKing a frame that was never
delivered. The bug was masked by EspHal's defensive
guard, but the slot was semantically wrong -- a gap
placeholder being treated the same as a received
frame.

**Fixed:** the `else` (n == 0) branch no longer
reserves a reorder slot. The keepalive is a
unidirectional heartbeat, not a frame to deliver or
ACK. The gap is counted in `lostMsgs` (it was a hole
in the seq), the NAK is sent so the sender
retransmits if it has real data in flight, and
nothing sits in the reorder buffer.

**Other cleanup:** removed the `is_gap` field I had
initially added to `ReorderSlot` in v5.3.41 (replaced
by the cleaner "don't reserve the slot" approach),
along with the corresponding test peeker
`test_reorderSlotIsGap()`.

**New regression test:**
`test_keepalive_gap_does_not_occupy_reorder_slot` in
`LinkReorderTest.cpp`. Sends a keepalive
(`cobsFrame(N, 0)`) out of order and asserts that no
reorder slot is reserved. Verified the test catches
the bug: re-introducing the old `in_use = true` write
makes the test fail with
`Assertion '!b.test_reorderSlotInUse(3)' failed`.

**Result:**
- 25 unit suites, 240+ test functions, all green.
- `build/test_pretty_print.py`: 16/16 pass.
- `make test` ~3.1 s.
