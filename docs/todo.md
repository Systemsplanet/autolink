# Open / Quarantined Items

Items below are tracked here so the release gate can stay green
while the underlying work is in progress. Each entry lists the
test, the failure mode, and the next step.

## Quarantine: empty

There are no quarantined unit tests. `make test` is 128/128.

The three long-standing failures — `run_test_fill_byte_roundtrip`,
`run_test_base_seq_tracking`, `run_test_linkrx_split_ctrl` —
were carried from the v6.1.51 baseline through v6.1.63 with a
different suspected cause written down for each. All three had
one cause, fixed in v6.1.64: the post-lock settle window dropped
CRC-valid frames, so anything sent in the first
`AUTOLINK_WIRE_SETTLE_MS` after a lock was discarded by the
receiver without an ACK, a NAK, or a counter. See
`docs/Version.md` v6.1.64.

## Open

- **Source-grep anti-pattern sweep.** Test files outside
  `meta/` still use `ifstream` source scanning. They are
  brittle against refactors — a pin that greps for a guard's
  position asserts the shape of the code rather than its
  behaviour, and can stay green while the behaviour is wrong.
  `SettleGateTest` pins d and e were exactly that shape and
  were converted to behavioural pins in v6.1.64; the rest
  should follow. Their CWD dependence was fixed in v6.1.64
  (`test/common/TestPaths.h`), so they now fail for the
  right reasons only — but they still pin shape, not
  behaviour. v6.1.66 added more of this shape
  (`EspHalBreakFlushGuardTest`) out of necessity — `EspHal.h`/
  `EspHalUartEvent.h` are ESP32-only and cannot compile or run
  in this sandbox, so a source-level pin was the only option.

- **File-size cap.** `LinkRx.cpp`, `Link.h`, `LinkApi.cpp`,
  `Ping.h`, `LinkCore.cpp` still exceed the 15 KB guidance.
  `LinkTimersOk.cpp` was split in v6.1.66 (GBN sweep-retx half
  moved to `timers/gbn/LinkTimersGbn.cpp`, ~25 KB -> ~16 KB);
  v6.1.67's baud-upgrade trigger grew it further to ~17 KB —
  still fractionally over, next split candidate if it grows
  further. Split the rest by concern — the BREAK suspicion
  state out of `Link.h`, the rate-admission block out of
  `LinkApi.cpp`.

- **Package file-count cap: `test/test_desktop/al/pingpong/`
  is at 8 `.cpp` files**, over the 7-file guidance — predates
  v6.1.67 (already 8 in the v6.1.66 zip), noticed only because
  v6.1.67 needed to place new pingpong-adjacent tests and used
  fresh sibling subdirectories (`gap/`, and
  `test/itest/test_desktop/al/pingpong/`) rather than adding to
  it. `test/test_desktop/al/link/sweep/guard/` (11 files),
  `test/test_desktop/al/link/health/` (19 files),
  `src/al/util/` (8 files), and
  `test/test_desktop/al/link/message/` (8 files) are the same
  shape — the latter two found during a v6.1.67 verification
  pass and also already 8 in the v6.1.66 zip. None resolved this
  pass.

- **`run_loopback_losssweep`'s prior 1% flood-loss floor
  failure (2 disconnects) no longer reproduces** — `disc=0` at
  all three floors (0.1%/1%/5%), confirmed across 2 runs on
  v6.1.66. Not root-caused to a specific v6.1.66 change (no
  revert-and-rerun was done this pass, unlike the v6.1.65 note
  this replaces); the GBN storm-stuck / backoff-clamp fixes in
  this release are the most plausible cause given they touch
  pool-headroom-adjacent retx timing directly, but that's an
  inference, not a confirmed root cause. Leave this line here
  until a revert-and-rerun confirms which change (if any)
  fixed it, or it resurfaces.

