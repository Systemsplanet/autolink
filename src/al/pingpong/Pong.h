
#pragma once
#ifdef ARDUINO

#    include "al/pingpong/PingPongBase.h"

namespace autolink {
class Pong {
public:
    Pong(uint32_t debugBaud, uart_port_t uartNum, int rxPin, int txPin,
         const char *ssid = nullptr, const char *password = nullptr,
         uint16_t webPort = 8765)
        : base_(debugBaud, uartNum, rxPin, txPin, false, ssid, password,
                webPort) {}

    Pong(const Pong &) = delete;
    Pong &operator=(const Pong &) = delete;

    void setup() {
        base_.log_.debug("Pong", "setup: sequencing init steps");
        const char *role = "Pong";

        initSerial(base_.log_, base_.debugBaud_, role, base_.ssid_);

        startWebMonitor(base_.log_, base_.mon_, role, base_.ssid_,
                        base_.password_, base_.webPort_);

        bringUpLink(base_.log_, base_.comm_);

        base_.comm_.kickoff();
        base_.log_.info("Pong", "mode=Pong  ready  awaiting Ping break");
    }

    void loop() {
        if (!base_.comm_.ready()) {
            if (base_.wasReady_) {
                base_.log_.info("Pong", "link lost  acks_sent=%lu",
                                (unsigned long)ackCount_);
                base_.wasReady_ = false;
                tNotReady_ = millis();

                resetStatBaseline(stat_);
            } else {
                uint32_t now = millis();
                if (now - tNotReady_ >= 1000) {
                    base_.log_.debug("Pong", "not ready");
                    tNotReady_ = now;
                }
            }
            base_.comm_.blinkWait(3, 100, 100, 0);
            return;
        }
        if (!base_.wasReady_) {
            base_.comm_.blinkWait(4);
            base_.wasReady_ = true;
            tReady_ = millis();
            return;
        }
        if (tReady_ != 0) {
            if (millis() - tReady_ < SETTLE_MS) {
                return;
            }
            int drained = 0;
            {
                uint8_t tmp[PingPongBase::BUF_SIZE];
                int n;
                while ((n = base_.comm_.recv(tmp, sizeof tmp)) > 0)
                    drained++;
            }
            base_.log_.debug("Pong", "drained %d stale bytes post-settle",
                             drained);
            tReady_ = 0;
        }

        int n;
        int recvThisLoop = 0;
        const int maxAck = (base_.comm_.mode() == AutoLinkConfig::Mode::SYNC)
            ? 1
            : PingPongBase::MAX_TX_PER_LOOP;
        while ((n = base_.comm_.recv(base_.buf_, sizeof base_.buf_)) > 0 &&
               recvThisLoop < maxAck) {
            recvThisLoop++;
            ackCount_++;
            // Info level: per-echo ASYNC pipeline traffic is
            // the operator's primary per-chunk signal in field
            // logs, complements the wire-COBS verbose line and
            // surfaces delivered-sequence progression distinctly
            // from the wire trace.
            base_.log_.info("Pong", "echo %u %d",
                            (unsigned)base_.comm_.lastRxSeq(), n);
            base_.comm_.blinkWait(1);
        }
        if (n < 0) {
            Stats s;
            base_.comm_.getStats(s);
            base_.log_.error("Pong",
                             "recv rejected (CRC/desync)  ackCount=%lu  "
                             "frameErrs=%lu",
                             (unsigned long)ackCount_,
                             (unsigned long)s.frameErrs);
        }

        AppStateLog app;
        app.paused = false;
        app.lastSendMsgReason = (int)base_.comm_.lastSendMsgReason();
        logStats(base_.log_, "Pong", base_.comm_, stat_, ackCount_, 0, 0, app);
    }

private:
    static constexpr uint32_t SETTLE_MS = AUTOLINK_APP_SETTLE_MS;

    uint64_t ackCount_ = 0;
    uint32_t tNotReady_ = 0;
    uint32_t tReady_ = 0;
    StatBaseline stat_;

    PingPongBase base_;
};

} // namespace autolink
#endif
