// UtilPong.h — ready-to-run AutoLink pong node for the ping-pong echo test.
//
// v4.0.0: Pong now logs the cobsSeq of every received message and the
// cobsSeq of the echo it sends back. With the wire-layer cobsSeq gap
// detection in ALink, any stale echo from a previous session is dropped
// before it ever reaches Pong's message layer, so the Pong loop is much
// simpler than the v3.x version.
//
// Pair with UtilPing on the other board.
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
// UtilPong — plug-and-play AutoLink pong node (v4.0.0).
//
// Echoes every complete message back to Ping, logs the cobsSeq of the
// received and echoed frames, and blinks the LED once per echo. Reconnects
// after any link disruption automatically — no state machine needed in the
// sketch.
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
        // v4.0.5: include MAX_TX_PER_LOOP in the setup log so a log
        // reader can see at a glance that the v4.0.5 throughput fix
        // (MAX_TX_PER_LOOP 2→4) is in effect. Pong's other tunables
        // (BUF_SIZE, idleTimeoutMs) come from the base class and would
        // be redundant here.
        log_.debug("Pong",
            "setup complete  MAX_TX_PER_LOOP=%d  BUF_SIZE=%d",
            MAX_TX_PER_LOOP, BUF_SIZE);
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
            // Drain any stale bytes from the RX buffer that accumulated
            // during SWP. The drain loop exits on the first recv=-1 (partial
            // or corrupt message), which leaves residual bytes in the buffer.
            // The flushRx() call afterwards clears both the stream buffer and
            // the UART driver ring so the first echo attempt is always clean.
            // v4.0.0: the cobsSeq gap detection in ALink will also drop any
            // stale bytes that arrive after the drain, so this is now a
            // belt-and-suspenders setup.
            int drained = 0;
            { uint8_t tmp[BUF_SIZE]; int n; while ((n = comm_.recv(tmp, sizeof tmp)) > 0) drained++; }
            log_.debug("Pong", "drained %d stale bytes pre-blink", drained);
            comm_.flushRx();
            // v4.0.5: changed from the v4.0.0..v4.0.4 blocking call
            //   comm_.blinkWait(4, 100, 100, 2000)
            // (which blocked for 4*(100+100) + 2000 = 2800 ms) to the
            // async form. The blocking blink starved the echo loop for
            // 2.8 seconds on every link-up, which is long enough to
            // trigger Ping's 3-second STALL_MS watchdog and force a
            // re-sweep — destroying the fast-ack advantage. The async
            // blink returns immediately; the LED pattern runs on an
            // esp_timer and doesn't block the echo loop. Stale-byte
            // risk is already handled by the drain + flushRx above.
            comm_.blinkWait(4);
            wasReady_ = true;
        }

        int n;
        int recvThisLoop = 0;
        while ((n = comm_.recv(buf_, sizeof buf_)) > 0 && recvThisLoop < MAX_TX_PER_LOOP) {
            recvThisLoop++;
            // v4.0.0: cobsSeq is part of the wire format now. We don't
            // extract it here — ALink already used it to drop stale frames
            // and the sender's cobsSeq is implicit in the message body. The
            // gap counter in the log gives us the diagnostic.
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
            comm_.blinkWait(1);   // one flash per echo — visual heartbeat
        }
        if (n < 0) {
            // v4.0.0: this should be rare. The wire-layer cobsSeq gap
            // detection catches most of the cases that used to land here.
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
    uint64_t echoCount_ = 0;   // lifetime echo counter for debug logging
    uint32_t tNotReady_ = 0;   // millis() of last not-ready log (rate limiter)

    // v4.0.5: raised from 2 to 4, matching UtilPing. The original v4.0.0
    // value of 2 was set to protect against an undersized txBufferSize;
    // v4.0.1+ auto-sizes the TX ring so 4 echoes per loop is safe and
    // necessary — Pong's drain rate has to keep up with Ping's send rate
    // or the WINDOW=8 pipeline never fills. 4 is half of WINDOW so a
    // single miss doesn't completely stall the pipeline.
    static constexpr int MAX_TX_PER_LOOP = 4;
};

} // namespace autolink
#endif // ARDUINO
