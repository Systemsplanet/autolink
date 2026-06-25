// Shared Ping/Pong config held by
// COMPOSITION in Ping and Pong.
// logStats + resetStatBaseline are
// free functions (diagnostics is a
// separate concern from comms/web).
// setupCommon was split into discrete
// steps (initSerial / bringUpLink /
// startWebMonitor) the caller sequences
// — app lifecycle is the caller's job.
#pragma once
#ifdef ARDUINO

#    include "AutoLink.h"
#    include "al/web/AutoLinkWeb.h"
#    include <Arduino.h>

namespace autolink {

// Rolling tx/rx byte-rate baseline
// for logStats. Owned by Ping / Pong
// — the rate window lives with the
// thing being measured, not with the
// comms handle.
struct StatBaseline {
    uint32_t tMs = 0;
    uint64_t lastTx = 0;
    uint64_t lastRx = 0;
};

// Diagnostics: every >5 s, log
// tx/rx byte rate, mode icon, current
// baud, disc count, frame errors.
// Caller holds the rolling baseline
// so the window survives reconnects
// across link-lost / resume paths.
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

// App-lifecycle step 1: bring up the
// debug Serial, set log level, print
// the boot banner. Caller can choose
// a different baud or skip this.
inline void initSerial(Log &log, uint32_t debugBaud, const char *role,
                       const char *ssid) {
    esp_log_level_set("*", ESP_LOG_VERBOSE);
    log.setLevel(Log::DEBUG);
    Serial.begin(debugBaud);
    log.info("PingPongBase", "boot: role=%s  baud=%lu  WiFi=%s", role,
             (unsigned long)debugBaud, ssid ? ssid : "disabled");
}

// App-lifecycle step 2: blink, bring
// up the link layer (comm.begin()),
// log version, blink again. Caller
// does NOT get the "link layer up"
// message mid-loop; it's a one-shot
// here. `prePaused` lets the caller
// signal "begin this link in the
// paused state" so Ping can defer
// the master break until the user
// pushes Start. Halt on isHealthy()
// == false — proceeding silently into
// loop() with a dead HAL just produces
// a wall of "not ready" logs and a
// silent wire.
inline void bringUpLink(Log &log, AutoLink &comm, bool prePaused = false) {
    if (prePaused)
        comm.setLinkPaused(true);
    comm.blinkWait(1, 100, 100, 2000);
    log.debug("PingPongBase", "calling comm_.begin()");
    comm.begin();
    if (!comm.isHealthy()) {
        log.error("PingPongBase",
                  "HAL not healthy after begin() — "
                  "UART install / task / timer failed; "
                  "halting to avoid silent wire");
        while (true)
            delay(1000);
    }
    log.info("PingPongBase", "link layer up (comm_.begin returned)");
    log.info("AutoLink", "v" AUTOLINK_VERSION);
    comm.blinkWait(2, 100, 100, 2000);
}

// App-lifecycle step 3: start the
// web monitor. Caller decides whether
// to call this at all (current
// behaviour: only if ssid is set).
// Returns whether mon.begin() reported
// ok — caller can log / branch on it.
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
    static constexpr int BUF_SIZE = 1024;

    // Per-loop send/receive cap for
    // ASYNC mode. SYNC mode always
    // transmits at most one frame per
    // loop regardless.
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
