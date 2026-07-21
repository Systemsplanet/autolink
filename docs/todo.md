# Open / Quarantined Items

Items below are tracked here so the release gate can stay green
while the underlying work is in progress. Each entry lists the
test, the failure mode, and the next step.

## Quarantine: empty

There are no quarantined unit tests. `make test` is 119/119.

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
  behaviour.

- **File-size cap.** Six files exceed the 15 KB guidance:
  `LinkRx.cpp`, `Link.h`, `LinkApi.cpp`, `LinkTimersOk.cpp`,
  `Ping.h`, `LinkCore.cpp`. Split by concern — the BREAK
  suspicion state out of `Link.h`, the rate-admission block
  out of `LinkApi.cpp`.

- **Hardware validation of the field-wedge batch.** The
  v6.1.60/61 fixes (hot-path `isPending` log removed,
  event-driven SYNC wait, SYNC mode forwarded to the HAL,
  CPU-stall re-arm in the stuck monitor, baud-derived
  peer-stalled watchdog) and the v6.1.64 settle-window fix
  have host pins but have not been re-run on the FireBeetle
  pair at 512000. That re-run is the remaining acceptance
  step.
