# test/test_embedded/

This directory is **reserved for embedded-target unit tests**
that run on a host (no ESP32 toolchain required).

It is currently empty. The reason: the only library code that
cannot be exercised by the host unit suite under
`test/test_desktop/` is the Arduino-only glue — `AutoLinkWeb.cpp`
(WiFi, NVS, httpd, `esp_timer`), the `Ping`/`Pong` classes, and
the `#ifdef ARDUINO` paths in `Link.cpp`. Every one of those
paths depends on the real ESP-IDF / Arduino toolchain and / or
the device itself, so a host-side unit test cannot exercise
them without porting the full ESP-IDF dependency tree — a
multi-week effort with no defect-finding payoff.

This mirrors the documented exception in AGENTS.md rule 18 for
`test/test_desktop/al/pingpong/`: the `Ping` / `Pong` /
`PingPongBase` / `PingPongMain` headers are all `#ifdef ARDUINO`
and reference `uart_port_t`, GPIO, and FreeRTOS task setup. No
host test can construct a `Ping` / `Pong` without that port.

**Where the embedded-target coverage actually lives:**

- `test/itest/test_embedded/test_embedded.ino` — the actual
  Arduino sketch that cross-compiles against the real ESP32
  toolchain via `bash build/arduino-cli-cmd.sh compile ...`.
  This is the gate that catches "compiles on host, breaks on
  device" failures (rule 13: green host tests are necessary
  but not sufficient).
- `build/verify_build/verify_build.ino` — a
  smoke-compile sketch that mirrors the README's
  Pong.ino shape (file-scope `PingPong upp(...)` +
  minimal `setup()` / `loop()`). Run via
  `./build/verify_build.sh` per AGENTS.md rule 4. The
  exhaustive facade-API exercise that used to live
  here is now covered by `CompileCheckTest` (source-
  level grep) and the host unit suite; the verify
  build focuses on the user-facing entry point so
  ArduinoDroid ctor errors in the `PingPong` /
  `Ping` / `Pong` / `PingPongBase` chain are caught
  on first cross-compile.

If a future change moves an embedded-only code path off
`#ifdef ARDUINO` (or stubs out the Arduino-only dependencies
enough for a host compile), drop the test under this directory
and link it from `test/Makefile`'s `make test` target.
