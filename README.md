
# ​🚀 AutoLink ESP32
​A production-grade, self-healing UART protocol layer for ESP32.

​Ever had a UART connection drop because a motor spun up nearby? Have you ever hardcoded a baud rate, only to realize the other microcontroller reset and desynced? Raw UART is notoriously fragile in the real world.
​AutoLink fixes this. It sits on top of the standard ESP-IDF UART drivers and transforms your serial connection into a robust, auto-negotiating, self-healing lifeline. If the line gets noisy, AutoLink drops down to a safer baud rate. If a wire gets bumped, it automatically sweeps and locks back onto the connection.
​It manages all the FreeRTOS background tasks, queues, and hardware interrupts automatically. You just read and write data.

# ​⚡ Quick Start
​Drop the AutoLink files into your project, include the header, and let the library do the heavy lifting.

''' cpp
#include "AutoLink.h"

using namespace autolink;

// Create a Master node on UART2 (RX=Pin 16, TX=Pin 17)
AutoLink masterLink(UART_NUM_2, 16, 17, true);

void setup() {
    Serial.begin(115200);
    
    if (!masterLink.isHealthy()) {
        Serial.println("Failed to initialize UART hardware!");
    }
}

void loop() {
    // 1. Write data
    uint8_t msg[] = "Hello Slave!";
    masterLink.write(msg, sizeof(msg));

    // 2. Read data asynchronously
    if (masterLink.available()) {
        uint8_t buffer[64];
        int len = masterLink.read(buffer, 64);
        Serial.printf("Received %d bytes!\n", len);
    }
    
    delay(10);
}
'''




# 🛠️ Simple Usage: Master & Slave
​AutoLink requires one device to act as the Master (initiates the baud negotiation) and one to act as the Slave (listens and responds to the sweep).

​## The Slave Node
​Setting up a listening device is just as easy as setting up the master. Just pass false for the master flag!


'''cpp
#include "AutoLink.h"
using namespace autolink;

// Initialize as Slave (false)
AutoLink slaveLink(UART_NUM_2, 16, 17, false);

void setup() {
    // Wait for the Master to find us and lock on!
    while(slaveLink.getState() != State::OK) {
        delay(100); 
    }
}

void loop() {
    if (slaveLink.available()) {
        uint8_t buf[128];
        int len = slaveLink.read(buf, 128);
        
        // Echo the data back
        slaveLink.write(buf, len);
    }
}

'''


#🧠 Advanced Usage: The Power User API
​AutoLink isn't just a wrapper; it's a dynamic state machine. For mission-critical applications, you want fine-grained control over exactly how the system reacts to noise, which baud rates it's allowed to use, and how it handles payload errors.

​Here is an advanced example showing how to utilize the entire API, including custom configurations and manual error tracking.

'''cpp
#include "AutoLink.h"
using namespace autolink;

std::unique_ptr<AutoLink> link;

void setup() {
    Serial.begin(115200);

    // Custom allowed baud rates (it will test these during a sweep)
    std::vector<uint32_t> myBauds = {9600, 38400, 115200, 1000000};
    
    // Advanced Configuration:
    // 1. UART Number
    // 2. RX Pin
    // 3. TX Pin
    // 4. Is Master? (true)
    // 5. Allowed Bauds array
    // 6. Error Threshold (Drop connection after 10 errors)
    // 7. Sweep Delay (Wait 100ms between baud rate tests)
    link.reset(new AutoLink(UART_NUM_2, 16, 17, true, myBauds, 10, 100));
}

void loop() {
    // Monitor the internal state machine
    State currentState = link->getState();

    if (currentState == State::SWP) {
        Serial.println("Connection lost. Sweeping for device...");
    } 
    else if (currentState == State::LCK) {
        Serial.println("Device found! Locking baud rate...");
    } 
    else if (currentState == State::OK) {
        
        // We are connected. Let's process some incoming data.
        if (link->available() >= 5) { 
            uint8_t payload[5];
            link->read(payload, 5);

            // Example: Validate your own application-level checksum
            bool isDataValid = verifyMyChecksum(payload, 5);

            if (isDataValid) {
                // IMPORTANT: Acknowledge good data to decay the error counter!
                link->clearErr();
            } else {
                // If the data is corrupt (likely due to noise), report it.
                // If this happens 10 times (our threshold), AutoLink will 
                // automatically sever the connection and start a new sweep.
                link->err();
                Serial.printf("Corrupt packet! Warning count: %d\n", link->getErrCount());
            }
        }
    }
    
    delay(10);
}

'''

# 📦 Features Under the Hood
​100% Non-Blocking: AutoLink relies on a dedicated FreeRTOS task, hardware interrupts, and StreamBuffers. Your loop() will never get blocked by a full TX buffer or a stalled RX line.

​Smart Framing: When in a negotiation state (SWP or LCK), AutoLink wraps commands in a multi-byte, CRC8-validated frame. Electrical noise physically cannot trigger a false-positive state change.
​
Namespace Isolation: Everything lives cleanly inside namespace autolink, preventing frustrating naming collisions with standard Arduino or ESP-IDF libraries.

​Test-Driven Core: The core state machine (ALink) is completely decoupled from the ESP32 hardware via Dependency Injection. Run make test to compile and verify the mathematical logic natively on your PC!

​# 📜 License
​MIT License. See LICENSE for details. Build something awesome.
