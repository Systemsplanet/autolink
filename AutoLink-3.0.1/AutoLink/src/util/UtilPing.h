// UtilPing.h — ready-to-run AutoLink master for the ping-pong echo test.
//
// Wraps AutoLink + AutoLinkWeb + the full send/compare/stats loop from the
// README Quick Start into a single setup()/loop() object. Drop the header
// into a sketch and wire three calls — that's the entire application.
//
// ┌──────────── WIRING ─────────────────────────────────────────────────────┐
// │ Cross-connect the two boards:  Master TX(GPIO17) ──► Slave RX(GPIO16)  │
// │                                Master RX(GPIO16) ◄── Slave TX(GPIO17)  │
// │                                shared GND                              │
// │ (TX→TX or RX→RX are the most common wiring mistakes and produce        │
// │  0 received bytes at every baud. A missing GND does the same.)         │
// │                                                                         │
// │ Default pins: rxPin=16, txPin=17 (ESP32 UART2 defaults).               │
// │ On the FireBeetle ESP32 these are header pins D11 (GPIO16) and          │
// │ D10 (GPIO17). Any free GPIOs work — pass different pins if needed.     │
// └─────────────────────────────────────────────────────────────────────────┘
//
// The WiFi web monitor is optional: pass a non-null SSID to enable it.
// If the SSID is omitted (or nullptr), the UART link runs unaffected and
// the AutoLinkWeb object is constructed but never started.
//
// Usage:
//   UtilPing ping(115200, UART_NUM_2, 16, 17);           // UART only
//   UtilPing ping(115200, UART_NUM_2, 16, 17,            // + web monitor
//                 "YourSSID", "password", 80);
//   void setup() { ping.setup(); }
//   void loop()  { ping.loop();  }
#pragma once
#ifdef ARDUINO

#include "../AutoLink.h"
#include "../AutoLinkWeb.h"
#include "UtilCrc.h"
#include <Arduino.h>
#include <string.h>

namespace autolink {

// ----------------------------------------------------------------------------
// UtilPing — plug-and-play AutoLink master.
//
// Sends random-length messages (1–1023 bytes), waits for the slave to echo
// each one back, compares byte-for-byte, and logs throughput every 5 s.
// Mismatches are logged as [E] MISMATCH lines (visible in serial and on the
// web dashboard). Reconnects after any link disruption automatically.
// ----------------------------------------------------------------------------
class UtilPing {
public:
    UtilPing(uint32_t    debugBaud,
               uart_port_t uartNum,
               int         rxPin,
               int         txPin,
               const char* ssid     = nullptr,
               const char* password = nullptr,
               uint16_t    webPort  = 8765)
        : debugBaud_(debugBaud)
        , comm_(uartNum, rxPin, txPin, /*isMaster=*/true)
        , mon_(comm_)
        , ssid_(ssid)
        , password_(password ? password : "")
        , webPort_(webPort)
        , log_(Log::getLog())
    {}

    // Non-copyable — AutoLink and AutoLinkWeb own hardware resources.
    UtilPing(const UtilPing&)            = delete;
    UtilPing& operator=(const UtilPing&) = delete;

    void setup() {
        esp_log_level_set("*", ESP_LOG_VERBOSE);
        log_.setLevel(Log::DEBUG);
        Serial.begin(debugBaud_);
        randomSeed(esp_random());
        comm_.blinkWait(1, 100, 100, 2000);
        comm_.begin();                                   // starts baud sweep
        if (ssid_) mon_.begin(ssid_, password_, webPort_);
        comm_.blinkWait(2, 100, 100, 2000);
    }

    void loop() {
        if (!comm_.ready()) {
            log_.debug("Main", "not ready");
            comm_.blinkWait(3, 100, 100, 2000);
            wasReady_ = false;
            pendHead_ = pendTail_ = pendCount_ = 0;  // link drop voids in-flight compares
            return;
        }
        if (!wasReady_) {
            log_.debug("Main", "ready");
            drainAndCompare_();   // drain anything queued during the gap
            comm_.blinkWait(4);
            wasReady_ = true;
        }

        // Pipeline: keep up to WINDOW messages in flight so both the TX and RX
        // directions stay busy. A strict one-at-a-time round-trip leaves each
        // direction idle while the other transmits, capping throughput at ~half
        // the line rate. Filling a window lets the slave's echoes flow back while
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

        // Serial throughput + error log every 5 s. Same stats on the dashboard.
        uint32_t now = millis();
        if (now - tStat_ > 5000) {
            uint64_t tx, rx, errs;
            comm_.getStats(tx, rx, errs);
            uint32_t dt = now - tStat_;
            uint64_t txRate = dt ? ((tx - lastTx_) * 1000ULL / dt) : 0;
            uint64_t rxRate = dt ? ((rx - lastRx_) * 1000ULL / dt) : 0;
            log_.info("Ping",
                "tx=%llu B/sec  rx=%llu B/sec  baud=%lu  disc=%llu  errs=%llu",
                txRate, rxRate, (unsigned long)comm_.getCurrentBaud(), errs,
                (unsigned long long)comm_.getLifetimeErrors());
            lastTx_ = tx;
            lastRx_ = rx;
            tStat_ = now;
        }
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
                log_.error("Main",
                    "recv %d bytes with no in-flight send (stale echo?)", got);
                continue;
            }
            Pending& p = pend_[pendHead_];
            comm_.blinkWait(1);   // one flash per verified echo

            if (got != p.len) {
                log_.error("Main",
                    "MISMATCH seq=%lu  sent=%d bytes  echoed=%d bytes",
                    (unsigned long)p.seq, p.len, got);
            } else if (UtilCrc::crc16(buf_, got) != p.crc) {
                log_.error("Main",
                    "MISMATCH seq=%lu  %d bytes, CRC differs (corruption in echo)",
                    (unsigned long)p.seq, got);
            } else {
                log_.debug("Main", "echo ok seq=%lu  %d bytes",
                           (unsigned long)p.seq, got);
            }
            pendHead_ = (pendHead_ + 1) % WINDOW;
            pendCount_--;
        }
        if (got < 0) {
            // CRC reject — bad message drained and counted. Drop the oldest
            // pending entry so the FIFO stays aligned with the echo stream.
            log_.error("Main", "recv rejected (CRC/desync)");
            if (pendCount_ > 0) {
                pendHead_ = (pendHead_ + 1) % WINDOW;
                pendCount_--;
            }
        }
    }

    // ── config ────────────────────────────────────────────────────────────
    uint32_t    debugBaud_;
    AutoLink    comm_;
    AutoLinkWeb mon_;
    const char* ssid_;
    const char* password_;
    uint16_t    webPort_;
    Log&        log_;

    // ── pipelined send state ────────────────────────────────────────────────
    static constexpr int WINDOW = 8;   // max messages in flight at once
    struct Pending { int len; uint16_t crc; uint32_t seq; };
    uint8_t  buf_[1024];       // shared RX / TX scratch buffer
    Pending  pend_[WINDOW];    // FIFO ring of in-flight sends (len + crc + seq)
    int      pendHead_  = 0;   // index of oldest pending entry
    int      pendTail_  = 0;   // next free slot
    int      pendCount_ = 0;   // number of entries currently in flight
    bool     wasReady_  = false;
    uint32_t msgSeq_    = 0;    // monotonically increasing send counter
    uint32_t tStat_     = 0;    // last time throughput was logged
    uint64_t lastTx_    = 0;    // tx byte total at last stat log
    uint64_t lastRx_    = 0;    // rx byte total at last stat log
};

} // namespace autolink
#endif // ARDUINO
