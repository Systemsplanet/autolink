// Arduino-only web server implementation.
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

namespace autolink
{
AutoLinkWeb::AutoLinkWeb(AutoLink &link) : link_(link), log_(Log::log()) {}

void AutoLinkWeb::setRole(const char *role)
{
    if (!role)
        role = "";
    snprintf(snap_.role, sizeof(snap_.role), "%s", role);
}

AutoLinkWeb::~AutoLinkWeb()
{
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

bool AutoLinkWeb::begin(const char *ssid, const char *pass, uint16_t port)
{
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

    Log &log = Log::log();
    log.info(TAG, "begin: entering (port=%u)", (unsigned)port);

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
            return false;
        }
    }
    log.info(TAG,
             "begin() returned; WiFi connecting in background (up to %lu s)",
             (unsigned long)(WIFI_BG_TIMEOUT_MS / 1000));
    return true;
}

void AutoLinkWeb::wifiTaskThunk_(void *arg)
{
    auto *self = static_cast<AutoLinkWeb *>(arg);
    Log &log = Log::log();
    log.info(TAG, "WiFi bg task started (timeout=%lu s)",
             (unsigned long)(WIFI_BG_TIMEOUT_MS / 1000));

    WiFi.begin(self->ssid_, self->pass_);
    log.info(TAG, "WiFi.begin returned status=%d", (int)WiFi.status());

    const uint32_t startMs = millis();
    uint32_t lastProgressLog = 0;
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - startMs > WIFI_BG_TIMEOUT_MS) {
            log.error(
                TAG,
                "WiFi bg task: gave up after %lu ms (status=%d) — web monitor disabled; SWP handshake continues. Sketch runs normally; reboot to retry WiFi.",
                (unsigned long)(millis() - startMs), (int)WiFi.status());
            self->wifiTask_ = nullptr;
            vTaskDelete(nullptr);
            return;
        }
        if (millis() - lastProgressLog > 2000) {
            log.info(TAG, "WiFi connecting... status=%d elapsed=%lu ms",
                     (int)WiFi.status(), (unsigned long)(millis() - startMs));
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
}

bool AutoLinkWeb::setupHttpAndLogging_()
{
    Log &log = Log::log();
    if (enabled_) {
        log.warning(TAG, "setupHttpAndLogging_ called twice; ignoring");
        return true;
    }
    logRing_ = (LogEntry *)calloc(RING_CAP, sizeof(LogEntry));
    snapMtx_ = xSemaphoreCreateMutex();
    logMtx_ = xSemaphoreCreateMutex();

    if (!logRing_ || !snapMtx_ || !logMtx_) {
        log.error(TAG, "resource alloc failed");
        goto fail;
    }

    Log::log().setSink(logSinkCb, this);

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
        cfg.stack_size = 6144;

        cfg.task_priority = 10;

        cfg.max_open_sockets = 7;

        cfg.lru_purge_enable = false;

        esp_err_t hs = httpd_start(&server_, &cfg);
        if (hs != ESP_OK) {
            log.error(TAG, "httpd_start failed: %s", esp_err_to_name(hs));
            goto fail;
        }

        const httpd_uri_t r0 = { "/", HTTP_GET, handleRoot, this };
        const httpd_uri_t r1 = { "/stats", HTTP_GET, handleStats, this };
        const httpd_uri_t r2 = { "/logs", HTTP_GET, handleLogs, this };
        const httpd_uri_t r3 = { "/reset", HTTP_POST, handleReset, this };
        const httpd_uri_t r4 = { "/reboot", HTTP_POST, handleReboot, this };
        const httpd_uri_t r5 = { "/level", HTTP_POST, handleLevel, this };
        const httpd_uri_t r6 = { "/mode", HTTP_POST, handleMode, this };
        const httpd_uri_t r7 = { "/pausemsg", HTTP_POST, handleMsgPause, this };
        static const httpd_uri_t *const URIS[] = { &r0, &r1, &r2, &r3,
                                                   &r4, &r5, &r6, &r7 };
        static const char *const PATHS[] = { "/",         "/stats", "/logs",
                                             "/reset",    "/level", "/mode",
                                             "/msgpause", "/reboot" };
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
    return false;
}

String AutoLinkWeb::ip() const
{
    return WiFi.localIP().toString();
}

void AutoLinkWeb::statTimerCb(void *arg)
{
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
    self->snap_.msgPaused =
        self->msgPausedReader_ ? (self->msgPausedReader_() ? 1u : 0u) : 0u;
    xSemaphoreGive(self->snapMtx_);

    self->prevTx_ = s.tx;
    self->prevRx_ = s.rx;
}

void AutoLinkWeb::logSinkCb(char sev, const char *tag, const char *msg,
                            void *ctx)
{
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

esp_err_t AutoLinkWeb::handleRoot(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, DASHBOARD_HTML, sizeof(DASHBOARD_HTML) - 1);
    return ESP_OK;
}

esp_err_t AutoLinkWeb::handleStats(httpd_req_t *req)
{
    AutoLinkWeb *self = (AutoLinkWeb *)req->user_ctx;

    Snapshot s;
    xSemaphoreTake(self->snapMtx_, portMAX_DELAY);
    s = self->snap_;
    xSemaphoreGive(self->snapMtx_);

    char buf[512];
    int len = formatStatsJson(&s, (int)Log::log().getLevel(), AUTOLINK_VERSION,
                              buf, sizeof(buf));

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

esp_err_t AutoLinkWeb::handleLogs(httpd_req_t *req)
{
    AutoLinkWeb *self = (AutoLinkWeb *)req->user_ctx;

    char query[48] = {};
    uint32_t since = 0;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[20] = {};
        if (httpd_query_key_value(query, "since", val, sizeof(val)) == ESP_OK) {
            since = (uint32_t)strtoul(val, nullptr, 10);
        }
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "close");
    char head[32];
    snprintf(head, sizeof(head), "{\"head\":%lu,\"lines\":[",
             (unsigned long)self->logHead_);
    httpd_resp_sendstr_chunk(req, head);

    bool first = true;
    if (xSemaphoreTake(self->logMtx_, pdMS_TO_TICKS(50)) == pdTRUE) {
        const uint32_t total = self->logHead_;
        uint32_t start = (total > (uint32_t)RING_CAP) ? (total - RING_CAP) : 0;
        if (since > start)
            start = since;

        char chunk[LINE_CAP * 2 + 64];

        for (uint32_t i = start; i < total; i++) {
            const LogEntry &e = self->logRing_[i % RING_CAP];
            if (e.seq != i)
                continue;

            if (!first)
                httpd_resp_sendstr_chunk(req, ",");
            first = false;

            int pos = snprintf(chunk, sizeof(chunk),
                               "{\"seq\":%lu,\"sev\":\"%c\", \"text\":\"",
                               (unsigned long)i, e.sev);

            for (const char *p = e.line; *p && pos < (int)(sizeof(chunk) - 4);
                 p++) {
                switch (*p) {
                case '"':
                    chunk[pos++] = '\\';
                    chunk[pos++] = '"';
                    break;
                case '\\':
                    chunk[pos++] = '\\';
                    chunk[pos++] = '\\';
                    break;
                case '\n':
                    chunk[pos++] = '\\';
                    chunk[pos++] = 'n';
                    break;
                case '\r':
                    chunk[pos++] = '\\';
                    chunk[pos++] = 'r';
                    break;
                default:
                    chunk[pos++] = *p;
                    break;
                }
            }
            chunk[pos++] = '"';
            chunk[pos++] = '}';
            esp_err_t sc = httpd_resp_send_chunk(req, chunk, pos);
            if (sc != ESP_OK) {
                self->log_.error("AutoLinkWeb", "resp_send_chunk failed: %s",
                                 esp_err_to_name(sc));
                xSemaphoreGive(self->logMtx_);
                return ESP_FAIL;
            }
        }
        xSemaphoreGive(self->logMtx_);
    }

    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}

esp_err_t AutoLinkWeb::handleReset(httpd_req_t *req)
{
    AutoLinkWeb *self = (AutoLinkWeb *)req->user_ctx;
    self->link_.resetStats();
    self->link_.resetErrors();
    self->link_.resetDiag();
    if (xSemaphoreTake(self->snapMtx_, pdMS_TO_TICKS(50)) == pdTRUE) {
        self->prevTx_ = 0;
        self->prevRx_ = 0;
        xSemaphoreGive(self->snapMtx_);
    }
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, "ok", 2);
    return ESP_OK;
}

esp_err_t AutoLinkWeb::handleLevel(httpd_req_t *req)
{
    char query[48] = {};
    char val[8] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "lv", val, sizeof(val)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "missing ?lv=", 13);
        return ESP_OK;
    }
    int rc = applyLogLevel(parseLevelQuery(val));
    if (rc < 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        if (rc == -2) {
            httpd_resp_send(
                req,
                "lv=0 (NONE) is rejected: silences all logs and is unrecoverable without reflash",
                79);
        } else {
            httpd_resp_send(req, "lv must be 1..5 (ERROR..VERBOSE)", 33);
        }
        return ESP_OK;
    }
    int lv = rc;
    Log::log().info(TAG, "Log level set to %d via web", lv);
    {
        Preferences prefs;
        if (prefs.begin("autolink", false)) {
            if (prefs.putUChar("log_level", (uint8_t)lv) == 0) {
                Log::log().warning(TAG, "failed to persist log level %d to NVS",
                                   lv);
            }
            prefs.end();
        } else {
            Log::log().warning(
                TAG, "could not open NVS namespace to persist log level");
        }
    }
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, "ok", 2);
    return ESP_OK;
}

esp_err_t AutoLinkWeb::handleMode(httpd_req_t *req)
{
    char query[48] = {};
    char val[8] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "m", val, sizeof(val)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "missing ?m=", 12);
        return ESP_OK;
    }
    auto *self = static_cast<AutoLinkWeb *>(req->user_ctx);
    if (!self->fillModeWriter_) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "no fill mode (Pong?)", 22);
        return ESP_OK;
    }
    uint8_t mode = 99;
    if (strcmp(val, "seq") == 0)
        mode = 0;
    else if (strcmp(val, "rand") == 0)
        mode = 1;
    if (mode > 1) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "m must be 'seq' or 'rand'", 25);
        return ESP_OK;
    }
    self->fillModeWriter_(mode);
    Log::log().info(TAG, "Fill mode set to %s via web",
                    mode == 0 ? "sequential" : "random");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, "ok", 2);
    return ESP_OK;
}

esp_err_t AutoLinkWeb::handleMsgPause(httpd_req_t *req)
{
    char query[48] = {};
    char val[8] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "p", val, sizeof(val)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "missing ?p=", 12);
        return ESP_OK;
    }
    auto *self = static_cast<AutoLinkWeb *>(req->user_ctx);
    if (!self->msgPausedWriter_) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "no msg pause (Pong?)", 22);
        return ESP_OK;
    }
    bool paused = false;
    if (strcmp(val, "1") == 0 || strcmp(val, "true") == 0)
        paused = true;
    else if (strcmp(val, "0") == 0 || strcmp(val, "false") == 0)
        paused = false;
    else {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "p must be '1', '0', 'true', or 'false'", 38);
        return ESP_OK;
    }
    self->msgPausedWriter_(paused);
    Log::log().info(TAG, "Message pause set to %s via web",
                    paused ? "PAUSED" : "RESUMED");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, paused ? "paused" : "resumed", paused ? 6 : 7);
    return ESP_OK;
}

esp_err_t AutoLinkWeb::handleReboot(httpd_req_t *req)
{
    Log::log().info("ALinkWeb",
                    "Reboot requested via web — restarting in 200 ms");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, "rebooting", 9);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

} // namespace autolink
#endif