// Shared Ping/Pong base: holds the AutoLink facade +
// web monitor, prints the AUTOLINK_VERSION banner,
// drives the 5s stats ticker.
#pragma once
#ifdef ARDUINO

#    include "AutoLink.h"
#    include "al/web/AutoLinkWeb.h"
#    include <Arduino.h>

namespace autolink
{
class PingPongBase
{
public:
    static constexpr int BUF_SIZE = 1024;

    PingPongBase(uint32_t debugBaud,
                 uart_port_t uartNum, int rxPin,
                 int txPin, bool isPing,
                 const char *ssid = nullptr,
                 const char *password = nullptr,
                 uint16_t webPort = 8765)
        : debugBaud_(debugBaud), isPing_(isPing),
          comm_(uartNum, rxPin, txPin, isPing),
          mon_(comm_), ssid_(ssid),
          password_(password ? password : ""),
          webPort_(webPort), log_(Log::log())
    {
    }

    PingPongBase(const PingPongBase &) = delete;
    PingPongBase &
    operator=(const PingPongBase &) = delete;

    static uint64_t echoCountReaderThunk_()
    {
        return 0;
    }
    static uint64_t mismatchCountReaderThunk_()
    {
        return 0;
    }

protected:
    void setupCommon()
    {
        esp_log_level_set("*", ESP_LOG_VERBOSE);
        log_.setLevel(Log::DEBUG);
        Serial.begin(debugBaud_);
        log_.info("PingPongBase",
                  "boot: role=%s  baud=%lu  WiFi=%s",
                  isPing_ ? "Ping" : "Pong",
                  (unsigned long)debugBaud_,
                  ssid_ ? ssid_ : "disabled");
        comm_.blinkWait(1, 100, 100, 2000);
        log_.debug("PingPongBase",
                   "calling comm_.begin()");
        comm_.begin();
        log_.info(
            "PingPongBase",
            "link layer up (comm_.begin returned)");
        if (ssid_) {
            log_.info("PingPongBase",
                      "starting web monitor (port %u)",
                      (unsigned)webPort_);


            mon_.setRole(isPing_ ? "Ping" : "Pong");
            uint32_t monStart = millis();
            bool monOk =
                mon_.begin(ssid_, password_, webPort_);
            log_.info(
                "PingPongBase",
                "web monitor begin returned %s in %lu "
                "ms",
                monOk ? "true" : "false",
                (unsigned long)(millis() - monStart));
        } else {
            log_.info("PingPongBase",
                      "WiFi disabled — skipping web "
                      "monitor");
        }


        log_.info("AutoLink", "v" AUTOLINK_VERSION);
        comm_.blinkWait(2, 100, 100, 2000);
    }


    void logStats(const char *tag)
    {
        uint32_t now = millis();
        if (now - tStat_ < 5000)
            return;
        Stats s;
        comm_.getStats(s);


        if (s.tx < lastTx_ || s.rx < lastRx_) {
            lastTx_ = s.tx;
            lastRx_ = s.rx;
            tStat_ = now;
            return;
        }
        uint32_t dt = now - tStat_;
        uint64_t txRate =
            dt ? ((s.tx - lastTx_) * 1000ULL / dt) : 0;
        uint64_t rxRate =
            dt ? ((s.rx - lastRx_) * 1000ULL / dt) : 0;
        log_.info(
            tag,
            "echos=%llu  mismatch=%llu  "
            "tx=%llu B/sec  rx=%llu B/sec  baud=%lu  "
            "disc=%llu  errs=%llu",
            (unsigned long long)
                echoCountReaderThunk_(),
            (unsigned long long)
                mismatchCountReaderThunk_(),
            txRate, rxRate,
            (unsigned long)comm_.getCurrentBaud(),
            (unsigned long long)s.discCount,
            (unsigned long long)s.frameErrs);
        lastTx_ = s.tx;
        lastRx_ = s.rx;
        tStat_ = now;
    }


    void resetStatBaseline()
    {
        lastTx_ = 0;
        lastRx_ = 0;
        tStat_ = 0;
    }

    uint32_t debugBaud_;
    bool isPing_;
    AutoLink comm_;
    AutoLinkWeb mon_;
    const char *ssid_;
    const char *password_;
    uint16_t webPort_;
    Log &log_;

    uint8_t buf_[BUF_SIZE];
    bool wasReady_ = false;

private:
    uint32_t tStat_ = 0;
    uint64_t lastTx_ = 0;
    uint64_t lastRx_ = 0;
};

} // namespace autolink
#endif