# AutoLink ESP32
A lightweight, self-healing UART protocol layer.

## Architecture
- `ILink`: Abstract interface preventing hardware coupling.
- `ALink`: The state machine handling error counting, breaking, sweeping, and locking.
- `EspHal`: The specific ESP32 Arduino hardware hook.

## How to layer
Instead of writing directly to `Serial`, use `link.tx()` and `link.rx()`. If your custom framing or Modbus protocol fails a CRC, simply call `link.err()`. `ALink` will manage the rest automatically!

## Tests
Run `make` to execute the C++ unit tests natively on your PC without an ESP32.

## Notes
Simple API & Layerable: 
You do not have to rewrite your existing protocols. Simply wrap your communication via link.rx() and link.tx(). Whenever your higher-level protocol (like Modbus or a custom packet framer) detects a bad CRC, just call link.err(). ALink intercepts the stream to fix the connection and returns to normal automatically.

​Short Variable Names: I used terse structural naming (ALink for AutoLink, st for state, spd for speed, isM for master role, hw for hardware).

​Light as Possible: Zero dynamic allocation (malloc/new). Zero std::vector or String objects. It is purely state-machine driven and memory-safe.

​Easily Testable (Decoupled): I created an ILink.h Hardware Abstraction Layer. ALink knows nothing about ESP32 hardware, which means you can compile and run the logic on your PC natively.

​Tests Included: You will find a test.cpp and a Makefile inside the zip. Running make will execute a full simulated workflow proving the master detects errors, sends a break, cycles through baud speeds, calculates the optimum, and locks it in.
