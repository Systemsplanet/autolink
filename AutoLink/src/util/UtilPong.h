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
        log_.debug("Pong", "setup: calling setupCommon");
        setupCommon();
        log_.debug("Pong", "setup complete  BUF_SIZE=%d", BUF_SIZE);
    }

    void loop() {
        if (!comm_.ready()) {
            if (wasReady_) {
                log_.info("Pong", "link lost  echoes_sent=%lu", (unsigned long)echoCount_);
                wasReady_ = false;
                tNotReady_ = millis();   // start rate limiter
            } else {
                uint32_t now = millis();
                if (now - tNotReady_ >= 1000) {
                    log_.debug("Pong", "not ready");
                    tNotReady_ = now;
                }
            }
            comm_.blinkWait(3, 100, 100, 0);
            return;
        }
        if (!wasReady_) {
            log_.debug("Pong", "link up  baud=%lu",
                (unsigned long)comm_.getCurrentBaud());
            // Drain any stale bytes from the RX buffer that accumulated
            // during SWP. The drain loop exits on the first recv=-1 (partial
            // or corrupt message), which leaves residual bytes in the buffer.
            // The flushRx() call afterwards clears both the stream buffer and
            // the UART driver ring so the first echo attempt is always clean.
            { uint8_t tmp[BUF_SIZE]; int n; while ((n = comm_.recv(tmp, sizeof tmp)) > 0) {}
              if (n < 0) log_.error("Pong", "drain: partial msg at link-up — flushing hw ring");
            }
            comm_.flushRx();
            comm_.blinkWait(4, 100, 100, 2000);
            wasReady_ = true;
        }

        int n;
        int recvThisLoop = 0;
        while ((n = comm_.recv(buf_, sizeof buf_)) > 0 && recvThisLoop < MAX_TX_PER_LOOP) {
            recvThisLoop++;
            if (comm_.send(buf_, n)) {
                echoCount_++;
                log_.debug("Pong", "echo #%lu  %d bytes  ok",
                    (unsigned long)echoCount_, n);
            } else {
                log_.error("Pong",
                    "echo #%lu  %d bytes  SEND FAILED (link dropped)",
                    (unsigned long)echoCount_, n);
            }
            comm_.blinkWait(1);   // one flash per echo — visual heartbeat
        }
        if (n < 0) {
            log_.error("Pong", "recv rejected (CRC/desync)  echoCount=%lu",
                (unsigned long)echoCount_);
            // Flush both the stream buffer and the UART ring so stale Ping
            // bytes do not keep rejecting every subsequent recv. Note: if the
            // reject storm persists, Ping's idle watchdog will drop and BREAK
            // after 5 s, which stops Ping's TX and lets both sides re-sweep.
            comm_.flushRx();
        }
        if (recvThisLoop > 0) {
            log_.debug("Pong", "processed %d msgs this loop  echoCount=%lu",
                recvThisLoop, (unsigned long)echoCount_);
        }

        logStats("Pong");
    }
private:
    uint64_t echoCount_ = 0;   // lifetime echo counter for debug logging
    uint32_t tNotReady_ = 0;   // millis() of last not-ready log (rate limiter)

    // Limit echoes per loop() call so large back-to-back sends don't fill the
    // UART TX ring and block uart_write_bytes while holding the ALink lock,
    // which would starve the UART event task and overflow the RX ring.
    static constexpr int MAX_TX_PER_LOOP = 2;
};

} // namespace autolink
#endif // ARDUINO
