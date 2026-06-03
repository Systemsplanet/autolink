# AutoLink ESP32
A lightweight, self-healing UART protocol layer.

## The `AutoLink` Wrapper (New API)
We've significantly simplified the API. You no longer need to manually wire the Hardware Abstraction Layer (`EspHal`) into the Core State Machine (`ALink`). 

Instead, just include `AutoLink.h` and use the single `AutoLink` class. It manages all the FreeRTOS background tasks, queues, and hardware interrupts automatically!

### How to Use on ESP32
```cpp
#include "AutoLink.h"

// Initialize AutoLink on UART2, RX Pin 16, TX Pin 17, as Master (true)
AutoLink* link;

void setup() {
    link = new AutoLink(UART_NUM_2, 16, 17, true);
}

void loop() {
    // 1. Read data asynchronously (No polling or blocking!)
    if (link->available()) {
        uint8_t buf[64];
        int len = link->read(buf, 64);
        
        // Process your payload here...
        // if (payload_crc_is_bad) {
        //     link->err(); 
        // }
    }

    // 2. Write data normally
    // uint8_t msg[] = {0x01, 0x02};
    // link->write(msg, 2);
}
```

## Architecture Under the Hood
- `AutoLink`: Clean interface. 
- `EspHal`: The ESP32 driver utilizing `uart_driver_install` and FreeRTOS Timers.
- `ALink`: The pure state machine triggered by events.
- `RingBuffer`: Safely queues incoming data from the background ISR/Task.

## PC Unit Testing
Run `make` to compile and run the entire suite. The core state machine (`ALink.cpp`) is tested natively on your PC, bypassing the ESP32 wrapper to prove the logic is flawless.

## Notes & Design Principles

* **Simple API & Layerable**: You do not have to rewrite your existing protocols. Simply wrap your communication via `link->read()` and `link->write()`. Whenever your higher-level protocol (like Modbus or a custom packet framer) detects a bad CRC, just call `link->err()`. The library intercepts the stream to fix the connection and returns to normal automatically.
* **Short Variable Names**: Used terse structural naming (`ALink` for AutoLink, `state` for state, `spd` for speed, `isM` for master role, `hw` for hardware).
* **Lightweight Core**: Uses zero `std::vector` or `String` objects. The core logic is purely state-machine driven and memory-safe (dynamic allocation is kept minimally to RTOS queue structures).
* **Easily Testable (Decoupled)**: An `ILink.h` Hardware Abstraction Layer ensures `ALink` knows nothing about ESP32 hardware, allowing you to compile and run the logic natively on your PC.
* **Tests Included**: A `test.cpp` and `Makefile` are included. Running `make` executes a full simulated workflow proving the master detects errors, sends a break, cycles through baud speeds, calculates the optimum, and locks it in. Includes a dedicated test for `RingBuffer`.
* **Fully Interrupt/Event Driven**: There is no polling `tick()` function. The main loop is completely free.
* **Native Hardware Break (No GPIO pins)**: The ESP32 hardware layer (`EspHal.h`) uses native ESP-IDF drivers (`driver/uart.h`) instead of `HardwareSerial`. To send a break, it natively uses `uart_write_bytes_with_break()` to hold the line low without touching a GPIO.
* **Hardware Event Queues**: A background FreeRTOS task blocks on the `uart_queue`. If the silicon detects a 15-bit low signal, it fires a `UART_BREAK` hardware event, instantly waking the task and triggering the Auto-Baud sweep.
* **RingBuffer Included**: Data arriving in the background task is safely buffered in `RingBuffer.h` until your application is ready to read it via `link->available()`.
* **Hardware Timer Sweeps**: The 50ms sweep delays are non-blocking. They are driven by a FreeRTOS software timer (`xTimerCreate`) that steps the state machine through baud rates in the background.
* **PC Testability Preserved**: Despite using FreeRTOS for the ESP32, the core logic (`ALink.cpp`) remains 100% decoupled. The `test.cpp` simulates asynchronous events to prove the state machine locks the new speed successfully.
