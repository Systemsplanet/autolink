// AutoLink web monitor -- lifecycle. Owns the
// AutoLinkWeb instance's constructor / destructor,
// the begin() boot sequence, the WiFi background task,
// the httpd setup + log ring / snapshot timer wiring,
// and the log sink callback that drains the singleton
// log into the ring buffer.
//
// The HTTP request handlers (handleRoot, handleStats,
// handleLogs, handleReset, handleLevel, handleMode,
// handleFillMode, handleMsgPause, handleDelay,
// handleReboot) live in AutoLinkWebHandlers.cpp. Same
// class, same namespace, separate TU -- the linker
// resolves static / private member references across
// the two files at link time.
//
// The class methods stay in AutoLinkWeb.h.
#include "al/web/AutoLinkWeb.h"
#include "al/web/AutoLinkWebHtml.h"
#include "al/web/AutoLinkWebCore.h"
#include "al/link/Link.h"
#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_heap_caps.h"
#include "esp_system.h"

#ifdef ARDUINO

namespace autolink {
AutoLinkWeb::AutoLinkWeb(AutoLink &link) : link_(link), log_(Log::log()) {}

void AutoLinkWeb::setRole(const char *role) {
    if (!role)
        role = "";
    snprintf(snap_.role, sizeof(snap_.role), "%s", role);
}

AutoLinkWeb::~AutoLinkWeb() {
    if (statTimer_) {
        esp_err_t ts = esp_timer_stop(statTimer_);
        if (ts != ESP_OK) {
            Log::log().error(TAG, "dtor esp_timer_stop: %s",
                             esp_err_to_name(ts));
        }
        esp_err_t td = esp_timer_delete(statTimer_);
        if (td != ESP_OK) {
            Log::log().error(TAG, "dtor esp_timer_delete: %s",
                             esp_err_to_name(td));
        }
        statTimer_ = nullptr;
    }
    if (server_) {
        esp_err_t hs = httpd_stop(server_);
        if (hs != ESP_OK) {
            Log::log().error(TAG, "dtor httpd_stop: %s", esp_err_to_name(hs));
        }
        server_ = nullptr;
    }
    Log::log().clearSink();

    if (snapMtx_) {
        vSemaphoreDelete(snapMtx_);
        snapMtx_ = nullptr;
    }
    if (logMtx_) {
        vSemaphoreDelete(logMtx_);
        logMtx_ = nullptr;
    }
    free(logRing_);
    logRing_ = nullptr;
}

bool AutoLinkWeb::begin(const char *ssid, const char *pass, uint16_t port) {
    if (enabled_)
        return true;
    port_ = port;

    if (ssid_) {
        free(ssid_);
        ssid_ = nullptr;
    }
    if (pass_) {
        free(pass_);
        pass_ = nullptr;
    }
    if (ssid)
        ssid_ = strdup(ssid);
    if (pass)
        pass_ = strdup(pass);

    logRing_ = (LogEntry *)calloc(RING_CAP, sizeof(LogEntry));
    snapMtx_ = xSemaphoreCreateMutex();
    logMtx_ = xSemaphoreCreateMutex();

    if (!logRing_ || !snapMtx_ || !logMtx_) {
        Log::log().error(TAG,
                         "begin: log ring / mutex alloc failed — "
                         "web monitor disabled");
        if (logRing_) {
            free(logRing_);
            logRing_ = nullptr;
        }
        if (snapMtx_) {
            vSemaphoreDelete(snapMtx_);
            snapMtx_ = nullptr;
        }
        if (logMtx_) {
            vSemaphoreDelete(logMtx_);
            logMtx_ = nullptr;
        }
        return false;
    }

    Log::log().setSink(logSinkCb, this);

    Log &log = Log::log();
    log.info("AutoLink", "v%s", AUTOLINK_VERSION);

    {
        Preferences prefs;
        bool prefsOk = prefs.begin("autolink", true);
        log.info(TAG, "begin: NVS open returned %s",
                 prefsOk ? "true" : "false");
        if (prefsOk) {
            uint8_t saved = prefs.getUChar("log_level", 0xFF);
            log.info(TAG, "begin: NVS getUChar returned %u (0xFF = unset)",
                     (unsigned)saved);
            prefs.end();
            if (saved != 0xFF) {
                if (saved == (uint8_t)Log::NONE) {
                    log.setLevel(Log::INFO);
                    log.warning(
                        TAG,
                        "Saved level was NONE (would silence all logs); upgraded to INFO for this boot and clearing NVS");
                    Preferences p2;
                    if (p2.begin("autolink", false)) {
                        p2.putUChar("log_level", (uint8_t)Log::INFO);
                        p2.end();
                    }
                } else if (saved <= (uint8_t)Log::VERBOSE) {
                    log.setLevel((Log::Level)saved);
                    log.info(TAG, "Restored saved log level %u from NVS",
                             (unsigned)saved);
                } else {
                    log.warning(
                        TAG,
                        "Saved log level %u in NVS is out of range; falling back to boot default",
                        (unsigned)saved);
                }
            }
        }
    }

    log.info(TAG, "WiFi connect SSID=\"%s\" passLen=%u (background task)", ssid,
             (unsigned)strlen(pass));

    WiFi.mode(WIFI_STA);
    log.info(TAG, "WiFi: mode set to STA, launching background connect task");
    if (wifiTask_) {
        log.warning(TAG, "WiFi bg task already running; ignoring re-begin");
    } else {
        BaseType_t ok = xTaskCreate(&AutoLinkWeb::wifiTaskThunk_, "al-wifi-bg",
                                    4096, this, 1, &wifiTask_);
        if (ok != pdPASS) {
            log.error(TAG,
                      "xTaskCreate for WiFi bg failed; web monitor disabled");
            return true;
        }
    }

    log.info(TAG,
             "begin() waiting up to %lu ms for WiFi; bg task continues "
             "retrying forever if not connected",
             (unsigned long)WIFI_BEGIN_QUICK_MS);
    const uint32_t quickStartMs = millis();
    while (WiFi.status() != WL_CONNECTED &&
           millis() - quickStartMs < WIFI_BEGIN_QUICK_MS) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (WiFi.status() == WL_CONNECTED) {
        log.info(TAG, "begin: WiFi up IP=%s after %lu ms — starting httpd",
                 WiFi.localIP().toString().c_str(),
                 (unsigned long)(millis() - quickStartMs));

        const uint32_t httpStartMs = millis();
        while (!isUp() && millis() - httpStartMs < HTTPD_BEGIN_QUICK_MS) {
            vTaskDelay(pdMS_TO_TICKS(HTTPD_RETRY_DELAY_MS));
        }
        if (isUp()) {
            log.info(TAG,
                     "begin: web monitor ready at http://%s:%u "
                     "(sketch setup() may proceed)",
                     WiFi.localIP().toString().c_str(), port_);
        } else {
            log.error(TAG,
                      "begin: web monitor NOT ready after %lu ms; "
                      "sketch setup() proceeds, bg task will keep retrying",
                      (unsigned long)HTTPD_BEGIN_QUICK_MS);
        }
    } else {
        log.info(TAG,
                 "begin: WiFi not connected after %lu ms (status=%d); "
                 "bg task continues retrying, sketch setup() proceeds",
                 (unsigned long)(millis() - quickStartMs), (int)WiFi.status());
    }
    return true;
}

void AutoLinkWeb::wifiTaskThunk_(void *arg) {
    auto *self = static_cast<AutoLinkWeb *>(arg);
    Log &log = Log::log();

    const bool haveCreds = (self->ssid_ != nullptr && self->ssid_[0] != '\0');

    if (!haveCreds) {
        log.info(TAG,
                 "WiFi bg task: no SSID configured — offline mode, "
                 "task exits after %lu s",
                 (unsigned long)(WIFI_BG_TIMEOUT_MS / 1000));
        WiFi.begin(self->ssid_, self->pass_);
        const uint32_t startMs = millis();
        uint32_t lastProgressLog = 0;
        while (WiFi.status() != WL_CONNECTED) {
            if (millis() - startMs > WIFI_BG_TIMEOUT_MS) {
                log.error(TAG,
                          "WiFi bg task: gave up after %lu ms (no creds) "
                          "— web monitor disabled; SWP handshake continues.",
                          (unsigned long)(millis() - startMs));
                self->wifiTask_ = nullptr;
                vTaskDelete(nullptr);
                return;
            }
            if (millis() - lastProgressLog > 2000) {
                log.info(TAG, "WiFi connecting... status=%d elapsed=%lu ms",
                         (int)WiFi.status(),
                         (unsigned long)(millis() - startMs));
                lastProgressLog = millis();
            }
            vTaskDelay(pdMS_TO_TICKS(WIFI_BG_TICK_MS));
        }
        log.info(TAG, "WiFi connected IP=%s (took %lu ms)",
                 WiFi.localIP().toString().c_str(),
                 (unsigned long)(millis() - startMs));
        self->setupHttpAndLogging_();
        self->wifiTask_ = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    log.info(TAG,
             "WiFi bg task started (credentials provided — "
             "retrying forever with backoff up to %lu s)",
             (unsigned long)(WIFI_RETRY_BACKOFF_MS_MAX / 1000));

    uint32_t backoffMs = WIFI_RETRY_BACKOFF_MS_MIN;
    while (true) {
        WiFi.disconnect();
        vTaskDelay(pdMS_TO_TICKS(100));
        WiFi.begin(self->ssid_, self->pass_);
        log.info(TAG, "WiFi.begin returned status=%d (backoff=%lu ms)",
                 (int)WiFi.status(), (unsigned long)backoffMs);

        const uint32_t startMs = millis();
        uint32_t lastProgressLog = 0;
        bool connected = false;
        while (WiFi.status() != WL_CONNECTED) {
            if (millis() - startMs > backoffMs) {
                log.warning(TAG,
                            "WiFi connect attempt timed out after %lu ms "
                            "(status=%d) — retrying",
                            (unsigned long)backoffMs, (int)WiFi.status());
                break;
            }
            if (millis() - lastProgressLog > 2000) {
                log.info(TAG, "WiFi connecting... status=%d elapsed=%lu ms",
                         (int)WiFi.status(),
                         (unsigned long)(millis() - startMs));
                lastProgressLog = millis();
            }
            vTaskDelay(pdMS_TO_TICKS(WIFI_BG_TICK_MS));
        }
        if (WiFi.status() == WL_CONNECTED) {
            log.info(TAG, "WiFi connected IP=%s (took %lu ms)",
                     WiFi.localIP().toString().c_str(),
                     (unsigned long)(millis() - startMs));
            backoffMs = WIFI_RETRY_BACKOFF_MS_MIN;
            connected = true;
        } else {
            backoffMs = backoffMs * 2;
            if (backoffMs > WIFI_RETRY_BACKOFF_MS_MAX)
                backoffMs = WIFI_RETRY_BACKOFF_MS_MAX;
        }

        if (connected) {
            while (!self->isUp()) {
                if (self->setupHttpAndLogging_()) {
                    log.info(TAG, "Web monitor at http://%s:%u",
                             WiFi.localIP().toString().c_str(), self->port_);
                    break;
                }
                log.error(TAG,
                          "setupHttpAndLogging_ failed; retrying in %lu ms",
                          (unsigned long)HTTPD_RETRY_BG_MS);
                vTaskDelay(pdMS_TO_TICKS(HTTPD_RETRY_BG_MS));
            }
            while (WiFi.status() == WL_CONNECTED && self->isUp()) {
                vTaskDelay(pdMS_TO_TICKS(WIFI_BG_TICK_MS));
            }
            if (self->isUp() && WiFi.status() != WL_CONNECTED) {
                log.warning(TAG,
                            "WiFi link dropped (status=%d); re-entering retry",
                            (int)WiFi.status());
                self->enabled_ = false;
                if (self->server_) {
                    esp_err_t hs2 = httpd_stop(self->server_);
                    if (hs2 != ESP_OK) {
                        log.error(TAG, "httpd_stop on drop: %s",
                                  esp_err_to_name(hs2));
                    }
                    self->server_ = nullptr;
                }
                if (self->statTimer_) {
                    esp_err_t tts = esp_timer_stop(self->statTimer_);
                    if (tts != ESP_OK) {
                        log.error(TAG, "drop esp_timer_stop: %s",
                                  esp_err_to_name(tts));
                    }
                    esp_err_t ttd = esp_timer_delete(self->statTimer_);
                    if (ttd != ESP_OK) {
                        log.error(TAG, "drop esp_timer_delete: %s",
                                  esp_err_to_name(ttd));
                    }
                    self->statTimer_ = nullptr;
                }
            }
            backoffMs = WIFI_RETRY_BACKOFF_MS_MIN;
        }
    }
}

bool AutoLinkWeb::setupHttpAndLogging_() {
    Log &log = Log::log();
    if (enabled_) {
        log.warning(TAG, "setupHttpAndLogging_ called twice; ignoring");
        return true;
    }

    if (!logRing_ || !snapMtx_ || !logMtx_) {
        log.error(
            TAG,
            "setupHttpAndLogging_: log ring / mutex not initialised — call begin() first");
        return false;
    }

    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
    tzset();
    {
        struct tm ti = {};
        const uint32_t ntpStart = millis();
        while (!getLocalTime(&ti, 0) && millis() - ntpStart < 5000)
            delay(100);
        if (getLocalTime(&ti, 0)) {
            ntpSynced_ = true;
            log.info(TAG, "NTP synced: %04d-%02d-%02d %02d:%02d:%02d EST/EDT",
                     ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday, ti.tm_hour,
                     ti.tm_min, ti.tm_sec);
        } else {
            log.info(
                TAG,
                "NTP not available — timestamps are uptime (HH:MM:SS.mmm*)");
        }
    }

    {
        const esp_timer_create_args_t ta = {
            .callback = statTimerCb,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "al_web_stat",
            .skip_unhandled_events = true,
        };
        esp_err_t tcr = esp_timer_create(&ta, &statTimer_);
        esp_err_t tst = esp_timer_start_periodic(statTimer_, 1000000ULL);
        if (tcr != ESP_OK || tst != ESP_OK) {
            log.error(TAG, "stat timer init failed: create=%s start=%s",
                      esp_err_to_name(tcr), esp_err_to_name(tst));
            goto fail;
        }
    }

    {
        httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
        cfg.server_port = port_;
        cfg.stack_size = 16384;

        cfg.task_priority = 10;

        cfg.max_open_sockets = 7;

        cfg.lru_purge_enable = false;
        cfg.max_uri_handlers = 10;

        esp_err_t hs = ESP_OK;
        // On reboot the prior boot's httpd socket is in
        // TIME_WAIT for up to ~60 s. A bare HTTP server
        // start right after boot races that and returns
        // EADDRINUSE / ESP_ERR_HTTPD_ALLOC. Each attempt
        // sleeps HTTPD_RETRY_PRE_MS first so the kernel can
        // clear the socket before the next httpd_start.
        for (uint32_t attempt = 1; attempt <= HTTPD_RETRY_MAX; attempt++) {
            log.info(TAG,
                     "setupHttpAndLogging_: attempt %lu/%lu — waiting %lu ms "
                     "for socket TIME_WAIT",
                     (unsigned long)attempt, (unsigned long)HTTPD_RETRY_MAX,
                     (unsigned long)HTTPD_RETRY_PRE_MS);
            vTaskDelay(pdMS_TO_TICKS(HTTPD_RETRY_PRE_MS)); // per-attempt settle
            hs = httpd_start(&server_, &cfg);
            if (hs == ESP_OK)
                break;
            log.error(TAG, "httpd_start attempt %lu/%lu failed: %s",
                      (unsigned long)attempt, (unsigned long)HTTPD_RETRY_MAX,
                      esp_err_to_name(hs));
        }
        if (hs != ESP_OK) {
            log.error(TAG, "httpd_start failed after %lu attempts: %s",
                      (unsigned long)HTTPD_RETRY_MAX, esp_err_to_name(hs));
            goto fail;
        }

        const httpd_uri_t r0 = { "/", HTTP_GET, handleRoot, this };
        const httpd_uri_t r1 = { "/stats", HTTP_GET, handleStats, this };
        const httpd_uri_t r2 = { "/logs", HTTP_GET, handleLogs, this };
        const httpd_uri_t r3 = { "/reset", HTTP_POST, handleReset, this };
        const httpd_uri_t r4 = { "/reboot", HTTP_POST, handleReboot, this };
        const httpd_uri_t r5 = { "/level", HTTP_POST, handleLevel, this };
        const httpd_uri_t r6 = { "/mode", HTTP_POST, handleMode, this };
        const httpd_uri_t r7 = { "/fillmode", HTTP_POST, handleFillMode,
                                 this };
        const httpd_uri_t r8 = { "/pausemsg", HTTP_POST, handleMsgPause,
                                 this };
        const httpd_uri_t r9 = { "/delay", HTTP_POST, handleDelay, this };
        static const httpd_uri_t *const URIS[] = { &r0, &r1, &r2, &r3, &r4,
                                                   &r5, &r6, &r7, &r8, &r9 };
        static const char *const PATHS[] = {
            "/",      "/stats", "/logs",     "/reset", "/reboot",
            "/level", "/mode",  "/fillmode", "/pausemsg", "/delay"
        };
        for (size_t i = 0; i < sizeof(URIS) / sizeof(URIS[0]); i++) {
            esp_err_t ruh = httpd_register_uri_handler(server_, URIS[i]);
            if (ruh != ESP_OK) {
                log.error(TAG, "register_uri_handler(%s) failed: %s", PATHS[i],
                          esp_err_to_name(ruh));
            }
        }
    }

    enabled_ = true;
    log.info(TAG, "Web monitor at http://%s:%u",
             WiFi.localIP().toString().c_str(), port_);
    return true;

fail:
    if (statTimer_) {
        esp_err_t ts2 = esp_timer_stop(statTimer_);
        if (ts2 != ESP_OK) {
            Log::log().error(TAG, "fail esp_timer_stop: %s",
                             esp_err_to_name(ts2));
        }
        esp_err_t td2 = esp_timer_delete(statTimer_);
        if (td2 != ESP_OK) {
            Log::log().error(TAG, "fail esp_timer_delete: %s",
                             esp_err_to_name(td2));
        }
        statTimer_ = nullptr;
    }
    if (server_) {
        esp_err_t hs2 = httpd_stop(server_);
        if (hs2 != ESP_OK) {
            Log::log().error(TAG, "fail httpd_stop: %s", esp_err_to_name(hs2));
        }
        server_ = nullptr;
    }
    // Do NOT destroy logRing_ / snapMtx_ / logMtx_ here —
    // those are begin()-lifetime resources owned by the
    // AutoLinkWeb instance. Destroying them makes the
    // bg-task retry of setupHttpAndLogging_ dereference
    // a null logMtx_ inside logSinkCb on the very next
    // log call. The bg task's HTTPD_RETRY_BG_MS loop
    // expects setupHttpAndLogging_() to be idempotent
    // over those resources; tearing them down on
    // partial-fail breaks the retry contract. They
    // outlive this AutoLinkWeb instance and are freed
    // in the destructor.
    return false;
}

String AutoLinkWeb::ip() const { return WiFi.localIP().toString(); }

void AutoLinkWeb::statTimerCb(void *arg) {
    AutoLinkWeb *self = (AutoLinkWeb *)arg;

    Stats s;
    self->link_.getStats(s);
    Diag d;
    self->link_.getDiag(d);

    xSemaphoreTake(self->snapMtx_, portMAX_DELAY);
    self->snap_.txBps =
        (s.tx >= self->prevTx_) ? (uint32_t)(s.tx - self->prevTx_) : 0;
    self->snap_.rxBps =
        (s.rx >= self->prevRx_) ? (uint32_t)(s.rx - self->prevRx_) : 0;
    self->snap_.txTotal = s.tx;
    self->snap_.rxTotal = s.rx;
    self->snap_.errTotal = s.discCount;
    self->snap_.errCount = (uint32_t)s.frameErrs;
    self->snap_.lostMsgs = d.lostMsgs;
    strncpy(self->snap_.state, StateToStr(self->link_.getState()), 3);
    self->snap_.state[3] = '\0';
    self->snap_.rssi = (int32_t)WiFi.RSSI();
    self->snap_.freeHeap = esp_get_free_heap_size();
    self->snap_.uptimeS = millis() / 1000;
    self->snap_.baudRate = self->link_.getCurrentBaud();
    self->snap_.fillMode = self->fillModeReader_ ? self->fillModeReader_() : 0;
    self->snap_.linkMode = (uint8_t)self->link_.mode();
    snprintf(self->snap_.linkModeLabel, sizeof(self->snap_.linkModeLabel), "%s",
             self->snap_.linkMode == (uint8_t)AutoLinkConfig::Mode::SYNC
                 ? "SYNC"
                 : "ASYNC");
    self->snap_.msgPaused =
        self->msgPausedReader_ ? (self->msgPausedReader_() ? 1u : 0u) : 0u;
    self->snap_.txDelayMs =
        self->txDelayReader_ ? (int32_t)self->txDelayReader_() : 0;
    xSemaphoreGive(self->snapMtx_);

    self->prevTx_ = s.tx;
    self->prevRx_ = s.rx;
}

void AutoLinkWeb::logSinkCb(char sev, const char *tag, const char *msg,
                            void *ctx) {
    AutoLinkWeb *self = (AutoLinkWeb *)ctx;
    if (!self->logRing_)
        return;

    if (xSemaphoreTake(self->logMtx_, pdMS_TO_TICKS(5)) != pdTRUE)
        return;

    const uint32_t idx = self->logHead_ % RING_CAP;
    self->logRing_[idx].seq = self->logHead_;
    self->logRing_[idx].sev = sev;
    char ts[16];
    if (self->ntpSynced_) {
        struct timeval tv = {};
        if (gettimeofday(&tv, nullptr) == 0) {
            struct tm ti = {};
            localtime_r(&tv.tv_sec, &ti);
            int ms = (int)(tv.tv_usec / 1000);
            snprintf(ts, sizeof(ts), "%02d:%02d:%02d.%03d", ti.tm_hour,
                     ti.tm_min, ti.tm_sec, ms);
        } else {
            uint32_t ms_total = millis();
            uint32_t s = ms_total / 1000;
            snprintf(ts, sizeof(ts), "%02lu:%02lu:%02lu.%03lu*",
                     (unsigned long)(s / 3600), (unsigned long)(s % 3600 / 60),
                     (unsigned long)(s % 60), (unsigned long)(ms_total % 1000));
        }
    } else {
        uint32_t ms_total = millis();
        uint32_t s = ms_total / 1000;
        snprintf(ts, sizeof(ts), "%02lu:%02lu:%02lu.%03lu*",
                 (unsigned long)(s / 3600), (unsigned long)(s % 3600 / 60),
                 (unsigned long)(s % 60), (unsigned long)(ms_total % 1000));
    }
    snprintf(self->logRing_[idx].line, LINE_CAP, "%s %c %s %s", ts, sev, tag,
             msg);
    self->logHead_++;

    xSemaphoreGive(self->logMtx_);
}

} // namespace autolink
#endif
