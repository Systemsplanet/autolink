// UtilMain.h — shared base for UtilPing and UtilPong.
//
// Owns the hardware objects (AutoLink + AutoLinkWeb), the shared receive/
// transmit scratch buffer, and the periodic stats logger. Both Ping and Pong
// inherit from this class; only their loop() bodies differ.
//
// ┌──────────── WIRING ─────────────────────────────────────────────────────┐
// │ Cross-connect the two boards:  Ping TX(GPIO17) ──► Pong RX(GPIO16)     │
// │                                Ping RX(GPIO16) ◄── Pong TX(GPIO17)     │
// │                                shared GND                              │
// │ (TX→TX or RX→RX are the most common wiring mistakes and produce        │
// │  0 received bytes at every baud. A missing GND does the same.)         │
// │                                                                         │
// │ Default pins: rxPin=16, txPin=17 (ESP32 UART2 defaults).               │
// │ On the FireBeetle ESP32 these are header pins D11 (GPIO16) and          │
// │ D10 (GPIO17). Any free GPIOs work — pass different pins if needed.     │
// └─────────────────────────────────────────────────────────────────────────┘
#pragma once
#ifdef ARDUINO

#include "../AutoLink.h"
#include "../AutoLinkWeb.h"
#include <Arduino.h>

namespace autolink {

// ----------------------------------------------------------------------------
// UtilMain — base class for UtilPing and UtilPong.
//
// Provides:
//   • AutoLink + AutoLinkWeb construction and setup()
//   • 1 KB shared scratch buffer (buf_ / BUF_SIZE)
//   • logStats(tag) — call once per loop() to emit the 5-second throughput line
//   • wasReady_ flag for transition-on-ready detection
// ----------------------------------------------------------------------------
class UtilMain {
public:
    static constexpr int BUF_SIZE = 1024;

    UtilMain(uint32_t    debugBaud,
             uart_port_t uartNum,
             int         rxPin,
             int         txPin,
             bool        isPing,
             const char* ssid     = nullptr,
             const char* password = nullptr,
             uint16_t    webPort  = 8765)
        : debugBaud_(debugBaud)
        , comm_(uartNum, rxPin, txPin, isPing)
        , mon_(comm_)
        , ssid_(ssid)
        , password_(password ? password : "")
        , webPort_(webPort)
        , log_(Log::getLog())
    {}

    // Non-copyable — AutoLink and AutoLinkWeb own hardware resources.
    UtilMain(const UtilMain&)            = delete;
    UtilMain& operator=(const UtilMain&) = delete;

protected:
    // Call from the subclass setup(). blinkCount lets Ping and Pong use
    // distinct blink patterns if desired (Ping uses 1/2, Pong uses 1/2 too,
    // so the argument is retained for flexibility).
    void setupCommon() {
        esp_log_level_set("*", ESP_LOG_VERBOSE);
        log_.setLevel(Log::DEBUG);
        Serial.begin(debugBaud_);
        log_.debug("UtilMain", "setupCommon: debug baud=%lu  role=%s  WiFi=%s",
            (unsigned long)debugBaud_,
            ssid_ ? "Ping+Web" : "Ping",   // overridden by subclass tag in practice
            ssid_ ? ssid_ : "disabled");
        comm_.blinkWait(1, 100, 100, 2000);
        log_.debug("UtilMain", "calling comm_.begin()");
        comm_.begin();
        if (ssid_) {
            log_.debug("UtilMain", "starting web monitor (port %u)", (unsigned)webPort_);
            mon_.begin(ssid_, password_, webPort_);
        } else {
            log_.debug("UtilMain", "WiFi disabled — skipping web monitor");
        }
        // Log the version here — after mon_.begin() so the sink is registered
        // and the line appears in the web log panel (after NTP sync line).
        log_.info("AutoLink", "v" AUTOLINK_VERSION);
        comm_.blinkWait(2, 100, 100, 2000);
        log_.debug("UtilMain", "setupCommon complete");
    }

    // Emit a throughput line at most once every 5 seconds.
    // tag should be "Ping" or "Pong" — appears as the log source label.
    void logStats(const char* tag) {
        uint32_t now = millis();
        if (now - tStat_ < 5000) return;
        Stats s;
        comm_.getStats(s);
        uint32_t dt     = now - tStat_;
        uint64_t txRate = dt ? ((s.tx - lastTx_) * 1000ULL / dt) : 0;
        uint64_t rxRate = dt ? ((s.rx - lastRx_) * 1000ULL / dt) : 0;
        log_.info(tag,
            "tx=%llu B/sec  rx=%llu B/sec  baud=%lu  disc=%llu  errs=%llu",
            txRate, rxRate, (unsigned long)comm_.getCurrentBaud(),
            (unsigned long long)s.discCount,
            (unsigned long long)s.frameErrs);
        lastTx_ = s.tx;
        lastRx_ = s.rx;
        tStat_  = now;
    }

    // ── hardware objects ──────────────────────────────────────────────────
    uint32_t    debugBaud_;
    AutoLink    comm_;
    AutoLinkWeb mon_;
    const char* ssid_;
    const char* password_;
    uint16_t    webPort_;
    Log&        log_;

    // ── shared state ──────────────────────────────────────────────────────
    uint8_t  buf_[BUF_SIZE];   // shared RX / TX scratch buffer
    bool     wasReady_ = false;

private:
    uint32_t tStat_  = 0;   // millis() at last stat log
    uint64_t lastTx_ = 0;   // tx byte total at last stat log
    uint64_t lastRx_ = 0;   // rx byte total at last stat log
};

} // namespace autolink
#endif // ARDUINO
