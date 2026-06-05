# AutoLink ESP32
A robust, auto-negotiating UART layer for the ESP32.

## What's New
- **Performance Enhancements**: Removed byte-by-byte mutex locking and transitioned `StreamBuffer` calls to high-speed block transfers. 
- **Stability Improvements**: Moved hardware and task initialization from the constructor into `begin()` to prevent early kernel panics upon global initialization.
- **Safety Fixes**: Removed heavy stack allocations inside loops, safely defer FreeRTOS timer events, and integrated native `ESP_LOG` functions for thread-safe logging.
- **Comprehensive Testing**: Full throughput benchmarking and message chunk testing scaling up to 16,000 bytes.

## Basic Usage

```cpp
#include "AutoLink.h"

using namespace autolink;

AutoLinkConfig cfg;
cfg.reliableMode = true;
cfg.streamBufferSize = 2048; // Configurable sizes!

// Instantiate globally (safe!)
AutoLink link(UART_NUM_1, 16, 17, true, cfg);

void setup() {
    Serial.begin(115200);
    link.begin(); // Initialize FreeRTOS tasks and UART queues here
}

void loop() {
    // Standard Stream methods
    if (link.available()) {
        int b = link.read();
        Serial.printf("Got: %02X\n", b);
    }
}
```
