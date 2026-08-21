
#include "al/web/AutoLinkWeb.h"
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
#include "esp_ota_ops.h"
#include <FS.h>
#include <LittleFS.h>

#ifdef ARDUINO

namespace autolink {
AutoLinkWeb::AutoLinkWeb(AutoLink &link) : link_(link), log_(Log::log()) {
    // Cap the link's maxMsg *before* the user calls link.begin() so
    // the buffer floors are sized for the smaller ask. The web
    // monitor's default cap of 2048 fits the dashboard's /stats
    // JSON comfortably and frees ~9 KB across streamBuf + txBuf
    // on a 41 KB post-alloc free heap (rxBuf is unchanged — it
    // depends on the ARQ pipeline window, not maxMsg). Without
    // this cap, the heap accounting cannot leave enough room for
    // httpd / WiFi on a 41 KB device and the GUI wedged on
    // boot. Pinned by EspHalHeapAccountingTest's field-numbers
    // case.
    if (link_.maxMsg() > webMaxMsgCap_)
        link_.setMaxMsg(webMaxMsgCap_);
}

void AutoLinkWeb::setLinkMaxMsg(size_t cap) {
    webMaxMsgCap_ = cap;
    // Re-apply if the link hasn't begun yet — the buffer floors
    // are sized in begin() from cfg.maxMsg, so a later cap
    // change is a no-op.
    if (link_.maxMsg() > webMaxMsgCap_)
        link_.setMaxMsg(webMaxMsgCap_);
}

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
                    // setupHttpAndLogging_() itself logs "Web monitor at
                    // http://..." on success (AutoLinkWebHttpd.cpp) —
                    // right where enabled_ is set and httpd is
                    // confirmed running. Logging it again here duplicated
                    // every line, which is why the field log showed it
                    // twice back-to-back.
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

} // namespace autolink
#endif
