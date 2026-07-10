
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
#include "esp_ota_ops.h"
#include <FS.h>
#include <LittleFS.h>
#include "al/web/OtaCore.h"

#ifdef ARDUINO

namespace autolink {

esp_err_t AutoLinkWeb::handleRoot(httpd_req_t *req) {
    AutoLinkWeb *self = (AutoLinkWeb *)req->user_ctx;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");

    if (self && self->fsOk_ && LittleFS.exists("/web/index.html")) {
        File f = LittleFS.open("/web/index.html", "r");
        if (f) {
            static char chunk[1024];
            for (;;) {
                size_t n = f.read((uint8_t *)chunk, sizeof(chunk));
                if (n == 0)
                    break;
                if (httpd_resp_send_chunk(req, chunk, (ssize_t)n) != ESP_OK) {
                    f.close();
                    return ESP_FAIL;
                }
            }
            f.close();
            httpd_resp_send_chunk(req, nullptr, 0);
            return ESP_OK;
        }
    }
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
    char val[12] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "m", val, sizeof(val)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "missing ?m=", 12);
        return ESP_OK;
    }
    auto *self = static_cast<AutoLinkWeb *>(req->user_ctx);
    AutoLinkConfig::Mode newMode;
    if (strcmp(val, "SYNC") == 0)
        newMode = AutoLinkConfig::Mode::SYNC;
    else if (strcmp(val, "ASYNC") == 0)
        newMode = AutoLinkConfig::Mode::ASYNC;
    else {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "m must be SYNC or ASYNC", 24);
        return ESP_OK;
    }
    AutoLinkConfig::Mode prev = self->link_.mode();
    {
        Preferences prefs;
        if (prefs.begin("autolink", false)) {
            prefs.putUChar("mode", (uint8_t)newMode);
            prefs.end();
        } else {
            Log::log().warning(TAG, "could not open NVS to persist link mode");
        }
    }
    Log::log().info(
        TAG, "Link mode persist %s -> %s via web (NVS only; reboot required)",
        prev == AutoLinkConfig::Mode::SYNC ? "SYNC" : "ASYNC",
        newMode == AutoLinkConfig::Mode::SYNC ? "SYNC" : "ASYNC");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, "ok", 2);
    return ESP_OK;
}

esp_err_t AutoLinkWeb::handleFillMode(httpd_req_t *req) {
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

void AutoLinkWeb::drainBody_(httpd_req_t *req, size_t remaining) {
    char sink[256];
    while (remaining > 0) {
        size_t ask = remaining > sizeof(sink) ? sizeof(sink) : remaining;
        int r = httpd_req_recv(req, sink, ask);
        if (r == HTTPD_SOCK_ERR_TIMEOUT)
            continue;
        if (r <= 0)
            return;
        remaining -= (size_t)r;
    }
}

esp_err_t AutoLinkWeb::handleOtaFw(httpd_req_t *req) {
    Log &log = Log::log();
    size_t total = req->content_len;
    size_t left = total;
    const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
    if (total == 0 || !next || total > next->size) {
        drainBody_(req, left);
        httpd_resp_set_status(
            req, !next ? "500 Internal Server Error" : "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_set_hdr(req, "Connection", "close");
        const char *m = !next ? "no OTA app slot (need ota_0/ota_1 partitions)"
                              : "empty or oversized image";
        log.error("ALinkWeb", "OTA fw rejected: %s (len=%u)", m,
                  (unsigned)total);
        httpd_resp_send(req, m, strlen(m));
        return ESP_OK;
    }
    size_t chunkSz =
        otaRecvChunkBytes(esp_get_free_heap_size(), OTA_HEAP_RESERVE);
    char *buf = (char *)malloc(chunkSz);
    esp_ota_handle_t h = 0;
    esp_err_t e = buf ? esp_ota_begin(next, total, &h) : ESP_ERR_NO_MEM;
    if (e == ESP_OK) {
        log.info("ALinkWeb", "OTA fw -> %s (%u bytes, chunk=%u)", next->label,
                 (unsigned)total, (unsigned)chunkSz);
        while (left > 0) {
            size_t ask = left > chunkSz ? chunkSz : left;
            int r = httpd_req_recv(req, buf, ask);
            if (r == HTTPD_SOCK_ERR_TIMEOUT)
                continue;
            if (r <= 0) {
                e = ESP_FAIL;
                break;
            }
            e = esp_ota_write(h, buf, (size_t)r);
            if (e != ESP_OK)
                break;
            left -= (size_t)r;
        }
        esp_err_t fin = (e == ESP_OK) ? esp_ota_end(h) : (esp_ota_abort(h), e);
        e = fin;
    }
    free(buf);
    if (e == ESP_OK)
        e = esp_ota_set_boot_partition(next);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Connection", "close");
    if (e != ESP_OK) {
        drainBody_(req, left);
        log.error("ALinkWeb", "OTA fw failed: %s", esp_err_to_name(e));
        httpd_resp_set_status(req, "400 Bad Request");
        const char *fm = "OTA failed — image invalid or write error";
        httpd_resp_send(req, fm, strlen(fm));
        return ESP_OK;
    }
    log.info("ALinkWeb", "OTA fw OK — booting %s, restarting in 200 ms",
             next->label);
    const char *okm = "OTA OK — rebooting";
    httpd_resp_send(req, okm, strlen(okm));
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

namespace {

struct GuiSink {
    File f;
    bool wrote = false;
};
bool guiEntry(void *c, const char *name, uint32_t) {
    GuiSink *s = (GuiSink *)c;
    char path[128];
    if (!otaSafeEntryName(name, path, sizeof(path)))
        return false;

    for (char *p = path + 1; *p; p++)
        if (*p == '/') {
            *p = '\0';
            LittleFS.mkdir(path);
            *p = '/';
        }
    s->f = LittleFS.open(path, "w");
    return (bool)s->f;
}
bool guiData(void *c, const uint8_t *b, size_t n) {
    GuiSink *s = (GuiSink *)c;
    return s->f.write(b, n) == n;
}
bool guiEnd(void *c) {
    GuiSink *s = (GuiSink *)c;
    s->f.close();
    s->wrote = true;
    return true;
}
} // namespace

esp_err_t AutoLinkWeb::handleOtaGui(httpd_req_t *req) {
    AutoLinkWeb *self = (AutoLinkWeb *)req->user_ctx;
    Log &log = Log::log();
    size_t left = req->content_len;
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Connection", "close");
    if (left == 0 || !self || !self->mountFs_()) {
        drainBody_(req, left);
        httpd_resp_set_status(
            req, left == 0 ? "400 Bad Request" : "507 Insufficient Storage");
        const char *m = left == 0 ? "empty upload" : "LittleFS unavailable";
        log.error("ALinkWeb", "OTA gui rejected: %s", m);
        httpd_resp_send(req, m, strlen(m));
        return ESP_OK;
    }
    GuiSink sink;
    OtaZipCbs cbs{ &sink, guiEntry, guiData, guiEnd };
    OtaZipStream zip(cbs);
    size_t chunkSz =
        otaRecvChunkBytes(esp_get_free_heap_size(), OTA_HEAP_RESERVE);
    char *buf = (char *)malloc(chunkSz);
    bool ok = buf != nullptr;
    while (ok && left > 0) {
        size_t ask = left > chunkSz ? chunkSz : left;
        int r = httpd_req_recv(req, buf, ask);
        if (r == HTTPD_SOCK_ERR_TIMEOUT)
            continue;
        if (r <= 0) {
            ok = false;
            break;
        }
        ok = zip.feed((const uint8_t *)buf, (size_t)r);
        left -= (size_t)r;
    }
    free(buf);
    if (sink.f)
        sink.f.close();
    if (!ok || !zip.done() || !sink.wrote) {
        drainBody_(req, left);
        const char *why = zip.err() ? zip.err() : "truncated or empty zip";
        log.error("ALinkWeb", "OTA gui failed: %s", why);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, why, strlen(why));
        return ESP_OK;
    }
    log.info("ALinkWeb", "OTA gui OK — dashboard now served from LittleFS");
    httpd_resp_send(req, "GUI updated", 11);
    return ESP_OK;
}

} // namespace autolink
#endif
