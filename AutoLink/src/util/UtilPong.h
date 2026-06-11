// UtilPong.h — ready-to-run AutoLink pong node for the ping-pong echo test.
//
// Wraps AutoLink + AutoLinkWeb + the receive/echo loop from the README Quick
// Start into a single setup()/loop() object. Pair with UtilPing on the
// other board.
//
// Usage:
//   UtilPong pong(115200, UART_NUM_2, 16, 17);           // UART only
//   UtilPong pong(115200, UART_NUM_2, 16, 17,            // + web monitor
//                "YourSSID", "password", 80);
//   void setup() { pong.setup(); }
//   void loop()  { pong.loop();  }
#pragma once
#ifdef ARDUINO

#include "UtilMain.h"

namespace autolink {

// ----------------------------------------------------------------------------
// UtilPong — plug-and-play AutoLink pong node.
//
// Echoes every complete message back to Ping, logs the byte count, and
// blinks the LED once per echo. Reconnects after any link disruption
// automatically — no state machine needed in the sketch.
// ----------------------------------------------------------------------------
class UtilPong : public UtilMain {
public:
    UtilPong(uint32_t    debugBaud,
             uart_port_t uartNum,
             int         rxPin,
             int         txPin,
             const char* ssid     = nullptr,
             const char* password = nullptr,
             uint16_t    webPort  = 8765)
        : UtilMain(debugBaud, uartNum, rxPin, txPin, /*isPing=*/false,
                   ssid, password, webPort)
    {}

    // Non-copyable — hardware resources are owned by the base.
    UtilPong(const UtilPong&)            = delete;
    UtilPong& operator=(const UtilPong&) = delete;

    void setup() {
        setupCommon();
    }

    void loop() {
        if (!comm_.ready()) {
            log_.debug("Pong", "not ready");
            comm_.blinkWait(3, 100, 100, 2000);
            wasReady_ = false;
            return;
        }
        if (!wasReady_) {
            log_.debug("Pong", "ready");
            comm_.blinkWait(4, 100, 100, 2000);
            wasReady_ = true;
        }

        int n;
        while ((n = comm_.recv(buf_, sizeof buf_)) > 0) {
            log_.debug("Pong", "recv %d bytes", n);
            if (comm_.send(buf_, n)) {
                log_.debug("Pong", "echoed %d bytes", n);
            } else {
                log_.error("Pong",
                    "echo send failed (link dropped)  %d bytes dropped", n);
            }
            comm_.blinkWait(1);   // one flash per echo — visual heartbeat
        }

        logStats("Pong");
    }
};

} // namespace autolink
#endif // ARDUINO
