
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

    mountFs_();
    {
        const esp_partition_t *run = esp_ota_get_running_partition();
        esp_ota_img_states_t st;
        if (run && esp_ota_get_state_partition(run, &st) == ESP_OK &&
            st == ESP_OTA_IMG_PENDING_VERIFY)
            esp_ota_mark_app_valid_cancel_rollback();
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
        cfg.stack_size = 8192;

        cfg.task_priority = 10;

        cfg.max_open_sockets = 3;

        cfg.lru_purge_enable = false;

        cfg.max_uri_handlers = 12;

        esp_err_t hs = ESP_OK;

        for (uint32_t attempt = 1; attempt <= HTTPD_RETRY_MAX; attempt++) {
            log.info(TAG,
                     "setupHttpAndLogging_: attempt %lu/%lu — waiting %lu ms "
                     "for socket TIME_WAIT",
                     (unsigned long)attempt, (unsigned long)HTTPD_RETRY_MAX,
                     (unsigned long)HTTPD_RETRY_PRE_MS);
            vTaskDelay(pdMS_TO_TICKS(HTTPD_RETRY_PRE_MS));
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
        const httpd_uri_t r7 = { "/fillmode", HTTP_POST, handleFillMode, this };
        const httpd_uri_t r8 = { "/pausemsg", HTTP_POST, handleMsgPause, this };
        const httpd_uri_t r9 = { "/delay", HTTP_POST, handleDelay, this };
        const httpd_uri_t r10 = { "/ota/fw", HTTP_POST, handleOtaFw, this };
        const httpd_uri_t r11 = { "/ota/gui", HTTP_POST, handleOtaGui, this };
        static const httpd_uri_t *const URIS[] = { &r0, &r1, &r2,  &r3,
                                                   &r4, &r5, &r6,  &r7,
                                                   &r8, &r9, &r10, &r11 };
        static const char *const PATHS[] = {
            "/",     "/stats",    "/logs",     "/reset", "/reboot", "/level",
            "/mode", "/fillmode", "/pausemsg", "/delay", "/ota/fw", "/ota/gui"
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

    return false;
}

String AutoLinkWeb::ip() const { return WiFi.localIP().toString(); }

bool AutoLinkWeb::mountFs_() {
    if (fsOk_)
        return true;
    fsOk_ = LittleFS.begin(true);
    if (!fsOk_)
        Log::log().warning(TAG,
                           "LittleFS mount failed — GUI OTA disabled, "
                           "serving baked-in dashboard");
    return fsOk_;
}

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
    self->snap_.postLockQuietDrops = s.postLockQuietDrops;
    self->snap_.rateLimitedDrops = s.rateLimitedDrops;
    self->snap_.txRingStallDrops = s.txRingStallDrops;
    // AL90-7: surface log ring drops in the
    // dashboard JSON. Pinned by
    // LogDropsSurfaceTest.
    self->snap_.logDrops = s.logDrops;
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

    if (xSemaphoreTake(self->logMtx_, pdMS_TO_TICKS(5)) != pdTRUE) {
        self->logDropped_++;
        if (!self->logDropWarned_) {
            self->logDropWarned_ = true;
            // Can't call Log::log().warning() here — that would
            // re-enter this exact sink. Nothing to do but count;
            // formatStatsJson (AL87-02b) surfaces logDropped_ so
            // the operator sees it even though this specific
            // occurrence is unlogged by definition.
        }
        return;
    }
    self->logDropWarned_ = false;

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
