# todo.md

Release history: `docs/Version.md`. Closed/archived items live in
`docs/Version.md` and are not re-listed here.

## Open

1. **Cross-compile verification unrun in-sandbox.** `bash build/verify_build.sh`
   and `bash build/check_arduino_iface.sh` cannot run here — no network egress,
   so `build/arduino-cli-cmd.sh` can't install `arduino-cli` + `esp32:esp32`.
   Both gates PASSED in the last networked session (see `docs/Version.md`
   v6.1.14: `.bin` 1074147 B / 81%, all 5 iface phases). Re-run in a
   network-capable environment before any release that touches
   `AutoLinkWeb.cpp` or public-header API surface; host tests skip that TU.

2. **Bench validation of the v6.1.14 ASYNC inter-chunk gap** (`cfg.asyncChunkGapMs`).
   Host pin (AsyncChunkGapTest) confirms the library emits the 1 ms gap between
   multi-chunk ASYNC frames. Still wants a physical FireBeetle pair run at
   512000 baud confirming the prior `errs` climb (0 → 5) and Pong rx-rate
   collapse no longer reproduce under RANDOM fill with a raised RANDOM ceiling
   (above the v6.1.13 window/2 cap).

3. **Bench validation of the v6.1.14 GBN whole-window backoff** (`decideGbnBackoff`).
   Simulation pins (GbnBackoffTest Pins 4–5) confirm exponential backoff under a
   stuck base and an honest maxRetx drop under the `8*syncAckTimeoutMs` cap.
   On-wire confirmation under real UART noise still pending: a recoverable-
   congestion scenario (base clears after N slow ACKs → link stays up) vs a
   peer-gone scenario (still drops at maxRetx). Bench sub-items carried from
   v6.1.14: re-lock cadence, sweep walk-down, ASYNC flood.

## Verify (last networked session)
`cd test && make test && make itest` — 80/80 unit + 6/6
itest. `bash build/verify_build.sh` PASS (esp32:esp32@3.3.5 /
firebeetle32, .bin 1074147 B / 81% program).
`bash build/check_arduino_iface.sh` PASS, all 5 phases.
