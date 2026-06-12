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
// Sends random-length messages (1–1023 bytes), waits for Pong to echo each
// one back, compares byte-for-byte, and logs throughput every 5 s.
// Mismatches are logged as [E] MISMATCH lines (visible in serial and on the
// web dashboard). Reconnects after any link disruption automatically.
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

    // Non-copyable — hardware resources are owned by the base.
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
            if (wasReady_) {
                // Log once on transition, not every loop iteration
                log_.debug("Ping", "link lost  pendCount was %d  seq=%lu",
                    pendCount_, (unsigned long)msgSeq_);
            } else {
                log_.debug("Ping", "not ready");
            }
            comm_.blinkWait(3, 100, 100, 2000);
            wasReady_ = false;
            pendHead_ = pendTail_ = pendCount_ = 0;  // link drop voids in-flight compares
            return;
        }
        if (!wasReady_) {
            log_.debug("Ping", "link up  baud=%lu  seq=%lu",
                (unsigned long)comm_.getCurrentBaud(), (unsigned long)msgSeq_);
            drainAndCompare_();   // drain anything queued during the gap
            comm_.blinkWait(4);
            wasReady_ = true;
        }

        // Pipeline: keep up to WINDOW messages in flight so both TX and RX
        // directions stay busy. A strict one-at-a-time round-trip leaves each
        // direction idle while the other transmits, capping throughput at ~half
        // the line rate. Filling a window lets Pong's echoes flow back while
        // we're still sending, roughly doubling effective throughput.
        //
        // Stall detection: if the pipeline is full and no echoes have arrived
        // for STALL_MS, the FIFO is desynchronised and Ping has gone silent.
        // Reset pendCount so sending resumes; the link itself stays up.
        uint32_t now = millis();
        if (pendCount_ == WINDOW) {
            if (tStall_ == 0) tStall_ = now;   // start the stall clock
            if (now - tStall_ > STALL_MS) {
                log_.error("Ping",
                    "pipeline stall — WINDOW=%d full for %lu ms, no echoes."
                    "  Resetting FIFO. seq=%lu  head=%d tail=%d",
                    WINDOW, (unsigned long)(now - tStall_),
                    (unsigned long)msgSeq_, pendHead_, pendTail_);
                pendHead_ = pendTail_ = pendCount_ = 0;
                tStall_ = 0;
            }
        } else {
            tStall_ = 0;   // echoes are flowing — reset the stall clock
        }

        int sentThisLoop = 0;
        while (pendCount_ < WINDOW) {
            int n = random(1, 1024);
            fill_(buf_, n);
            if (!comm_.send(buf_, n)) {
                log_.debug("Ping",
                    "send failed (link busy/dropped)  n=%d  pendCount=%d", n, pendCount_);
                break;
            }
            pend_[pendTail_].len = n;
            pend_[pendTail_].crc = UtilCrc::crc16(buf_, n);
            pend_[pendTail_].seq = msgSeq_++;
            pendTail_ = (pendTail_ + 1) % WINDOW;
            pendCount_++;
            sentThisLoop++;
        }
        if (sentThisLoop > 0) {
            log_.debug("Ping", "sent %d msgs  pendCount=%d  seq=%lu",
                sentThisLoop, pendCount_, (unsigned long)msgSeq_);
        }

        drainAndCompare_();
        logStats("Ping");
    }

private:
    // Fill a buffer with random bytes.
    static void fill_(uint8_t* b, int n) {
        for (int i = 0; i < n; i++) b[i] = (uint8_t)random(256);
    }

    // Drain all complete echoes and verify each against the OLDEST pending send
    // (FIFO). Echoes come back in send order, so comparing length + CRC-16 to
    // the head of the pending ring is an unambiguous, byte-equivalent check
    // without needing to stash every full payload.
    void drainAndCompare_() {
        int got;
        while ((got = comm_.recv(buf_, sizeof buf_)) > 0) {
            if (pendCount_ == 0) {
                log_.error("Ping",
                    "recv %d bytes with no in-flight send (stale echo?)", got);
                continue;
            }
            Pending& p = pend_[pendHead_];
            comm_.blinkWait(1);   // one flash per verified echo

            if (got != p.len) {
                log_.error("Ping",
                    "MISMATCH seq=%lu  sent=%d bytes  echoed=%d bytes  "
                    "pendCount=%d (length mismatch often = FIFO desync, not corruption)",
                    (unsigned long)p.seq, p.len, got, pendCount_);
            } else if (UtilCrc::crc16(buf_, got) != p.crc) {
                log_.error("Ping",
                    "MISMATCH seq=%lu  %d bytes, CRC differs  pendCount=%d",
                    (unsigned long)p.seq, got, pendCount_);
            } else {
                log_.debug("Ping", "echo ok seq=%lu  %d bytes",
                           (unsigned long)p.seq, got);
            }
            pendHead_ = (pendHead_ + 1) % WINDOW;
            pendCount_--;
        }
        if (got < 0) {
            // CRC reject — bad message drained and counted. Drop the oldest
            // pending entry so the FIFO stays aligned with the echo stream.
            //
            // DIAGNOSTIC (v3.0.9): a single reject does NOT reliably map to
            // one echo — the link layer may have dropped, merged, or split
            // frames. Dropping exactly one pending entry here can leave the
            // FIFO misaligned with the real echo stream, which then makes
            // every following good echo compare against the wrong entry and
            // count as a fresh error. Log the FIFO depth so the cascade is
            // visible: a burst of rejects with shrinking pendCount is the
            // desync signature.
            log_.error("Ping",
                "recv rejected (CRC/desync)  pendCount=%d head=%d tail=%d",
                pendCount_, pendHead_, pendTail_);
            if (pendCount_ > 0) {
                pendHead_ = (pendHead_ + 1) % WINDOW;
                pendCount_--;
            }
        }
    }

    // ── pipelined send state ──────────────────────────────────────────────
    static constexpr int      WINDOW   = 8;      // max messages in flight at once
    static constexpr uint32_t STALL_MS = 3000;   // ms of no echoes before FIFO reset
    struct Pending { int len; uint16_t crc; uint32_t seq; };
    Pending  pend_[WINDOW];    // FIFO ring of in-flight sends (len + crc + seq)
    int      pendHead_  = 0;   // index of oldest pending entry
    int      pendTail_  = 0;   // next free slot
    int      pendCount_ = 0;   // number of entries currently in flight
    uint32_t msgSeq_    = 0;   // monotonically increasing send counter
    uint32_t tStall_    = 0;   // millis() when pipeline first went full with no drain
};

} // namespace autolink
#endif // ARDUINO
