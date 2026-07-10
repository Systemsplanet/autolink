
#pragma once
#ifdef ARDUINO

#    include "AutoLink.h"
#    include "al/web/AutoLinkWeb.h"
#    include <Arduino.h>
#    include <Preferences.h>

namespace autolink {

struct StatBaseline {
    uint32_t tMs = 0;
    uint64_t lastTx = 0;
    uint64_t lastRx = 0;
};

inline void logStats(Log &log, const char *tag, AutoLink &comm, StatBaseline &b,
                     uint64_t echoCount, uint64_t mismatchCount) {
    uint32_t now = millis();
    if (now - b.tMs < 5000)
        return;
    Stats s;
    comm.getStats(s);

    if (s.tx < b.lastTx || s.rx < b.lastRx) {
        b.lastTx = s.tx;
        b.lastRx = s.rx;
        b.tMs = now;
        return;
    }
    uint32_t dt = now - b.tMs;
    uint64_t txRate = dt ? ((s.tx - b.lastTx) * 1000ULL / dt) : 0;
    uint64_t rxRate = dt ? ((s.rx - b.lastRx) * 1000ULL / dt) : 0;
    const char *modeIcon =
        (comm.mode() == AutoLinkConfig::Mode::SYNC) ? "[S]" : "[A]";
    log.info(
        tag,
        "%s echos=%llu  mismatch=%llu   tx=%llu B/sec  rx=%llu B/sec  baud=%lu   disc=%llu  errs=%llu",
        modeIcon, (unsigned long long)echoCount,
        (unsigned long long)mismatchCount, txRate, rxRate,
        (unsigned long)comm.getCurrentBaud(), (unsigned long long)s.discCount,
        (unsigned long long)s.frameErrs);
    b.lastTx = s.tx;
    b.lastRx = s.rx;
    b.tMs = now;
}

inline void resetStatBaseline(StatBaseline &b) {
    b.lastTx = 0;
    b.lastRx = 0;
    b.tMs = 0;
}

inline void initSerial(Log &log, uint32_t debugBaud, const char *role,
                       const char *ssid) {
    esp_log_level_set("*", ESP_LOG_VERBOSE);
    log.setLevel(Log::DEBUG);
    Serial.begin(debugBaud);
    log.info("PingPongBase", "boot: role=%s  baud=%lu  WiFi=%s", role,
             (unsigned long)debugBaud, ssid ? ssid : "disabled");
}

inline void bringUpLink(Log &log, AutoLink &comm, bool prePaused = false) {
    log.info("AutoLink", "v" AUTOLINK_VERSION);
    if (prePaused)
        comm.setLinkPaused(true);
    {
        Preferences prefs;
        if (prefs.begin("autolink", true)) {
            uint8_t saved = prefs.getUChar("mode", 0xFF);
            prefs.end();
            if (saved != 0xFF) {
                comm.setMode(saved == (uint8_t)AutoLinkConfig::Mode::SYNC
                                 ? AutoLinkConfig::Mode::SYNC
                                 : AutoLinkConfig::Mode::ASYNC);
                log.info(
                    "PingPongBase", "Restored persisted link mode from NVS: %s",
                    saved == (uint8_t)AutoLinkConfig::Mode::SYNC ? "SYNC"
                                                                 : "ASYNC");
            }
        }
    }
    comm.blinkWait(1, 100, 100, 2000);
    log.debug("PingPongBase", "calling comm_.begin()");
    comm.begin();
    if (!comm.isHealthy()) {
        log.error("PingPongBase",
                  "HAL not healthy after begin() — "
                  "UART install / task / timer failed; "
                  "halting to avoid silent wire");
        while (true) {
            comm.blinkWait(1, 300, 300, 0);
        }
    }
    log.info("PingPongBase", "link layer up (comm_.begin returned)");
    comm.blinkWait(2, 100, 100, 2000);
}

inline bool startWebMonitor(Log &log, AutoLinkWeb &mon, const char *role,
                            const char *ssid, const char *password,
                            uint16_t webPort) {
    if (!ssid) {
        log.info("PingPongBase", "WiFi disabled — skipping web monitor");
        return false;
    }
    log.info("PingPongBase", "starting web monitor (port %u)",
             (unsigned)webPort);
    mon.setRole(role);
    uint32_t monStart = millis();
    bool monOk = mon.begin(ssid, password, webPort);
    log.info("PingPongBase", "web monitor begin: %s  (%lu ms)",
             monOk ? "ok" : "FAILED", (unsigned long)(millis() - monStart));
    return monOk;
}

struct PingPongBase {
    static constexpr size_t BUF_SIZE = AUTOLINK_DEFAULT_MAX_MSG;

    static constexpr int MAX_TX_PER_LOOP = 16;

    PingPongBase(uint32_t debugBaud, uart_port_t uartNum, int rxPin, int txPin,
                 bool isPing, const char *ssid = nullptr,
                 const char *password = nullptr, uint16_t webPort = 8765)
        : debugBaud_(debugBaud), isPing_(isPing),
          comm_(uartNum, rxPin, txPin, isPing), mon_(comm_), ssid_(ssid),
          password_(password ? password : ""), webPort_(webPort),
          log_(Log::log()) {}

    PingPongBase(const PingPongBase &) = delete;
    PingPongBase &operator=(const PingPongBase &) = delete;

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
};

} // namespace autolink
#endif
