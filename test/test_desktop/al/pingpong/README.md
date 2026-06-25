# Why no host tests for pingpong?

The `src/al/pingpong/` package — `Ping`, `Pong`, `PingPongBase`,
`PingPongMain` — is the plug-and-play ping/pong sketch layer
that sits on top of `Link`. Every header is `#ifdef ARDUINO`
because it references `uart_port_t`, GPIO pins, FreeRTOS task
setup, and the ESP-IDF Arduino core.

None of that exists on host. No host test can construct a
`Ping` or `Pong`, exercise its `setup()`/`loop()`, or assert
on its behaviour without first porting the entire ESP-IDF
dependency tree to the desktop build. That's a multi-week port
with no defect-finding payoff.

## What covers this code on host

The underlying `Link` protocol that PingPong rides on is fully
covered by the host unit tests in `test/test_desktop/al/link/`
and by the host integration tests in `test/itest/test_desktop/`:

- **Unit** (`test/test_desktop/al/link/`) — `LinkArqTest.cpp`,
  `LinkBaudSweepTest.cpp`, `LinkCobsSeqTest.cpp`,
  `LinkDecisionTest.cpp`, `LinkErrorTest.cpp`,
  `LinkFrameRxTest.cpp`, `LinkIOTest.cpp`, `LinkMessageTest.cpp`,
  `LinkReorderTest.cpp`, `LinkV53Test.cpp`, `LinkV531Test.cpp`,
  `LinkV531NeverLeaveP1Test.cpp`. Pure decision logic, byte I/O,
  message API, ARQ, baud sweep, gap/stale, reorder buffer.
- **Integration** (`test/itest/test_desktop/al/link/`) —
  `loopback_test.cpp` (two-node end-to-end) and
  `loopback_noise_test.cpp` (stochastic frame drops +
  intermittent BREAKs).

These tests use the `MockHal` fixture (`test/common/MockHal.h`)
which implements the `IHal` seam that `EspHal` implements on the
real ESP32. From PingPong's point of view, `MockHal` is
identical to `EspHal` for everything except GPIO/UART.

## When a real ESP32 is in the loop

The on-hardware self-loopback smoke in
`test/itest/test_embedded/test_embedded.ino` exercises the full
`AutoLink` facade end-to-end. PingPong is the canonical user of
that facade, so this test catches any regression in how the
pingpong headers wire up to the rest of the stack on actual
hardware.

## If you want to add a host test here

Two paths, neither recommended:

1. **Stub the ESP-IDF layer for host.** Replace `uart_port_t`
   with an integer, fake `gpio_set_direction`, etc. The stub
   would need ~200 lines just to get `Ping` to compile, and
   would diverge from the real Arduino behaviour within weeks.
2. **Compile-only smoke.** `#include` each pingpong header
   inside `#ifdef ARDUINO` and assert it parses. Catches a
   typo in the headers but tells you nothing about behaviour.

If the underlying link ever moves off `#ifdef ARDUINO` (e.g. a
POSIX UART backend), this package gets host tests
automatically. Until then, AGENTS.md rule 19 documents this as
the one allowed exception.
