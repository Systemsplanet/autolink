// UtilPong.h — plug-and-play AutoLink pong node. Echoes every complete
// message back to Ping. Reconnects after link disruption automatically —
// no state machine needed in the sketch.
#pragma once
#ifdef ARDUINO

#include "UtilMain.h"

namespace autolink {

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

    UtilPong(const UtilPong&)            = delete;
    UtilPong& operator=(const UtilPong&) = delete;

    void setup() {
        log_.debug("Pong", "setup: calling setupCommon");
        setupCommon();
        log_.debug("Pong",
            "setup complete  MAX_TX_PER_LOOP=%d  BUF_SIZE=%d",
            MAX_TX_PER_LOOP, BUF_SIZE);
        log_.info("Pong", "mode=Pong  ready");
    }

    void loop() {
        if (!comm_.ready()) {
            if (wasReady_) {
                log_.info("Pong", "link lost  echoes_sent=%lu", (unsigned long)echoCount_);
                wasReady_ = false;
                tNotReady_ = millis();
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
            log_.debug("Pong", "link up  baud=%lu  blink=async",
                (unsigned long)comm_.getCurrentBaud());
            // Drain + flush stale pre-link bytes. cobsSeq catches
            // any residue anyway; this just makes the first echo
            // attempt clean.
            int drained = 0;
            { uint8_t tmp[BUF_SIZE]; int n; while ((n = comm_.recv(tmp, sizeof tmp)) > 0) drained++; }
            log_.debug("Pong", "drained %d stale bytes pre-blink", drained);
            comm_.flushRx();
            // Async blink — blocking form held the loop for ~2.8 s
            // and tripped Ping's 3 s STALL_MS watchdog.
            comm_.blinkWait(4);
            wasReady_ = true;
        }

        int n;
        int recvThisLoop = 0;
        while ((n = comm_.recv(buf_, sizeof buf_)) > 0 && recvThisLoop < MAX_TX_PER_LOOP) {
            recvThisLoop++;
            Diag d; comm_.getDiag(d);
            if (comm_.send(buf_, n)) {
                echoCount_++;
                log_.debug("Pong", "echo #%lu  %d bytes  ok  (cobsSeq gap=%llu stale=%llu)",
                    (unsigned long)echoCount_, n,
                    (unsigned long long)d.gaps,
                    (unsigned long long)d.stale);
            } else {
                log_.error("Pong",
                    "echo #%lu  %d bytes  SEND FAILED (link dropped)",
                    (unsigned long)echoCount_, n);
            }
            comm_.blinkWait(1);   // visual heartbeat per echo
        }
        if (n < 0) {
            // Rare — wire-layer cobsSeq catches most cases.
            Diag d; comm_.getDiag(d);
            log_.error("Pong", "recv rejected (CRC/desync)  echoCount=%lu  gap=%llu stale=%llu",
                (unsigned long)echoCount_,
                (unsigned long long)d.gaps,
                (unsigned long long)d.stale);
            comm_.flushRx();
        }
        if (recvThisLoop > 0) {
            log_.debug("Pong", "processed %d msgs this loop  echoCount=%lu",
                recvThisLoop, (unsigned long)echoCount_);
        }

        logStats("Pong");
    }
private:
    uint64_t echoCount_ = 0;
    uint32_t tNotReady_ = 0;   // millis() of last not-ready log (rate limiter)

    // Match UtilPing's MAX_TX_PER_LOOP — Pong's per-tick echo cap is
    // the only knob controlling how fast the RX path clears for new
    // frames, so it must scale with Ping's send rate or Pong becomes
    // the bottleneck.
    static constexpr int MAX_TX_PER_LOOP = 16;
};

} // namespace autolink
#endif // ARDUINO
