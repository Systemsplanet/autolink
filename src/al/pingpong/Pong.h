// Pong role: RX-driven echo. No local pending state —
// the wire guarantees in-order delivery, so each recv
// maps 1:1 to a send.
#pragma once
#ifdef ARDUINO

#    include "al/pingpong/PingPongBase.h"

namespace autolink
{
class Pong : public PingPongBase
{
public:
    Pong(uint32_t debugBaud, uart_port_t uartNum,
         int rxPin, int txPin,
         const char *ssid = nullptr,
         const char *password = nullptr,
         uint16_t webPort = 8765)
        : PingPongBase(debugBaud, uartNum, rxPin,
                       txPin, false, ssid, password,
                       webPort)
    {
    }

    Pong(const Pong &) = delete;
    Pong &operator=(const Pong &) = delete;

    void setup()
    {
        log_.debug("Pong",
                   "setup: calling setupCommon");
        setupCommon();
        log_.info("Pong", "mode=Pong  ready");
    }

    void loop()
    {
        if (!comm_.ready()) {
            if (wasReady_) {
                log_.info("Pong",
                          "link lost  echoes_sent=%lu",
                          (unsigned long)echoCount_);
                wasReady_ = false;
                tNotReady_ = millis();


                resetStatBaseline();
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
            int drained = 0;
            {
                uint8_t tmp[BUF_SIZE];
                int n;
                while ((n = comm_.recv(
                            tmp, sizeof tmp)) > 0)
                    drained++;
            }
            log_.debug(
                "Pong",
                "drained %d stale bytes pre-blink",
                drained);
            comm_.blinkWait(4);
            wasReady_ = true;
        }

        int n;
        int recvThisLoop = 0;
        while ((n = comm_.recv(buf_, sizeof buf_)) >
                   0 &&
               recvThisLoop < MAX_TX_PER_LOOP) {
            recvThisLoop++;
            if (comm_.send(buf_, n)) {
                echoCount_++;
                log_.debug(
                    "Pong", "echo #%lu  %d bytes  ok",
                    (unsigned long)echoCount_, n);
            } else {
                log_.error("Pong",
                           "echo #%lu  %d bytes  SEND "
                           "FAILED (link dropped)",
                           (unsigned long)echoCount_,
                           n);
            }
            comm_.blinkWait(1);
        }
        if (n < 0) {
            log_.error("Pong",
                       "recv rejected (CRC/desync)  "
                       "echoCount=%lu",
                       (unsigned long)echoCount_);
        }

        logStats("Pong");
    }

private:
    uint64_t echoCount_ = 0;
    uint32_t tNotReady_ = 0;


    static constexpr int MAX_TX_PER_LOOP = 16;
};

} // namespace autolink
#endif