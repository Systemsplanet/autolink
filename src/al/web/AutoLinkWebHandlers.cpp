// AutoLink web monitor -- HTTP handlers. The ten
// httpd_uri_t callbacks registered in
// AutoLinkWeb::setupHttpAndLogging_ (handleRoot,
// handleStats, handleLogs, handleReset, handleLevel,
// handleMode, handleModeToggle, handleMsgPause,
// handleDelay, handleReboot) all live here. They
// share the same AutoLinkWeb instance as the
// lifecycle code in AutoLinkWeb.cpp; the linker
// resolves the private member references across
// the two TUs.
//
// Splitting these out keeps the lifecycle TU
// focused on boot + WiFi + httpd + log ring
// (which never changes once the device is up),
// while the handler TU is the surface that
// evolves as new dashboard features land.
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

esp_err_t AutoLinkWeb::handleRoot(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    const char *p = DASHBOARD_HTML;
    size_t remaining = sizeof(DASHBOARD_HTML) - 1;
    const size_t CHUNK = 4096;
    while (remaining > 0) {
        size_t n = (remaining > CHUNK) ? CHUNK : remaining;
        esp_err_t err = httpd_resp_send_chunk(req, p, (ssize_t)n);
        if (err != ESP_OK)
            return err;
        p += n;
        remaining -= n;
    }
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}

esp_err_t AutoLinkWeb::handleStats(httpd_req_t *req) {
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

esp_err_t AutoLinkWeb::handleLogs(httpd_req_t *req) {
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

esp_err_t AutoLinkWeb::handleReset(httpd_req_t *req) {
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

esp_err_t AutoLinkWeb::handleLevel(httpd_req_t *req) {
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

esp_err_t AutoLinkWeb::handleMode(httpd_req_t *req) {
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

// Flip the link's SYNC/ASYNC mode, persist to NVS, and reboot.
// The dashboard reads the persisted mode on next boot (via
// PingPongBase's bringUpLink NVS read) so the new mode takes
// effect across the reboot. The response is dispatched on a
// detached FreeRTOS task so httpd can drain the socket before
// esp_restart() pulls the rug out.
static void alinkModeToggleReboot_(void *) {
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}
esp_err_t AutoLinkWeb::handleModeToggle(httpd_req_t *req) {
    auto *self = static_cast<AutoLinkWeb *>(req->user_ctx);
    uint8_t cur = (uint8_t)self->link_.mode();
    uint8_t nxt = (cur == (uint8_t)AutoLinkConfig::Mode::SYNC)
        ? (uint8_t)AutoLinkConfig::Mode::ASYNC
        : (uint8_t)AutoLinkConfig::Mode::SYNC;

    Log::log().info(
        "ALinkWeb",
        "Link mode toggle %s -> %s via web; "
        "persisting to NVS and rebooting",
        cur == (uint8_t)AutoLinkConfig::Mode::SYNC ? "SYNC" : "ASYNC",
        nxt == (uint8_t)AutoLinkConfig::Mode::SYNC ? "SYNC" : "ASYNC");

    {
        Preferences prefs;
        if (prefs.begin("autolink", false)) {
            prefs.putUChar("mode", nxt);
            prefs.end();
            Log::log().info("ALinkWeb",
                            "Persisted mode=%u to NVS namespace 'autolink'",
                            (unsigned)nxt);
        } else {
            Log::log().error(
                "ALinkWeb",
                "Could not open NVS namespace 'autolink' to persist mode; "
                "reboot will not change the running mode");
        }
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, "rebooting", 9);

    BaseType_t ok = xTaskCreate(alinkModeToggleReboot_, "alink-mode-rbt", 1024,
                                nullptr, 1, nullptr);
    if (ok != pdPASS) {
        Log::log().error("ALinkWeb",
                         "xTaskCreate for mode-toggle reboot failed; "
                         "calling esp_restart() inline");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }
    return ESP_OK;
}

esp_err_t AutoLinkWeb::handleMsgPause(httpd_req_t *req) {
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

esp_err_t AutoLinkWeb::handleDelay(httpd_req_t *req) {
    char query[64] = {};
    char val[12] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "ms", val, sizeof(val)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "missing ?ms=", 12);
        return ESP_OK;
    }
    auto *self = static_cast<AutoLinkWeb *>(req->user_ctx);
    if (!self->txDelayWriter_) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "no tx delay (Pong?)", 20);
        return ESP_OK;
    }
    int ms = atoi(val);
    if (ms < 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "ms must be >= 0", 15);
        return ESP_OK;
    }
    self->txDelayWriter_(ms);
    Log::log().info(TAG, "txDelayMs set to %d via web", ms);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Connection", "close");
    char out[16];
    int n = snprintf(out, sizeof(out), "%d", ms);
    httpd_resp_send(req, out, n);
    return ESP_OK;
}

esp_err_t AutoLinkWeb::handleReboot(httpd_req_t *req) {
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
