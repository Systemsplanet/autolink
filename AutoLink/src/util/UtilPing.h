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
        randomSeed(esp_random());
        setupCommon();
    }

    void loop() {
        if (!comm_.ready()) {
            log_.debug("Ping", "not ready");
            comm_.blinkWait(3, 100, 100, 2000);
            wasReady_ = false;
            pendHead_ = pendTail_ = pendCount_ = 0;  // link drop voids in-flight compares
            return;
        }
        if (!wasReady_) {
            log_.debug("Ping", "ready");
            drainAndCompare_();   // drain anything queued during the gap
            comm_.blinkWait(4);
            wasReady_ = true;
        }

        // Pipeline: keep up to WINDOW messages in flight so both TX and RX
        // directions stay busy. A strict one-at-a-time round-trip leaves each
        // direction idle while the other transmits, capping throughput at ~half
        // the line rate. Filling a window lets Pong's echoes flow back while
        // we're still sending, roughly doubling effective throughput.
        while (pendCount_ < WINDOW) {
            int n = random(1, 1024);
            fill_(buf_, n);
            if (!comm_.send(buf_, n)) break;   // link busy/dropped — try again next loop
            pend_[pendTail_].len = n;
            pend_[pendTail_].crc = UtilCrc::crc16(buf_, n);
            pend_[pendTail_].seq = msgSeq_++;
            pendTail_ = (pendTail_ + 1) % WINDOW;
            pendCount_++;
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
                    "MISMATCH seq=%lu  sent=%d bytes  echoed=%d bytes",
                    (unsigned long)p.seq, p.len, got);
            } else if (UtilCrc::crc16(buf_, got) != p.crc) {
                log_.error("Ping",
                    "MISMATCH seq=%lu  %d bytes, CRC differs (corruption in echo)",
                    (unsigned long)p.seq, got);
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
            log_.error("Ping", "recv rejected (CRC/desync)");
            if (pendCount_ > 0) {
                pendHead_ = (pendHead_ + 1) % WINDOW;
                pendCount_--;
            }
        }
    }

    // ── pipelined send state ──────────────────────────────────────────────
    static constexpr int WINDOW = 8;   // max messages in flight at once
    struct Pending { int len; uint16_t crc; uint32_t seq; };
    Pending  pend_[WINDOW];    // FIFO ring of in-flight sends (len + crc + seq)
    int      pendHead_  = 0;   // index of oldest pending entry
    int      pendTail_  = 0;   // next free slot
    int      pendCount_ = 0;   // number of entries currently in flight
    uint32_t msgSeq_    = 0;   // monotonically increasing send counter
};

} // namespace autolink
#endif // ARDUINO
