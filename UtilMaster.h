// UtilMaster.h — ready-to-run AutoLink master for the ping-pong echo test.
//
// Wraps AutoLink + AutoLinkWeb + the full send/compare/stats loop from the
// README Quick Start into a single setup()/loop() object. Drop the header
// into a sketch and wire three calls — that's the entire application.
//
// ┌──────────── WIRING ─────────────────────────────────────────────────────┐
// │ Cross-connect the two boards:  Master TX ──► Slave RX                  │
// │                                Master RX ◄── Slave TX                  │
// │ (TX→TX or RX→RX are the most common wiring mistakes and produce        │
// │  0 received bytes at every baud.)                                       │
// │                                                                         │
// │ Default pins: rxPin=16, txPin=17 (ESP32 UART2 defaults).               │
// │ FireBeetle ESP32: GPIO 16/17 are NOT on the header.                    │
// │   Use GPIO18/19  →  UtilMaster um(115200, UART_NUM_2, 18, 19, ...);   │
// │   Use GPIO21/22  →  UtilMaster um(115200, UART_NUM_2, 21, 22, ...);   │
// └─────────────────────────────────────────────────────────────────────────┘
//
// The WiFi web monitor is optional: pass a non-null SSID to enable it.
// If the SSID is omitted (or nullptr), the UART link runs unaffected and
// the AutoLinkWeb object is constructed but never started.
//
// Usage:
//   UtilMaster um(115200, UART_NUM_2, 16, 17);           // UART only
//   UtilMaster um(115200, UART_NUM_2, 16, 17,            // + web monitor
//                 "YourSSID", "password", 80);
//   void setup() { um.setup(); }
//   void loop()  { um.loop();  }
#pragma once
#ifdef ARDUINO

#include "AutoLink.h"
#include "AutoLinkWeb.h"
#include <Arduino.h>
#include <string.h>

namespace autolink {

// ----------------------------------------------------------------------------
// UtilMaster — plug-and-play AutoLink master.
//
// Sends random-length messages (1–1023 bytes), waits for the slave to echo
// each one back, compares byte-for-byte, and logs throughput every 5 s.
// Mismatches are logged as [E] MISMATCH lines (visible in serial and on the
// web dashboard). Reconnects after any link disruption automatically.
// ----------------------------------------------------------------------------
class UtilMaster {
public:
    UtilMaster(uint32_t    debugBaud,
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
    UtilMaster(const UtilMaster&)            = delete;
    UtilMaster& operator=(const UtilMaster&) = delete;

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
            sentLen_  = 0;   // link drop invalidates any in-flight compare
            return;
        }
        if (!wasReady_) {
            log_.debug("Main", "ready");
            // Drain anything the slave queued during the gap before sending.
            drainAndCompare_();
            comm_.blinkWait(4);
            wasReady_ = true;
        }

        // Keep one message in flight at a time (simple round-trip).
        // sentLen_ == 0 means the previous echo has been received and matched.
        if (sentLen_ == 0) {
            int n = random(1, 1024);
            fill_(buf_, n);
            if (comm_.send(buf_, n)) {
                sentLen_ = n;
                sentSeq_ = msgSeq_++;
                memcpy(sent_, buf_, n);
                log_.debug("Main", "sent %d bytes seq=%lu",
                           n, (unsigned long)sentSeq_);
            } else {
                log_.error("Main", "send failed (link dropped mid-send)");
            }
        }

        drainAndCompare_();

        // Serial throughput log every 5 s.
        // The same stats are visible live on the AutoLinkWeb dashboard.
        if (millis() - tStat_ > 5000) {
            uint64_t tx, rx, errs;
            comm_.getStats(tx, rx, errs);
            log_.info("Main", "tx=%llu B  rx=%llu B  baud=%lu  disconnects=%llu",
                tx, rx, (unsigned long)comm_.getCurrentBaud(), errs);
            tStat_ = millis();
        }
    }

private:
    // Fill a buffer with random bytes.
    static void fill_(uint8_t* b, int n) {
        for (int i = 0; i < n; i++) b[i] = (uint8_t)random(256);
    }

    // Drain all complete echoes from the slave and compare each to the
    // last sent payload. One message in flight at a time makes the compare
    // unambiguous — the first echo is always the reply to the last send.
    void drainAndCompare_() {
        int got;
        while ((got = comm_.recv(buf_, sizeof buf_)) > 0) {
            if (sentLen_ == 0) {
                log_.error("Main",
                    "recv %d bytes with no in-flight send (stale echo?)", got);
                continue;
            }
            log_.debug("Main", "recv %d bytes  sentSeq=%lu",
                       got, (unsigned long)sentSeq_);
            comm_.blinkWait(1);   // mirror the slave's per-echo blink

            if (got != sentLen_) {
                log_.error("Main",
                    "MISMATCH sentSeq=%lu  sent=%d bytes  echoed=%d bytes",
                    (unsigned long)sentSeq_, sentLen_, got);
            } else if (memcmp(buf_, sent_, (size_t)got) != 0) {
                int firstBad = -1;
                for (int i = 0; i < got; i++) {
                    if (buf_[i] != sent_[i]) { firstBad = i; break; }
                }
                log_.error("Main",
                    "MISMATCH seq=%lu %d bytes differ, first bad offset=%d "
                    "expected 0x%02X got 0x%02X",
                    (unsigned long)sentSeq_, got, firstBad,
                    sent_[firstBad >= 0 ? firstBad : 0],
                    buf_ [firstBad >= 0 ? firstBad : 0]);
            }
            sentLen_ = 0;
        }
        // got == -1: CRC reject — bad message drained and counted.
        // Clear sentLen_ so a fresh send clears the stale in-flight state.
        if (got < 0) {
            log_.error("Main", "recv rejected (CRC/desync) sentSeq=%lu",
                       (unsigned long)sentSeq_);
            sentLen_ = 0;
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

    // ── per-send state ────────────────────────────────────────────────────
    uint8_t  buf_[1024];       // shared RX / TX scratch buffer
    uint8_t  sent_[1024];      // stash of the last sent payload for comparison
    int      sentLen_  = 0;    // length of the stashed payload; 0 = nothing in flight
    uint32_t sentSeq_  = 0;    // sequence number of the stashed payload
    bool     wasReady_ = false;
    uint32_t msgSeq_   = 0;    // monotonically increasing send counter
    uint32_t tStat_    = 0;    // last time throughput was logged
};

} // namespace autolink
#endif // ARDUINO
