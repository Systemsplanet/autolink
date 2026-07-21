
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
#include "al/web/OtaCore.h"

#ifdef ARDUINO

namespace autolink {
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
