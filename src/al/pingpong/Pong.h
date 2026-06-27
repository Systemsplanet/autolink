// Pong role: RX-driven ack-only.
// Holds PingPongBase by composition.
//
// Pong does NOT echo the received payload. For each received
// frame the link layer sends a wire-level ACK carrying the
// acked cobsSeq and the number of bytes received (extended
// wire ACK frame; see LinkTx::sendAckFrame_unlocked). Pong's
// loop just tracks the ack count and verifies the per-chunk
// CRC was correct (the link layer already drops frames with
// a bad CRC before they reach onPayload, so an ACK means
// the CRC was correct by construction).
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

        // Step 1: Serial + boot banner first so we can see
        // what happens during WiFi/httpd bring-up.
        initSerial(base_.log_, base_.debugBaud_, role, base_.ssid_);

        // Step 2: WiFi + httpd (up to 5 s quick-start window;
        // bg task keeps retrying forever after that).
        startWebMonitor(base_.log_, base_.mon_, role, base_.ssid_,
                        base_.password_, base_.webPort_);

        // Step 3: bring up the link layer.
        bringUpLink(base_.log_, base_.comm_);

        // Pong (slave) starts in unpaused mode and
        // immediately arms the SWP P1 listener so it
        // can hear Ping's break when the user pushes
        // Start on Ping's dashboard.
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
            int drained = 0;
            {
                uint8_t tmp[PingPongBase::BUF_SIZE];
                int n;
                while ((n = base_.comm_.recv(tmp, sizeof tmp)) > 0)
                    drained++;
            }
            base_.log_.debug("Pong", "drained %d stale bytes pre-blink",
                             drained);
            base_.comm_.blinkWait(4);
            base_.wasReady_ = true;
        }

        // Drain incoming frames. The link layer already
        // sent the per-frame wire ACK with the cobsSeq and
        // bytes-recvd as soon as onPayload fired (see
        // LinkRx::onPayload -> sendAckFrame_unlocked with
        // the bytes-recvd). Pong does NOT echo the
        // payload back; the wire ACK is the entire
        // Pong-side response. Just count the receive
        // and let the app buffer fill; nothing else to
        // send.
        int n;
        int recvThisLoop = 0;
        const int maxAck = (base_.comm_.mode() == AutoLinkConfig::Mode::SYNC)
            ? 1
            : PingPongBase::MAX_TX_PER_LOOP;
        while ((n = base_.comm_.recv(base_.buf_, sizeof base_.buf_)) > 0 &&
               recvThisLoop < maxAck) {
            recvThisLoop++;
            ackCount_++;
            base_.log_.debug(
                "Pong",
                "echo %u %d",
                (unsigned)base_.comm_.lastRxSeq(), n);
            base_.comm_.blinkWait(1);
        }
        if (n < 0) {
            base_.log_.error("Pong",
                             "recv rejected (CRC/desync)  ackCount=%lu",
                             (unsigned long)ackCount_);
        }

        logStats(base_.log_, "Pong", base_.comm_, stat_, ackCount_, 0);
    }

private:
    uint64_t ackCount_ = 0;
    uint32_t tNotReady_ = 0;
    StatBaseline stat_;

    PingPongBase base_;
};

} // namespace autolink
#endif
