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

​PC Testability Preserved: Despite switching to FreeRTOS concepts for the ESP32, the core logic (ALink.cpp) remains 100% decoupled. The test.cpp simply acts as the "hardware" and simulates firing these asynchronous events to prove the state 



eof
