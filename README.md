# AutoLink ESP32
A production-grade, self-healing UART protocol layer with comprehensive test coverage.

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
    
    // Check if tasks/memory allocated properly
    if (!link->isHealthy()) {
        // Handle failure
    }
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

## Advanced Testability & Coverage
The `ALink` core has been deeply refactored to prioritize testability and C++11 best practices.
- **Introspection Methods:** New state accessors (`getErrCount()`, `getCurrentSpdIndex()`) allow testing frameworks to verify internal state logic without breaking encapsulation.
- **Dependency Injection:** The core state machine expects an `ILink` hardware reference at instantiation, eliminating null-pointer risks entirely.
- **Enhanced Mocking:** The `MockHal` testing hardware layer now employs Spies (`sendBreakCalls`, `spdHistory`) to trace exactly how the core logic manipulates the hardware under varying scenarios.
- **Granular Test Suite:** `test.cpp` features a structured test runner handling isolated test cases for IO operations, baud sweep logic, and master/slave locking sequences.

## PC Unit Testing
Run `make` to compile and execute the test runner natively on your PC, bypassing the ESP32 wrapper to prove the core protocol math is flawless.
