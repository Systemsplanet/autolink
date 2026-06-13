// UtilPing.h — ready-to-run AutoLink ping node for the ping-pong echo test.
//
// Wraps AutoLink + AutoLinkWeb + the full send/compare/stats loop from the
// README Quick Start into a single setup()/loop() object. Drop the header
// into a sketch and wire three calls — that's the entire application.
//
// Pair with UtilPong on the other board. Ping initiates the baud sweep;
// Pong listens and locks onto the negotiated baud.
//
// Usage:
//   UtilPing ping(115200, UART_NUM_2, 16, 17);           // UART only
//   UtilPing ping(115200, UART_NUM_2, 16, 17,            // + web monitor
//                 "YourSSID", "password", 80);
//   void setup() { ping.setup(); }
//   void loop()  { ping.loop();  }
#pragma once
#ifdef ARDUINO

#include "UtilMain.h"
#include "UtilCrc.h"
#include <string.h>

namespace autolink {

// ----------------------------------------------------------------------------
// UtilPing — plug-and-play AutoLink ping node.
//
// Sends random-length messages (1–1023 bytes), keeps up to WINDOW messages
// in flight simultaneously (pipelined for ~2× throughput), and verifies each
// echo in FIFO order by length + CRC-16.
//
// FIFO safety rules (v3.1.0):
//   • Any mismatch or CRC reject clears the entire FIFO rather than trying
//     to advance by one. A single desync makes every subsequent comparison
//     wrong; a full clear is the only safe recovery.
//   • TX and RX use separate buffers (sendBuf_ / recvBuf_) so a recv can
//     never overwrite a payload whose CRC hasn't been recorded yet.
//   • On link-up, the receive buffer is fully drained before any new sends,
//     so stale echoes from the previous session never corrupt the fresh FIFO.
// ----------------------------------------------------------------------------
class UtilPing : public UtilMain {
public:
    UtilPing(uint32_t    debugBaud,
             uart_port_t uartNum,
             int         rxPin,
             int         txPin,
             const char* ssid     = nullptr,
             const char* password = nullptr,
             uint16_t    webPort  = 8765)
        : UtilMain(debugBaud, uartNum, rxPin, txPin, /*isPing=*/true,
                   ssid, password, webPort)
    {}

    UtilPing(const UtilPing&)            = delete;
    UtilPing& operator=(const UtilPing&) = delete;

    void setup() {
        log_.debug("Ping", "setup: seeding RNG, calling setupCommon");
        randomSeed(esp_random());
        setupCommon();
        log_.debug("Ping", "setup complete  WINDOW=%d  BUF_SIZE=%d", WINDOW, BUF_SIZE);
    }

    void loop() {
        if (!comm_.ready()) {
            uint32_t now = millis();
            if (wasReady_) {
                log_.debug("Ping", "link lost  pendCount was %d  seq=%lu",
                    pendCount_, (unsigned long)msgSeq_);
                wasReady_ = false;
                resetFifo_("link drop");
                tSweepStall_ = now;   // start sweep watchdog
            } else {
                // Sweep stall watchdog: if the SWP timer stops firing (FreeRTOS
                // timer queue overflow after error-threshold drop), sweep hangs
                // silently. Detect it and call dropLink() to send a BREAK and
                // restart the sweep cleanly from the protocol layer.
                if (now - tSweepStall_ > SWEEP_STALL_MS) {
                    log_.error("Ping",
                        "SWP stall — no sweep progress for %lu ms, forcing BREAK to restart",
                        (unsigned long)(now - tSweepStall_));
                    comm_.dropLink();
                    tSweepStall_ = now;
                }
                if (now - tNotReady_ >= 1000) {
                    log_.debug("Ping", "not ready  swpAge=%lu ms",
                        (unsigned long)(now - tSweepStall_));
                    tNotReady_ = now;
                }
            }
            comm_.blinkWait(3, 100, 100, 0);
            return;
        }
        if (!wasReady_) {
            log_.debug("Ping", "link up  baud=%lu  seq=%lu  settling %lu ms",
                (unsigned long)comm_.getCurrentBaud(), (unsigned long)msgSeq_,
                (unsigned long)SETTLE_MS);
            // Drain ALL stale echoes before sending anything new.
            // Echoes from the previous session would desync the fresh FIFO.
            int drained = 0;
            while (comm_.recv(recvBuf_, sizeof recvBuf_) > 0) drained++;
            if (drained) log_.debug("Ping", "drained %d stale echo(s)", drained);
            comm_.blinkWait(4);
            tReady_ = millis();
            wasReady_ = true;
        }

        // Settle guard — give Pong's sweep time to complete its own lock.
        if (millis() - tReady_ < SETTLE_MS) {
            log_.debug("Ping", "settling  %lu ms remaining",
                (unsigned long)(SETTLE_MS - (millis() - tReady_)));
            return;
        }

        // Pipeline stall detection — if the window is full and nothing drains
        // for STALL_MS, clear the FIFO and let new sends proceed.
        uint32_t now = millis();
        if (pendCount_ == WINDOW) {
            if (tStall_ == 0) tStall_ = now;
            if (now - tStall_ > STALL_MS) {
                log_.error("Ping",
                    "pipeline stall — WINDOW=%d full for %lu ms, no echoes. "
                    "Clearing FIFO. seq=%lu  head=%d tail=%d",
                    WINDOW, (unsigned long)(now - tStall_),
                    (unsigned long)msgSeq_, pendHead_, pendTail_);
                resetFifo_("stall");
            }
        } else {
            tStall_ = 0;
        }

        // Fill the send window, but pace it: emitting the whole window in one
        // loop() dumps up to WINDOW KB-sized frames back-to-back, overrunning
        // Pong's RX (partial writes + COBS desync). Cap per-loop sends so the
        // pipeline fills over a few ticks instead of one burst.
        int sentThisLoop = 0;
        while (pendCount_ < WINDOW && sentThisLoop < MAX_TX_PER_LOOP) {
            int n = random(1, 1024);
            fill_(sendBuf_, n);
            if (!comm_.send(sendBuf_, n)) {
                log_.debug("Ping",
                    "send failed (link dropped)  n=%d  pendCount=%d", n, pendCount_);
                break;
            }
            pend_[pendTail_].len = n;
            pend_[pendTail_].crc = UtilCrc::crc16(sendBuf_, n);
            pend_[pendTail_].seq = msgSeq_++;
            pendTail_ = (pendTail_ + 1) % WINDOW;
            pendCount_++;
            sentThisLoop++;
        }
        if (sentThisLoop > 0) {
            log_.debug("Ping", "sent %d msgs  pendCount=%d  seq=%lu",
                sentThisLoop, pendCount_, (unsigned long)msgSeq_);
        }

        // Drain available echoes and verify each against the oldest pending slot.
        int got;
        while ((got = comm_.recv(recvBuf_, sizeof recvBuf_)) > 0) {
            if (pendCount_ == 0) {
                log_.error("Ping",
                    "recv %d bytes with no in-flight send (stale echo?) — discarding",
                    got);
                continue;
            }
            comm_.blinkWait(1);
            Pending& p = pend_[pendHead_];

            if (got != p.len) {
                log_.error("Ping",
                    "MISMATCH seq=%lu  sent=%d bytes  echoed=%d bytes  "
                    "pendCount=%d — clearing FIFO (desync)",
                    (unsigned long)p.seq, p.len, got, pendCount_);
                resetFifo_("length mismatch");
                break;   // don't process further echoes against a cleared FIFO
            } else if (UtilCrc::crc16(recvBuf_, got) != p.crc) {
                log_.error("Ping",
                    "MISMATCH seq=%lu  %d bytes  CRC differs  pendCount=%d "
                    "— clearing FIFO (desync)",
                    (unsigned long)p.seq, got, pendCount_);
                resetFifo_("CRC mismatch");
                break;
            } else {
                log_.debug("Ping", "echo ok seq=%lu  %d bytes",
                           (unsigned long)p.seq, got);
                pendHead_ = (pendHead_ + 1) % WINDOW;
                pendCount_--;
            }
        }
        if (got < 0) {
            // Link-layer CRC/desync reject — clear the whole FIFO.
            // A single reject doesn't map to one echo reliably.
            log_.error("Ping",
                "recv rejected (CRC/desync)  pendCount=%d head=%d tail=%d "
                "— clearing FIFO",
                pendCount_, pendHead_, pendTail_);
            resetFifo_("recv reject");
        }

        logStats("Ping");
    }

private:
    static void fill_(uint8_t* b, int n) {
        for (int i = 0; i < n; i++) b[i] = (uint8_t)random(256);
    }

    // Clear the in-flight FIFO. Called on any desync event so that the next
    // batch of sends starts with a clean slate. Does NOT reset msgSeq_ — the
    // sequence counter is monotonic for diagnostic purposes.
    void resetFifo_(const char* reason) {
        if (pendCount_ > 0) {
            log_.debug("Ping", "FIFO cleared (%s)  dropped=%d  seq was=%lu",
                reason, pendCount_, (unsigned long)msgSeq_);
        }
        pendHead_ = pendTail_ = pendCount_ = 0;
        tStall_ = 0;
        // Discard stale echoes from the ALink receive buffer. Without this,
        // the old echoes (no longer matched by the FIFO we just reset) remain
        // in the stream and desync recvMsg for every subsequent recv:
        // recvMsg reads a stale header, CRC mismatches the new in-flight seq,
        // and repeats forever -- onPayload() resets the consecutive error
        // counter on each valid COBS frame so errThreshold is never reached.
        comm_.flushRx();
    }

    // ── pipeline state ────────────────────────────────────────────────────
    static constexpr int      WINDOW    = 8;     // messages in flight at once
    static constexpr int      MAX_TX_PER_LOOP = 2;  // frames emitted per loop() (paced, no bursts)
    static constexpr uint32_t STALL_MS      = 3000;  // ms full window with no drain
    static constexpr uint32_t SWEEP_STALL_MS = 2000;  // ms stuck in SWP before forcing BREAK
    static constexpr uint32_t SETTLE_MS = 300;   // ms after link-up before sending

    struct Pending { int len; uint16_t crc; uint32_t seq; };
    Pending  pend_[WINDOW];

    int      pendHead_  = 0;
    int      pendTail_  = 0;
    int      pendCount_ = 0;
    uint32_t msgSeq_    = 0;
    uint32_t tStall_      = 0;
    uint32_t tReady_      = 0;
    uint32_t tSweepStall_ = 0;   // millis() when SWP watchdog started
    uint32_t tNotReady_   = 0;   // millis() of last not-ready/settling log (rate limiter)

    // Separate TX/RX buffers so recv() can never overwrite a payload whose
    // CRC is still pending comparison.
    uint8_t sendBuf_[BUF_SIZE];
    uint8_t recvBuf_[BUF_SIZE];
};

} // namespace autolink
#endif // ARDUINO
