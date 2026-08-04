
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
    // lastLinkDropped / lastAppDropped carry the
    // previous-window values of droppedChunksOnReset and
    // the app's local droppedOnClear_ so the next
    // logStats line can cross-check the deltas. Seeded
    // to UINT64_MAX so the first window skips the
    // comparison.
    uint64_t lastLinkDropped = 0;
    uint64_t lastAppDropped = UINT64_MAX;
};

// App-state fields appended to the periodic stats line. Pinned by
// StatsIncludeAppStateTest. The buggy-original logStats did not include any
// of these, so a wedged app (gap-stop, paused, no sends) emitted
// no stats line and the operator had no idea what state the app
// was in. Every periodic tick (5s) carries the app state —
// gapStopped / missing seq / paused / last sendMsg reason — so a
// wedge self-identifies.
struct AppStateLog {
    bool gapStopped = false;
    uint8_t gapMissingSeq = 0xFF;
    bool paused = false;
    int lastSendMsgReason = 0; // raw SendMsgReason enum value
};

inline void logStats(Log &log, const char *tag, AutoLink &comm, StatBaseline &b,
                     uint64_t echoCount, uint64_t mismatchCount,
                     uint64_t appDropped = 0,
                     const AppStateLog &app = AppStateLog()) {
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
    // Cross-check the link's reported chunk loss
    // (droppedChunksOnReset, the wire-level count) against
    // the app's local-queue drops (appDropped, the
    // user-visible count). The two counters are not
    // expected to match by magnitude — a single queued
    // message spans multiple chunks — so the check is
    // directional: any single side moving while the other
    // stays at zero flags a silent loss path. The
    // equality test is intentionally dropped. Pinned
    // by DroppedCountCrossCheckTest.
    if (b.lastAppDropped != UINT64_MAX) {
        uint64_t linkDelta = s.droppedChunksOnReset - b.lastLinkDropped;
        uint64_t appDelta = appDropped - b.lastAppDropped;
        if (linkDelta > 0 && appDelta == 0) {
            log.warning(tag,
                        "link-side chunk loss without app-side "
                        "drop: link=%llu (delta=%llu) — chunks "
                        "wiped by reset were not in the app queue",
                        (unsigned long long)s.droppedChunksOnReset,
                        (unsigned long long)linkDelta);
        } else if (appDelta > 0 && linkDelta == 0) {
            log.warning(tag,
                        "app-side drop without link-side loss: "
                        "app=%llu (delta=%llu) — queue flushed "
                        "without a wire-level reset signal",
                        (unsigned long long)appDropped,
                        (unsigned long long)appDelta);
        }
    }
    b.lastLinkDropped = s.droppedChunksOnReset;
    b.lastAppDropped = appDropped;
    uint32_t dt = now - b.tMs;
    uint64_t txRate = dt ? ((s.tx - b.lastTx) * 1000ULL / dt) : 0;
    uint64_t rxRate = dt ? ((s.rx - b.lastRx) * 1000ULL / dt) : 0;
    const char *modeIcon =
        (comm.mode() == AutoLinkConfig::Mode::SYNC) ? "[S]" : "[A]";
    // SendMsgReason enum name for the log line. The raw int
    // is for the test pin; the human-readable name is for
    // the operator. See AutoLink.h's SendMsgReason enum for
    // the source of truth.
    const char *sendReasonName = "Ok";
    switch (app.lastSendMsgReason) {
    case 0:
        sendReasonName = "Ok";
        break;
    case 1:
        sendReasonName = "NotOk";
        break;
    case 2:
        sendReasonName = "PostLockQuiet";
        break;
    case 3:
        sendReasonName = "GbnWindowFull";
        break;
    case 4:
        sendReasonName = "PoolExhaust";
        break;
    case 5:
        sendReasonName = "LengthInvalid";
        break;
    case 6:
        sendReasonName = "LengthZero";
        break;
    case 7:
        sendReasonName = "ChunksOverflow";
        break;
    case 8:
        sendReasonName = "SyncMidMessageTimeout";
        break;
    case 9:
        sendReasonName = "RateLimited";
        break;
    default:
        sendReasonName = "?";
        break;
    }
    log.info(
        tag,
        "%s echos=%llu  mismatch=%llu   dropped=%llu   appDropped=%llu   quietDrops=%llu  rateDrops=%llu  settleDrops=%llu   tx=%llu B/sec  rx=%llu B/sec  baud=%lu   disc=%llu  errs=%llu"
        "  gapStopped=%s  gapMissing=%u  paused=%s  lastSend=%s",
        modeIcon, (unsigned long long)echoCount,
        (unsigned long long)mismatchCount,
        (unsigned long long)s.droppedChunksOnReset,
        (unsigned long long)appDropped,
        (unsigned long long)s.postLockQuietDrops,
        (unsigned long long)s.rateLimitedDrops,
        (unsigned long long)s.settleDrops, txRate, rxRate,
        (unsigned long)comm.getCurrentBaud(), (unsigned long long)s.discCount,
        (unsigned long long)s.frameErrs, app.gapStopped ? "true" : "false",
        (unsigned)app.gapMissingSeq, app.paused ? "true" : "false",
        sendReasonName);
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
    // INFO is the production default for ASYNC builds. The
    // prior default (the per-echo debug level) flooded the log
    // sink with per-echo lines at the ASYNC pipeline rate and
    // starved the log of the very state-transition lines
    // needed to diagnose a wedge — the per-echo lines were
    // demoted to verbose in a prior pass, but the level was
    // still set to emit them at full ASYNC pipeline rate.
    // Verbose log entries are still available for deep
    // debugging; the runtime needs to opt in by raising the
    // level explicitly.
    esp_log_level_set("*", ESP_LOG_INFO);
    log.setLevel(Log::INFO);
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
