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
