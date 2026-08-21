
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
            // blinkWait(..., 0) is non-blocking (blinker.start()); with
            // the link down this branch returns every loop() call with
            // nothing else in the path that yields. Arduino's loopTask
            // has no built-in inter-iteration yield, so a bare "spin
            // and return" here pins core 1 at full speed for as long as
            // the link stays down — starving anything else scheduled
            // on that core, including the log drain task, at exactly
            // the point in boot (WiFi/httpd coming up, link not yet
            // synced) where an operator most needs those logs to
            // actually flush.
            delay(10);
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
                // AL87-09: this was a bare `return` — Pong
                // never touched comm_.recv() for the whole 600ms
                // settle window. The link layer keeps accepting
                // and buffering incoming chunks the whole time
                // (only the first AUTOLINK_WIRE_SETTLE_MS=50ms
                // wire-level gate actually discards), so any data
                // the master sends inside this window accumulates
                // in the app buffer completely undrained. If the
                // master's own settle window ends even slightly
                // before this side's, its first sends land here
                // and fill the buffer from a real message the app
                // never asked to see yet — exactly the "app buf
                // full ... NAK, no write, no seq advance" storm
                // seen within a second of every field lock. Drain
                // (and discard — this data predates app-ready) on
                // every loop iteration instead of once at the end,
                // so the buffer never has the chance to fill.
                // AL89-12: tmp hoisted to a member
                // (scratch_) — the loopTask's
                // 8 KB stack was holding two
                // 5 KB stack buffers in two
                // adjacent scopes, every
                // settle/post-settle iteration.
                // Pinned by
                // PongScratchHoistedTest.
                while (base_.comm_.recv(scratch_, sizeof scratch_) > 0) {
                }
                return;
            }
            int drained = 0;
            {
                int n;
                while ((n = base_.comm_.recv(scratch_, sizeof scratch_)) > 0)
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
        // AL89-10: cap tested BEFORE recv. The
        // previous order — recv() then test the
        // cap — consumed a message before the
        // cap fired; when recvThisLoop == maxAck
        // the message was delivered, drained from
        // the app buf, and thrown away without an
        // ack. Pinned by
        // PongRecvCapReordersTest.
        while (recvThisLoop < maxAck &&
               (n = base_.comm_.recv(base_.buf_, sizeof base_.buf_)) > 0) {
            recvThisLoop++;
            ackCount_++;
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
    // AL89-12: hoisted from a stack-local in
    // loop(). Two 5 KB stack buffers in two
    // adjacent scopes were crushing the
    // loopTask's 8 KB stack frame. Held as a
    // member so the cost is paid once at
    // construction, not per loop iteration.
    uint8_t scratch_[PingPongBase::BUF_SIZE];

    PingPongBase base_;
};

} // namespace autolink
#endif
