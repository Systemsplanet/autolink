# AutoLink ESP32 (Interrupt & Event Driven)
A lightweight, self-healing UART protocol layer.

## What's New: Fully Event-Driven
This library has been upgraded from polling to run entirely on hardware interrupts and FreeRTOS tasks.
- **No Polling (`tick()`) required.** Your main loop is 100% free.
- **No physical GPIO manipulation.** It uses standard ESP-IDF UART registers natively to trigger `uart_write_bytes_with_break` and detect a hardware `UART_BREAK` event.
- **Ring Buffer built-in:** Asynchronously collects data in the background.

## Architecture
- `RingBuffer`: Safely queues incoming data from the ISR/Task avoiding race conditions.
- `ILink`: Hardware Abstraction Layer allowing PC unit testing.
- `ALink`: The pure state machine triggered by events.
- `EspHal`: The ESP32 driver utilizing `uart_driver_install` event queues and FreeRTOS Timers.

## How to Use
Instead of touching `HardwareSerial` (e.g., `Serial2`), use the library:
```cpp
#include "EspHal.h"
#include "ALink.h"

EspHal* hal;
ALink* link;

void setup() {
    // Connect to UART2, RX Pin 16, TX Pin 17
    hal = new EspHal(UART_NUM_2, 16, 17);
    link = new ALink(hal, true); // true = Master
}

void loop() {
    // Read data asynchronously without blocking
    if (link->available()) {
        uint8_t buf[64];
        int len = link->read(buf, 64);
        // Process your upper-level Modbus/custom packet here...
    }

    // If your packet's CRC fails, simply flag it:
    // link->err(); 
}
```

## Testing natively on PC
Run `make` to compile and run the entire suite verifying Master/Slave negotiation completely decoupled from ESP32 hardware!

autolink
/README.md
Go to file
t
T
Systemsplanet
Systemsplanet
Update README.md
9452d9e
 · 
42 minutes ago
25 lines (17 loc) · 1.63 KB

Preview

Code

Blame
Older
Newer
Systemsplanet
48 minutes ago

gitupload: README.md
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
42 minutes ago

Update README.md
## Notes
Simple API & Layerable: 
You do not have to rewrite your existing protocols. Simply wrap your communication via link.rx() and link.tx(). Whenever your higher-level protocol (like Modbus or a custom packet framer) detects a bad CRC, just call link.err(). ALink intercepts the stream to fix the connection and returns to normal automatically.
​Short Variable Names: I used terse structural naming (ALink for AutoLink, st for state, spd for speed, isM for master role, hw for hardware).
​Light as Possible: Zero dynamic allocation (malloc/new). Zero std::vector or String objects. It is purely state-machine driven and memory-safe.
​Easily Testable (Decoupled): I created an ILink.h Hardware Abstraction Layer. ALink knows nothing about ESP32 hardware, which means you can compile and run the logic on your PC natively.
​Tests Included: You will find a test.cpp and a Makefile inside the zip. Running make will execute a full simulated workflow proving the master detects errors, sends a break, cycles through baud speeds, calculates the optimum, and locks it in.


Fully Interrupt/Event Driven: The tick() function is completely gone. The main loop (app) does not need to poll the library anymore.
​Native Hardware Break (No GPIO pins): I switched the ESP32 hardware layer (EspHal.h) to use native ESP-IDF drivers (driver/uart.h) rather than Arduino's HardwareSerial.
​To send a break, it now uses uart_write_bytes_with_break() which utilizes the silicon to hold the line low without touching a GPIO.
​It runs a background FreeRTOS task blocking on the uart_queue. If the silicon detects a 15-bit low signal, it fires a UART_BREAK hardware event, instantly waking the task and triggering the Auto-Baud sweep.
​RingBuffer Included: Since data now arrives in the background via tasks, I added RingBuffer.h to safely buffer data until your application is ready to read it (link->available()).
​Hardware Timer Sweeps: The 50ms sweep delays are no longer blocking or polled. They are driven by a FreeRTOS software timer (xTimerCreate) that steps the state machine through the baud rates in the background.
​PC Testability Preserved: Despite switching to FreeRTOS concepts for the ESP32, the core logic (ALink.cpp) remains 100% decoupled. The test.cpp simply acts as the "hardware" and simulates firing these asynchronous events to prove the state machine locks the new speed successfully.
