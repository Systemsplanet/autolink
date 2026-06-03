# AutoLink ESP32
A production-grade, self-healing UART protocol layer.

## The `AutoLink` Wrapper (Modern API)
We've significantly simplified the API. You no longer need to manually wire the Hardware Abstraction Layer (`EspHal`) into the Core State Machine (`ALink`). 

Instead, just include `AutoLink.h` and use the single `AutoLink` class. It manages all the FreeRTOS background tasks, queues, and hardware interrupts automatically!

### How to Use on ESP32
```cpp
#include "AutoLink.h"

// Initialize AutoLink on UART2, RX Pin 16, TX Pin 17, as Master (true)
std::unique_ptr<AutoLink> link;

void setup() {
    // You can optionally pass custom baud rates, error thresholds, and delays!
    link.reset(new AutoLink(UART_NUM_2, 16, 17, true));
}

void loop() {
    // Read data asynchronously
    if (link->available()) {
        uint8_t buf[64];
        int len = link->read(buf, 64);
        
        // Process payload...
        // if (payload_crc_is_bad) link->err(); 
    }

    // Write data normally
    // uint8_t msg[] = {0x01, 0x02};
    // link->write(msg, 2);
}
```

## Architecture Under the Hood
- `AutoLink`: Clean interface utilizing C++11 `<memory>` to prevent leaks.
- `EspHal`: The ESP32 driver utilizing `uart_driver_install` and FreeRTOS Timers. Protected by hardware initialization checks (`ESP_ERROR_CHECK`). 
- `ALink`: The pure state machine triggered by events. Protected internally against multi-task race conditions by abstract hardware locks.
- `StreamBuffer`: Safely queues incoming data from the background ISR/Task using the native `freertos/stream_buffer.h`.

## PC Unit Testing
Run `make` to compile and run the entire suite. The core state machine (`ALink.cpp`) is tested natively on your PC using standard C++ libraries (`<queue>` and `<mutex>`), bypassing the ESP32 wrapper to prove the logic is flawless.
