// UtilSlave.h — ready-to-run AutoLink slave for the ping-pong echo test.
//
// Wraps AutoLink + AutoLinkWeb + the receive/echo loop from the README Quick
// Start into a single setup()/loop() object. Pair with a UtilMaster on the
// other board.
//
// The WiFi web monitor is optional: pass a non-null SSID to enable it.
// If the SSID is omitted (or nullptr), the UART link runs unaffected and
// the AutoLinkWeb object is constructed but never started.
//
// Usage:
//   UtilSlave us(115200, UART_NUM_2, 16, 17);           // UART only
//   UtilSlave us(115200, UART_NUM_2, 16, 17,            // + web monitor
//                "YourSSID", "password", 80);
//   void setup() { us.setup(); }
//   void loop()  { us.loop();  }
#pragma once
#ifdef ARDUINO

#include "AutoLink.h"
#include "AutoLinkWeb.h"
#include <Arduino.h>

namespace autolink {

// ----------------------------------------------------------------------------
// UtilSlave — plug-and-play AutoLink slave.
//
// Echoes every complete message back to the master, logs the byte count, and
// blinks the LED once per echo. Reconnects after any link disruption
// automatically — no state machine needed in the sketch.
// ----------------------------------------------------------------------------
class UtilSlave {
public:
    UtilSlave(uint32_t    debugBaud,
              uart_port_t uartNum,
              int         rxPin,
              int         txPin,
              const char* ssid     = nullptr,
              const char* password = nullptr,
              uint16_t    webPort  = 8765)
        : debugBaud_(debugBaud)
        , comm_(uartNum, rxPin, txPin, /*isMaster=*/false)
        , mon_(comm_)
        , ssid_(ssid)
        , password_(password ? password : "")
        , webPort_(webPort)
        , log_(Log::getLog())
    {}

    // Non-copyable — AutoLink and AutoLinkWeb own hardware resources.
    UtilSlave(const UtilSlave&)            = delete;
    UtilSlave& operator=(const UtilSlave&) = delete;

    void setup() {
        esp_log_level_set("*", ESP_LOG_VERBOSE);
        log_.setLevel(Log::DEBUG);
        Serial.begin(debugBaud_);
        comm_.blinkWait(1, 100, 100, 2000);
        comm_.begin();                                   // SWP: waits for master
        if (ssid_) mon_.begin(ssid_, password_, webPort_);
        comm_.blinkWait(2, 100, 100, 2000);
    }

    void loop() {
        if (!comm_.ready()) {
            log_.debug("Main", "comm not ready");
            comm_.blinkWait(3, 100, 100, 2000);
            wasReady_ = false;
            return;
        }
        if (!wasReady_) {
            log_.debug("Main", "comm ready");
            comm_.blinkWait(4, 100, 100, 2000);
            wasReady_ = true;
        }

        int n;
        while ((n = comm_.recv(buf_, sizeof buf_)) > 0) {
            log_.debug("Main", "recv %d bytes", n);
            if (comm_.send(buf_, n)) {
                log_.debug("Main", "echoed %d bytes", n);
            } else {
                log_.error("Main",
                    "echo send failed (link dropped)  %d bytes dropped", n);
            }
            comm_.blinkWait(1);   // one flash per echo — visual heartbeat
        }
    }

private:
    // ── config ────────────────────────────────────────────────────────────
    uint32_t    debugBaud_;
    AutoLink    comm_;
    AutoLinkWeb mon_;
    const char* ssid_;
    const char* password_;
    uint16_t    webPort_;
    Log&        log_;

    // ── per-loop state ────────────────────────────────────────────────────
    uint8_t buf_[1024];        // receive / echo buffer
    bool    wasReady_ = false;
};

} // namespace autolink
#endif // ARDUINO
