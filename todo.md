# todo.md — v6.0.40

Release history: `docs/Version.md`. Order = priority. Nothing here is done.

## Open

1. **Restore the device-path verification loop, then bench-validate the
   6.0.33–6.0.40 backlog.** `verify_build.sh` last cleared the build at
   6.0.32; source-touching releases since shipped cross-compile-unverified
   and 6.0.37 exists only because the host gate misses Arduino-only
   breakage by construction. Run `./build/verify_build.sh`
   (esp32:esp32@3.3.5), then on the FireBeetle pair: re-lock cadence
   symmetry (slave free-runs P1→P2 ~300 ms for ~6 s post-disc while master
   idles — reproduce first, dwells are pinned by
   `PongP1GuardOutlastsMasterP2Test` et al.), sweep walk-down
   (512000 → 115200 → … lock on first PONG_ACK), ASYNC-flood CRC/desync
   wedge + the unified health-monitor drop/resweep end-to-end
   (`LinkHealth.h` via `LinkTimers.cpp`), heap-cap boot log, and both OTA
   uploads (`curl --data-binary @fw.bin /ota/fw`; `zip -0` GUI →
   `/ota/gui`, `GET /` serves from LittleFS).

2. **SYNC: resync on the failure, not the watchdog.** A lost mid-message
   ACK wedges the length-prefixed framer; recovery waits up to
   `idleTimeoutMs` (10 s default). Issue BREAK immediately on a
   mid-message `waitForAck` timeout so both framers realign in one
   round-trip. Regression: extend `LinkSyncStallWatchdogTest` — wedge
   drops within one RTO, not one idle window; toggle-off → red.

3. **ASYNC: raise the delivery floor under loss.** ~74 % delivery at 1 %
   frame loss (post-6.0.34) is not bulletproof — losses trace to
   `reorderHoldMs` (1500, `AutoLinkConfig.h:258`) expiring before retx
   closes the gap, and pool exhaustion under sustained flood. Derive
   reorder hold from measured RTO × `maxRetx` instead of a fixed 1500 ms;
   add a loss-sweep itest (0.1 / 1 / 5 %) with pinned per-rate floors
   (target ≥ 99 % at 1 %); pin pool headroom under flood.

## Verify
`cd test && make test && make itest && ./build/verify_build.sh`
(70/70 unit, 4/4 itest. Cross-compile MANDATORY before flashing — OTA
`esp_ota_*`/LittleFS paths compile only in the Arduino build. Item 1
needs the FireBeetle pair.)
