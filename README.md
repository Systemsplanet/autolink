# AutoLink ESP32
A production-grade, self-healing UART protocol layer with comprehensive test coverage.

## The `AutoLink` Wrapper (Modern API)
We've significantly simplified the API. You no longer need to manually wire the Hardware Abstraction Layer (`EspHal`) into the Core State Machine (`ALink`). 

Instead, just include `AutoLink.h` and use the single `AutoLink` class. It manages all the FreeRTOS background tasks, queues, and hardware interrupts automatically!

### How to Use on ESP32
```cpp
#include "AutoLink.h"

// Access the library namespace
using namespace autolink;

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
        
        // Process payload and acknowledge success to reset the error counter!
        if (payload_crc_is_good) {
            link->clearErr(); 
        } else {
            link->err();
        }
    }
}
```

## Namespace & Architecture
To prevent collisions, all classes (`AutoLink`, `ALink`, `EspHal`, `ILink`) and state enums (`State::OK`, `State::SWP`, `State::LCK`) are enclosed within the `autolink` namespace. 
