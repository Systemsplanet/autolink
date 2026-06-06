# 🚀 AutoLink ESP32

**A production-grade, self-healing UART protocol layer for ESP32.**


Ever had a UART connection drop because a motor spun up nearby? Have you ever hardcoded a baud rate, only to realize the other microcontroller reset and desynced? Raw UART is notoriously fragile in real-world applications.


**AutoLink** fixes this. It sits on top of the standard ESP-IDF UART drivers and transforms your serial connection into a robust, auto-negotiating, self-healing lifeline. 


If the line gets noisy, AutoLink drops down to a safer baud rate. If a wire gets bumped, it automatically sweeps and locks back onto the connection.

It manages all the FreeRTOS background tasks, queues, and hardware interrupts automatically. You just read and write data.


# ⚡ Quick Start
Drop the AutoLink files into your project, include the header, and let the library do the heavy lifting.


``` cpp
#include "AutoLink.h"

using namespace autolink;

// built-in blue LED on pin 2
const int ledPin = 2;

AutoLink* myLink = nullptr;

void flash(int times = 1) {
  for (int i = 0; i < times; i++) {
    digitalWrite(ledPin, HIGH);
    delay(100);
    digitalWrite(ledPin, LOW);
    delay(100);
  }
  if (times > 1) delay(2000);
}


void setup() {
    // Silence AutoLink library logs (optional)
    // Log::getLog().setLevel(Log::NONE);
    pinMode(ledPin, OUTPUT);
    flash(2);
    Serial.begin(115200);

    AutoLinkConfig cfg;
    cfg.reliableMode = true;
    cfg.streamBufferSize = 2048;
    flash(3);
    myLink = new AutoLink(UART_NUM_1, 16, 17, true, cfg);
    myLink->begin(); // Triggers baud sweep; blocks until HAL is healthy
    flash(4);

    if (!myLink->isHealthy()) {
        Log::getLog().error("App", "Failed to initialize UART hardware!");
        while (true) delay(1000);
    }

    // Wait for negotiation to complete before sending user data.
    Log::getLog().info("App", "Waiting for link...");
    while (myLink->getState() != State::OK) delay(10);
    flash(5);
    Log::getLog().info("App", "Link established. Starting loop.");
}

void loop() {
    // Only transmit when the link is healthy.
    if (myLink->getState() == State::OK) {
        const char* str = "Hello Slave!";
        myLink->write((const uint8_t*)str, strlen(str)); // no null terminator
        flash(2);
    }

    while (myLink->available()) {
        uint8_t buffer[64];
        int len = myLink->read(buffer, sizeof(buffer));
        buffer[len] = '\0'; // safe: buffer has room
        Log::getLog().info("App", "Received %d bytes: %s", len, (char*)buffer);
        flash(1);
    }

    delay(500); // throttle ping rate
}
```




# 🛠️ Simple Usage: Master & Slave
AutoLink requires one device to act as the **Master** (initiates the baud negotiation) and one to act as the **Slave** (listens and responds to the sweep).

## The Slave Node

Setting up a listening device is just as easy as setting up the master. Just pass **false** for the master flag!


```cpp
#include "AutoLink.h"

using namespace autolink;

// built-in blue LED on pin 2
const int ledPin = 2;

AutoLink* myLink = nullptr;

void flash(int times = 1) {
  for (int i = 0; i < times; i++) {
    digitalWrite(ledPin, HIGH);
    delay(100);
    digitalWrite(ledPin, LOW);
    delay(100);
  }
  if (times > 1) delay(2000);
}

void setup() {
    pinMode(ledPin, OUTPUT);
    flash(2);
    Serial.begin(115200);

    AutoLinkConfig cfg;
    cfg.reliableMode = true;
    cfg.streamBufferSize = 2048;
    flash(3);
    myLink = new AutoLink(UART_NUM_1, 16, 17, false, cfg);
    myLink->begin(); // Arms slave in SWP — waits for master's PING sweep

    if (!myLink->isHealthy()) {
        Log::getLog().error("App", "Failed to initialize UART hardware!");
        while (true) delay(1000);
    }
    flash(4);

    // Wait for the Master to find us and lock on.
    // The master drives negotiation; we just wait here.
    Log::getLog().info("App", "Waiting for master...");
    while (myLink->getState() != State::OK) {
        flash(1);
        delay(200);
    }
    flash(5);
    Log::getLog().info("App", "Link established. Starting loop.");
}

void loop() {
    if (myLink->available()) {
        uint8_t buffer[64];
        int len = myLink->read(buffer, sizeof(buffer));
        buffer[len] = '\0'; // safe: buffer has room
        Log::getLog().info("App", "Received %d bytes: %s", len, (char*)buffer);
        flash(1);

        // Echo the data back — only when link is confirmed OK.
        if (myLink->getState() == State::OK) {
            myLink->write(buffer, len);
            flash(2);
        }
    }
}

# 🧠 Advanced Usage: The Power User API

AutoLink isn't just a wrapper; it's a dynamic state machine. For mission-critical applications, you want fine-grained control over exactly how the system reacts to noise, which baud rates it's allowed to use, and how aggressively it can retry.


Here is an advanced example showing how to utilize the **entire API**, including custom configurations and manual error tracking.


```cpp
#include "AutoLink.h"
#include <vector>

using namespace autolink;

// Allocate globally via standard pointer to match the rest of the library's paradigm
AutoLink* link = nullptr;

// Custom validation helper function
bool verifyMyChecksum(const uint8_t* data, int len) {
    return (data[0] ^ data[1] ^ data[2] ^ data[3]) == data[4];
}

void setup() {
    Serial.begin(115200);

    // 1. Build the configuration object matching the first two examples
    AutoLinkConfig cfg;
    cfg.allowedBauds = {9600, 38400, 115200, 1000000}; // Custom allowed baud rates
    cfg.errThreshold = 10;                              // Drop connection after 10 errors
    cfg.delayMs      = 100;                             // Wait 100ms between baud rate tests
    cfg.reliableMode = false;                           // Raw processing with manual checksums

    // 2. Instantiate cleanly using the correct constructor signature
    link = new AutoLink(UART_NUM_2, 16, 17, true, cfg);
    link->begin();
}

void loop() {
    if (!link) return;

    // Monitor the internal state machine
    State currentState = link->getState();

    if (currentState == State::SWP) {
        Log::getLog().info("App", "Connection lost. Sweeping for device...");
    } 
    else if (currentState == State::LCK) {
        Log::getLog().info("App", "Device found! Locking baud rate...");
    } 
    else if (currentState == State::OK) {
        
        // We are connected. Let's process some incoming data.
        if (link->available() >= 5) { 
            uint8_t payload[5];
            link->read(payload, 5);

            // Validate your own application-level checksum
            bool isDataValid = verifyMyChecksum(payload, 5);

            if (isDataValid) {
                // IMPORTANT: Acknowledge good data to decay the error counter!
                link->clearErr();
            } else {
                // If the data is corrupt, report it.
                // After 10 sequential errors (our threshold), AutoLink automatically
                // triggers a hardware BREAK and drops back to State::SWP.
                link->err();
                Log::getLog().error("App", "Corrupt packet! Warning count: %d", link->getErrCount());
            }
        }
    }
    
    delay(10);
}

```

## Robust recovery
``` cpp
#include "AutoLink.h"

using namespace autolink;

AutoLink* link = nullptr;
AutoLinkConfig globalCfg; // Keep config global so we can recreate the object identically

// Track timing for structural self-healing
unsigned long lastStateChangeMs = 0;
State lastKnownState = State::OK;
const unsigned long SWEEP_TIMEOUT_MS = 15000; // Force hard reset if stuck sweeping for 15 seconds

bool verifyMyChecksum(const uint8_t* data, int len) {
    return (data[0] ^ data[1] ^ data[2] ^ data[3]) == data[4];
}

// Dedicated hardware instantiation routine for clean reboots
void initializeAutoLink() {
    if (link != nullptr) {
        Log::getLog().info("App", "Self-Healing: Deleting corrupted AutoLink instance");
        delete link; 
        link = nullptr;
        delay(100); // Give FreeRTOS tasks time to fully dismantle
    }
    
    Log::getLog().info("App", "Self-Healing: Initializing fresh UART peripheral stack");
    link = new AutoLink(UART_NUM_2, 16, 17, true, globalCfg);
    link->begin();
    
    lastKnownState = State::OK;
    lastStateChangeMs = millis();
}

void setup() {
    Serial.begin(115200);

    // Populate global configurations
    globalCfg.allowedBauds = {9600, 38400, 115200}; 
    globalCfg.errThreshold = 10;                     
    globalCfg.delayMs      = 100;                    
    globalCfg.reliableMode = false;                  

    initializeAutoLink();
    Log::getLog().info("App", "System initialized successfully.");
}

void loop() {
    // 1. Critical Safe Check: If instantiation failed, retry immediately
    if (!link) {
        initializeAutoLink();
        return;
    }

    State currentState = link->getState();

    // 2. Monitor State Machine Health Transitions
    if (currentState != lastKnownState) {
        lastKnownState = currentState;
        lastStateChangeMs = millis(); // Reset timeout watchdog timer on any state transition
    }

    // 3. The Watchdog Recovery Engine
    // If stuck in Sweep mode for too long, the physical bus or transceiver is locked up.
    if (currentState == State::SWP && (millis() - lastStateChangeMs > SWEEP_TIMEOUT_MS)) {
        Log::getLog().error("App", "Sweep timeout exceeded! Hardware/Line deadlock suspected.");
        initializeAutoLink(); // Destroy and recreate the entire peripheral stack
        return;
    }

    // 4. State Machine Execution
    if (currentState == State::SWP) {
        Log::getLog().info("App", "Connection dropped or noisy. Sweeping available spectrum...");
        delay(500); // Reduce monitoring spam on terminal
    } 
    else if (currentState == State::LCK) {
        Log::getLog().info("App", "Handshake target identified. Locking baud rate parameters...");
    } 
    else if (currentState == State::OK) {
        
        // Actively process synchronized incoming streams
        int availableBytes = link->available();
        
        if (availableBytes >= 5) { 
            uint8_t payload[5];
            
            // Peek at data to make sure the buffer hasn't been drained by a race
            if (link->peek() == -1) {
                return; // Nothing to read after all
            }

            link->read(payload, 5);

            if (verifyMyChecksum(payload, 5)) {
                link->clearErr(); // Decay error count
                Log::getLog().info("App", "Valid user data packet decoded.");
            } else {
                // link->err() will naturally shift internal state to State::SWP if count > threshold
                link->err();
                Log::getLog().error("App", "Corrupted packet detected. Warning count: %d", link->getErrCount());
            }
        }
        // Garbage Collector: If a partial fragment lingers without completing a packet, drain it
        else if (availableBytes > 0 && availableBytes < 5 && (millis() - lastStateChangeMs > 2000)) {
            uint8_t discard[8];
            while (link->available()) link->read(discard, sizeof(discard)); // Drain stale bytes
        }
    }
    
    delay(10); // FreeRTOS CPU yield
}

```

# 📦 Features Under the Hood


+ **100% Non-Blocking:** AutoLink relies on a dedicated FreeRTOS task, hardware interrupts, and `StreamBuffers`. Your `loop()` will never get blocked by a full TX buffer or a stalled RX line.


+ **Smart Framing:** When in a negotiation state (`SWP` or `LCK`), AutoLink wraps commands in a multi-byte, CRC8-validated frame. Electrical noise physically cannot trigger a false-positive state change. Only the Master can initiate state transitions.

+ **Namespace Isolation:** Everything lives cleanly inside `namespace autolink`, preventing frustrating naming collisions with standard Arduino or ESP-IDF libraries.


+ **Test-Driven Core:** The core state machine (`ALink`) is completely decoupled from the ESP32 hardware via Dependency Injection. Run `make test` to compile and verify the mathematical logic native to your build machine.




# 🛠️ Developer Notes
​If you are contributing to or maintaining this library, keep the following architectural decisions in mind:

+ **​Hardware Abstraction (Dependency Injection):** The core protocol logic (ALink.cpp) is entirely decoupled from the ESP32 hardware (EspHal.cpp) via the ILink interface. Do not put ESP-IDF or FreeRTOS headers inside ALink.cpp.


+ ​**Native PC Testing:** Because of the abstraction mentioned above, the entire state machine and negotiation logic can be tested locally on your computer. Run make test to compile and execute the mock hardware tests in test.cpp.
  
+ **​State Machine Mechanics:**
  + ​`SWP` (Sweep): The master iterates through the allowed baud rates sending PING. The slave listens and calculates an RSSI-like score based on successful reads.
​  + `LCK` (Lock): The master requests the best baud rate from the slave. Both ends switch to the agreed speed.
​  + `OK` (Connected): Raw data or Reliable Mode frames are exchanged.

+ **​Reliable Mode:** When enabled via `AutoLinkConfig`, raw user data is encapsulated using COBS framing with a trailing CRC-8 byte. Each frame is delimited by `0x00` sentinel bytes, allowing the receiver to detect and discard corrupt or misaligned frames without desyncing the stream.

+ **​CRC Optimization:** We use a precomputed 256-byte Lookup Table (LUT) for O(1) CRC-8 calculations, keeping CPU utilization low even during high-throughput data streams.
​



# 📅 Revision History

**​v2.0.0 (Production-Ready)**

+ **​API Enhancement:** Inherited AutoLink from the standard Arduino Stream class, unlocking native compatibility with standard functions like .println(), .parseInt(), and other third-party libraries.
+ **​Reliable Mode Added:** Added an opt-in cfg.reliableMode that wraps standard user data in CRC-8 validated frames, completely shielding the application layer from corrupted bytes.
​+ **Performance Optimization:** Replaced bitwise nested loops in the CRC calculator with an O(1) 256-byte Lookup Table (LUT).
+ **​Concurrency Fixes:**
  + ​Fixed a teardown trap in the FreeRTOS uart_event_task by replacing portMAX_DELAY with pdMS_TO_TICKS(100), ensuring clean thread exits during destruction.
  + Held the mutex lock throughout the entirety of hardware TX sequences in ALink::write() to prevent interleaved transmissions from multiple tasks.
+ ​**Memory Safety:** Transitioned resource allocations to std::make_unique and patched memory leaks occurring during failed UART hardware initializations.

+ **​Developer Ergonomics:** Consolidated long constructor parameter signatures into a clean AutoLinkConfig struct.

**​v1.0.0 (Initial Prototype)**

+ ​Initial Master/Slave auto-baud negotiation logic.

+ ​Basic FreeRTOS stream buffer and hardware interrupt integration.




# 📜 License

MIT License. 

Build something awesome.
