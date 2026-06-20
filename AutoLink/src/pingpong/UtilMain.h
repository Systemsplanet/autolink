// UtilMain — shared base for UtilPing and UtilPong. Owns the hardware
// objects (AutoLink + AutoLinkWeb), the shared scratch buffer, and the
// periodic stats logger. Only the loop() body differs between Ping
// and Pong.
//
// Wiring: cross-connect the two boards.
//   Ping TX(GPIO17) ──► Pong RX(GPIO16)
//   Ping RX(GPIO16) ◄── Pong TX(GPIO17)
//   shared GND
// TX→TX or RX→RX (and missing GND) both produce 0 received bytes.
#pragma once
#ifdef ARDUINO

#include "../AutoLink.h"
#include "../AutoLinkWeb.h"
#include <Arduino.h>

namespace autolink {

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
        , isPing_(isPing)
        , comm_(uartNum, rxPin, txPin, isPing)
        , mon_(comm_)
        , ssid_(ssid)
        , password_(password ? password : "")
        , webPort_(webPort)
        , log_(Log::getLog())
    {}

    UtilMain(const UtilMain&)            = delete;
    UtilMain& operator=(const UtilMain&) = delete;

protected:
    // Subclass setup() calls this. Default log level is DEBUG so a
    // fresh board shows full per-loop / per-frame chatter — the
    // operator can see exactly what the protocol is doing on first
    // boot. Switch to INFO from the dashboard's radio if the log is
    // too noisy. AutoLinkWeb::begin() restores any NVS-saved level
    // before this runs.
    void setupCommon() {
        esp_log_level_set("*", ESP_LOG_VERBOSE);
        log_.setLevel(Log::DEBUG);
        Serial.begin(debugBaud_);
        // Boot banner at INFO so a Serial monitor that opened AFTER
        // reset still sees a clear "I'm alive" line — without this,
        // the first visible line is whatever the next event is
        // (UART ready, link up, etc.) and a hung boot shows nothing.
        log_.info("UtilMain", "boot: role=%s  baud=%lu  WiFi=%s",
            isPing_ ? "Ping" : "Pong",
            (unsigned long)debugBaud_,
            ssid_ ? ssid_ : "disabled");
        log_.debug("UtilMain", "setupCommon: debug baud=%lu  role=%s  WiFi=%s",
            (unsigned long)debugBaud_,
            ssid_ ? (isPing_ ? "Ping+Web" : "Pong+Web")
                  : (isPing_ ? "Ping"     : "Pong"),
            ssid_ ? ssid_ : "disabled");
        comm_.blinkWait(1, 100, 100, 2000);
        log_.debug("UtilMain", "calling comm_.begin()");
        comm_.begin();
        log_.info("UtilMain", "link layer up (comm_.begin returned)");
        if (ssid_) {
            log_.info("UtilMain", "starting web monitor (port %u)", (unsigned)webPort_);
            // setRole before begin() so the first /stats response
            // already has the role pill populated.
            mon_.setRole(isPing_ ? "Ping" : "Pong");
            uint32_t monStart = millis();
            bool monOk = mon_.begin(ssid_, password_, webPort_);
            log_.info("UtilMain", "web monitor begin returned %s in %lu ms",
                monOk ? "true" : "false", (unsigned long)(millis() - monStart));
        } else {
            log_.info("UtilMain", "WiFi disabled — skipping web monitor");
        }
        // Version logged after mon_.begin() so the line appears in
        // the web log panel (sink is registered inside begin()).
        log_.info("AutoLink", "v" AUTOLINK_VERSION);
        comm_.blinkWait(2, 100, 100, 2000);
        log_.debug("UtilMain", "setupCommon complete");
    }

    // Emit a throughput line at most once every 5 seconds. tag is
    // "Ping" or "Pong" — appears as the log source label.
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

    uint32_t    debugBaud_;
    bool        isPing_;        // captured for mon_.setRole()
    AutoLink    comm_;
    AutoLinkWeb mon_;
    const char* ssid_;
    const char* password_;
    uint16_t    webPort_;
    Log&        log_;

    uint8_t  buf_[BUF_SIZE];
    bool     wasReady_ = false;

private:
    uint32_t tStat_  = 0;
    uint64_t lastTx_ = 0;
    uint64_t lastRx_ = 0;
};

} // namespace autolink
#endif // ARDUINO
