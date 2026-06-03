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
