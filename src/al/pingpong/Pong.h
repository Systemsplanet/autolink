// Pong role: RX-driven echo.
// Holds PingPongBase by composition.
#pragma once
#ifdef ARDUINO

#    include "al/pingpong/PingPongBase.h"

namespace autolink
{
class Pong
{
public:
    Pong(uint32_t debugBaud, uart_port_t uartNum, int rxPin, int txPin,
         const char *ssid = nullptr, const char *password = nullptr,
         uint16_t webPort = 8765)
        : base_(debugBaud, uartNum, rxPin, txPin, false, ssid, password,
                webPort)
    {
    }

    Pong(const Pong &) = delete;
    Pong &operator=(const Pong &) = delete;

    void setup()
    {
        base_.log_.debug("Pong", "setup: calling setupCommon");
        base_.setupCommon();
        base_.log_.info("Pong", "mode=Pong  ready");
    }

    void loop()
    {
        if (!base_.comm_.ready()) {
            if (base_.wasReady_) {
                base_.log_.info("Pong", "link lost  echoes_sent=%lu",
                                (unsigned long)echoCount_);
                base_.wasReady_ = false;
                tNotReady_ = millis();

                base_.resetStatBaseline();
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

        int n;
        int recvThisLoop = 0;
        const int maxEcho = (base_.comm_.mode() == AutoLinkConfig::Mode::SYNC)
            ? 1
            : MAX_TX_PER_LOOP;
        while ((n = base_.comm_.recv(base_.buf_, sizeof base_.buf_)) > 0 &&
               recvThisLoop < maxEcho) {
            recvThisLoop++;
            if (base_.comm_.send(base_.buf_, n)) {
                echoCount_++;
                base_.log_.debug("Pong", "echo #%lu  %d bytes  ok",
                                 (unsigned long)echoCount_, n);
            } else {
                base_.log_.error(
                    "Pong", "echo #%lu  %d bytes  SEND FAILED (link dropped)",
                    (unsigned long)echoCount_, n);
            }
            base_.comm_.blinkWait(1);
        }
        if (n < 0) {
            base_.log_.error("Pong",
                             "recv rejected (CRC/desync)  echoCount=%lu",
                             (unsigned long)echoCount_);
        }

        base_.logStats("Pong", echoCount_, 0);
    }

private:
    uint64_t echoCount_ = 0;
    uint32_t tNotReady_ = 0;

    static constexpr int MAX_TX_PER_LOOP = 16;

    PingPongBase base_;
};

} // namespace autolink
#endif