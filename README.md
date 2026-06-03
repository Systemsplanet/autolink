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
