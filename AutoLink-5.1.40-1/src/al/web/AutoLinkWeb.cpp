#include <time.h>
#include <sys/time.h>  // gettimeofday() — gives us microsecond resolution
                       // for the milliseconds field in log timestamps.
// AutoLinkWeb.cpp — AutoLinkWeb implementation (Arduino/ESP32 only).
//
// Embeds the full dashboard HTML/CSS/JS as a raw string literal, connects
// to WiFi in begin(), starts esp_http_server, registers four endpoints
// (GET /, GET /stats, GET /logs, POST /reset), and runs a 1 Hz esp_timer
// to snapshot AutoLink stats for the /stats handler.
#ifdef ARDUINO

#include "al/web/AutoLinkWeb.h"
#include "al/web/AutoLinkWebHtml.h" // embedded dashboard HTML (testable)
#include "al/web/AutoLinkWebCore.h" // format/parse helpers (testable on host)
#include "al/protocol/ALink.h"          // StateToStr()
#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>   // NVS-backed log level persistence
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_heap_caps.h"
#include "esp_system.h"   // esp_restart()

namespace autolink {

// ---------------------------------------------------------------------------
// Embedded dashboard — single HTML file, all CSS + JS inline, no CDN deps.
// Dark mobile-first layout; updates via fetch() polling every 1 second.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

AutoLinkWeb::AutoLinkWeb(AutoLink& link) : link_(link) {}

// Role label setter. Short string ("Ping" or "Pong"); snprintf for
// safety. Called from setup() before the timer starts, so no
// locking needed — the role string is stable for the life of the
// process once begin() runs.
void AutoLinkWeb::setRole(const char* role) {
    if (!role) role = "";
    // Copy into the snapshot field directly — the field is plain
    // memory and the timer is single-writer / single-reader on the
    // same task after begin(). We pad/truncate to 7 chars + NUL.
    snprintf(snap_.role, sizeof(snap_.role), "%s", role);
}

AutoLinkWeb::~AutoLinkWeb() {
    // Shutdown order matters:
    //   1. Stop the timer   — no more stat callbacks touching snap_
    //   2. Stop the server  — no more HTTP handlers touching snap_ or logRing_
    //   3. Clear the sink   — no more logSinkCb calls touching logRing_
    //   4. Free resources
    if (statTimer_) {
        esp_timer_stop(statTimer_);
        esp_timer_delete(statTimer_);
        statTimer_ = nullptr;
    }
    if (server_) {
        httpd_stop(server_);
        server_ = nullptr;
    }
    Log::log().clearSink();

    if (snapMtx_) { vSemaphoreDelete(snapMtx_); snapMtx_ = nullptr; }
    if (logMtx_)  { vSemaphoreDelete(logMtx_);  logMtx_  = nullptr; }
    free(logRing_);
    logRing_ = nullptr;
}

// ---------------------------------------------------------------------------
// begin() — connect WiFi, allocate resources, start server
// ---------------------------------------------------------------------------

bool AutoLinkWeb::begin(const char* ssid, const char* pass, uint16_t port) {
    if (enabled_) return true;
    port_ = port;

    // v5.1.32: heap-copy the SSID and password so the bg task can
    // read them after this stack frame is gone. The password stays
    // in this object's member only; we never log it (only its
    // length, see wifiTaskThunk_).
    if (ssid_) { free(ssid_); ssid_ = nullptr; }
    if (pass_) { free(pass_); pass_ = nullptr; }
    if (ssid) ssid_ = strdup(ssid);
    if (pass) pass_ = strdup(pass);

    Log& log = Log::log();
    log.info(TAG, "begin: entering (port=%u)", (unsigned)port);
    // Restore the saved log level from NVS before any other setup.
    // Namespace "autolink" to avoid user-key collisions; key
    // "log_level" (uint8_t) holds the Log::Level enum value. Stale
    // values are clamped — if a legacy device has an out-of-range
    // value, fall back to the sketch's boot default rather than
    // running at a wrong level.
    {
        log.info(TAG, "begin: opening NVS namespace 'autolink' (read-only)");
        Preferences prefs;
        bool prefsOk = prefs.begin("autolink", true);  // read-only
        log.info(TAG, "begin: NVS open returned %s", prefsOk ? "true" : "false");
        if (prefsOk) {
            uint8_t saved = prefs.getUChar("log_level", 0xFF);
            log.info(TAG, "begin: NVS getUChar returned %u (0xFF = unset)",
                (unsigned)saved);
            prefs.end();
            if (saved != 0xFF) {
                // v4.1.16: auto-recover from a stored NONE. If a
                // previous build (or this one before the NONE
                // rejection) saved lv=0 to NVS, the device appears
                // bricked (no log lines anywhere). Silently upgrade
                // to INFO so the operator can see logs again, then
                // overwrite the NVS entry so subsequent boots skip
                // this branch.
                if (saved == (uint8_t)Log::NONE) {
                    log.setLevel(Log::INFO);
                    log.warning(TAG,
                        "Saved level was NONE (would silence all logs); "
                        "upgraded to INFO for this boot and clearing NVS");
                    Preferences p2;
                    if (p2.begin("autolink", false)) {
                        p2.putUChar("log_level", (uint8_t)Log::INFO);
                        p2.end();
                    }
                } else if (saved <= (uint8_t)Log::VERBOSE) {
                    // CRITICAL: apply the saved level BEFORE logging
                    // the "restored" message. Otherwise a saved value
                    // of 0 (Log::NONE) would silence every log line
                    // from this point onward — including the very
                    // message that tells the user the level was set.
                    log.setLevel((Log::Level)saved);
                    log.info(TAG, "Restored saved log level %u from NVS",
                        (unsigned)saved);
                } else {
                    log.warning(TAG,
                        "Saved log level %u in NVS is out of range; "
                        "falling back to boot default", (unsigned)saved);
                }
            }
        }
    }
    log.info(TAG, "WiFi connect SSID=\"%s\" passLen=%u (background task)",
        ssid, (unsigned)strlen(pass));

    // v5.1.32: WiFi connect runs in a background FreeRTOS task
    // (wifiTask_). begin() returns immediately so the rest of
    // setup() — and especially the link layer's SWP handshake —
    // runs without being blocked by WiFi's `delay(250)` calls and
    // interrupt storms. The task gives up after WIFI_BG_TIMEOUT_MS
    // (10 s by default) so a long-term AP outage doesn't pin a
    // task on the scheduler.
    //
    // Disclosed: this trades the old "block setup until WiFi is up
    // OR fails forever" for "try once in the background, give up
    // after 10 s if it doesn't connect". The user gets the Serial
    // monitor back immediately and can monitor the SWP start
    // process even if WiFi never comes up. If WiFi fails, the
    // dashboard never starts and isUp() stays false.
    WiFi.mode(WIFI_STA);
    log.info(TAG, "WiFi: mode set to STA, launching background connect task");
    if (wifiTask_) {
        // begin() called twice; let the prior task finish on its own.
        log.warning(TAG, "WiFi bg task already running; ignoring re-begin");
    } else {
        BaseType_t ok = xTaskCreate(
            &AutoLinkWeb::wifiTaskThunk_, "al-wifi-bg",
            4096,        // stack words
            this,        // arg
            1,           // priority (low)
            &wifiTask_);
        if (ok != pdPASS) {
            log.error(TAG, "xTaskCreate for WiFi bg failed; web monitor disabled");
            return false;
        }
    }
    // HTTP server, log ring, and stats timer are started by the bg
    // task on successful WiFi connect. Until then isUp() == false
    // and the sketch runs normally (SWP keeps going, UART RX
    // works, the operator can read the Serial monitor).
    log.info(TAG, "begin() returned; WiFi connecting in background (up to %lu s)",
        (unsigned long)(WIFI_BG_TIMEOUT_MS / 1000));
    return true;
}

// v5.1.32: WiFi bg task. Polls WiFi.status() for up to
// WIFI_BG_TIMEOUT_MS. On success, sets up the HTTP server, log
// ring, stats timer (the same setup the blocking version did
// inline). On timeout, logs and exits. Self-deletes.
void AutoLinkWeb::wifiTaskThunk_(void* arg) {
    auto* self = static_cast<AutoLinkWeb*>(arg);
    Log& log = Log::log();
    log.info(TAG, "WiFi bg task started (timeout=%lu s)",
        (unsigned long)(WIFI_BG_TIMEOUT_MS / 1000));

    WiFi.begin(self->ssid_, self->pass_);
    log.info(TAG, "WiFi.begin returned status=%d", (int)WiFi.status());

    const uint32_t startMs = millis();
    uint32_t lastProgressLog = 0;
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - startMs > WIFI_BG_TIMEOUT_MS) {
            log.error(TAG,
                "WiFi bg task: gave up after %lu ms (status=%d) — "
                "web monitor disabled; SWP handshake continues. "
                "Sketch runs normally; reboot to retry WiFi.",
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

    // From here on, this is the same setup the blocking version
    // did after WiFi came up. We need a lock here because
    // begin() is being called from the Arduino main task and
    // these resources might be touched by it; use a small mutex
    // around the resource setup.
    // (No-op if already set up by a prior begin(); we already
    // guarded wifiTask_ above.)
    self->setupHttpAndLogging_();
    self->wifiTask_ = nullptr;
    vTaskDelete(nullptr);
}

// v5.1.32: factored out of begin() so the bg task can call it
// after WiFi comes up. Allocates the log ring, stats timer, and
// HTTP server; sets enabled_=true on success.
bool AutoLinkWeb::setupHttpAndLogging_() {
    Log& log = Log::log();
    if (enabled_) {
        log.warning(TAG, "setupHttpAndLogging_ called twice; ignoring");
        return true;
    }
    logRing_ = (LogEntry*)calloc(RING_CAP, sizeof(LogEntry));
    snapMtx_ = xSemaphoreCreateMutex();
    logMtx_  = xSemaphoreCreateMutex();

    if (!logRing_ || !snapMtx_ || !logMtx_) {
        log.error(TAG, "resource alloc failed");
        goto fail;
    }

    // Register sink now — ring and mutex are ready; NTP and "Web monitor"
    // log lines below will flow into the ring and appear in the web log panel.
    Log::log().setSink(logSinkCb, this);

    // ----- NTP sync — EST/EDT (UTC-5/UTC-4 with auto DST) -----
    // configTime uses the POSIX tz string; SNTP runs in the background.
    // We poll up to 5 s for a valid time; if it doesn't arrive we fall
    // back to uptime timestamps (marked with * in the log).
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
    tzset();
    {
        struct tm ti = {};
        const uint32_t ntpStart = millis();
        while (!getLocalTime(&ti, 0) && millis() - ntpStart < 5000) delay(100);
        if (getLocalTime(&ti, 0)) {
            ntpSynced_ = true;
            log.info(TAG, "NTP synced: %04d-%02d-%02d %02d:%02d:%02d EST/EDT",
                ti.tm_year+1900, ti.tm_mon+1, ti.tm_mday,
                ti.tm_hour, ti.tm_min, ti.tm_sec);
        } else {
            log.info(TAG, "NTP not available — timestamps are uptime (HH:MM:SS.mmm*)");
        }
    }


    {   // 1 Hz periodic stats timer
        const esp_timer_create_args_t ta = {
            .callback              = statTimerCb,
            .arg                   = this,
            .dispatch_method       = ESP_TIMER_TASK,
            .name                  = "al_web_stat",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&ta, &statTimer_) != ESP_OK
         || esp_timer_start_periodic(statTimer_, 1000000ULL) != ESP_OK) {
            log.error(TAG, "stat timer create failed");
            goto fail;
        }
    }

    {   // HTTP server
        httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
        cfg.server_port      = port_;
        cfg.stack_size       = 6144;
        // task_priority 10: above the timer service (1), the UART
        // event task (5), and the log sink callback (running in
        // whatever task calls Log::emit — usually loop, but the
        // httpd handler itself also emits). Below the WiFi stack
        // (~23) so we don't fight it. The Arduino loopTask at
        // priority 25 still preempts us when the loop is busy, but
        // the httpd handler's work is small (<1 ms typical) so
        // most bursts leave enough idle time for it to run. The
        // browser's fetch timeout is 5 s (set in tfetch) to absorb
        // the worst-case 16-msg send burst from a 115200 Ping.
        cfg.task_priority    = 10;
        // NOTE: v5.0.1 tried pinning to core 0 (cfg.core_id = 0) to
        // keep the httpd task off the same core as the Arduino
        // loopTask during tight send bursts. The pin broke the
        // dashboard — the httpd worker never serviced requests on
        // core 0 in the field. Root cause: ESP-IDF's core_id = 0
        // appears to be ignored (the default config leaves it at
        // tskNO_AFFINITY, and the task lands wherever the scheduler
        // picks). The dashboard works fine on tskNO_AFFINITY
        // because the loop's tight send bursts are bounded by the
        // TX-FIFO drain time and the httpd task gets CPU between
        // chunks.
        // max_open_sockets=7: one for the / page (browsers keep the
        // document connection), two for the parallel /stats + /logs
        // polls, one for an in-flight POST (level / mode / reset /
        // reboot), two for the browser's parallel preconnects, one
        // headroom. The earlier value of 3 + lru_purge was fine for
        // a one-tab user but any click during a slow /logs response
        // could LRU-purge the in-flight socket and produce
        // ERR_CONNECTION_REFUSED on the next poll.
        cfg.max_open_sockets = 7;
        // LRU purger disabled: for a single-client dashboard it's
        // more harmful than helpful (purges mid-write sockets). With
        // 7 headroom, no purge is needed.
        cfg.lru_purge_enable = false;

        if (httpd_start(&server_, &cfg) != ESP_OK) {
            log.error(TAG, "httpd_start failed");
            goto fail;
        }

        const httpd_uri_t r0 = { "/",      HTTP_GET,  handleRoot,  this };
        const httpd_uri_t r1 = { "/stats", HTTP_GET,  handleStats, this };
        const httpd_uri_t r2 = { "/logs",  HTTP_GET,  handleLogs,  this };
        const httpd_uri_t r3 = { "/reset",  HTTP_POST, handleReset,  this };
        const httpd_uri_t r4 = { "/reboot", HTTP_POST, handleReboot, this };
        const httpd_uri_t r5 = { "/level",  HTTP_POST, handleLevel,  this };
        const httpd_uri_t r6 = { "/mode",   HTTP_POST, handleMode,   this };
        const httpd_uri_t r7 = { "/pausemsg", HTTP_POST, handleMsgPause, this };
        httpd_register_uri_handler(server_, &r0);
        httpd_register_uri_handler(server_, &r1);
        httpd_register_uri_handler(server_, &r2);
        httpd_register_uri_handler(server_, &r3);
        httpd_register_uri_handler(server_, &r4);
        httpd_register_uri_handler(server_, &r5);
        httpd_register_uri_handler(server_, &r6);
        httpd_register_uri_handler(server_, &r7);
    }

    enabled_ = true;
    log.info(TAG, "Web monitor at http://%s:%u", WiFi.localIP().toString().c_str(), port_);
    return true;

fail:
    if (statTimer_) { esp_timer_stop(statTimer_); esp_timer_delete(statTimer_); statTimer_ = nullptr; }
    if (server_)    { httpd_stop(server_); server_ = nullptr; }
    if (snapMtx_)   { vSemaphoreDelete(snapMtx_); snapMtx_ = nullptr; }
    if (logMtx_)    { vSemaphoreDelete(logMtx_);  logMtx_  = nullptr; }
    free(logRing_);  logRing_ = nullptr;
    return false;
}

String AutoLinkWeb::ip() const {
    return WiFi.localIP().toString();
}

// ---------------------------------------------------------------------------
// 1 Hz stats snapshot — runs in the esp_timer task
// ---------------------------------------------------------------------------

void AutoLinkWeb::statTimerCb(void* arg) {
    AutoLinkWeb* self = (AutoLinkWeb*)arg;

    Stats s;
    self->link_.getStats(s);
    Diag d;
    self->link_.getDiag(d);

    xSemaphoreTake(self->snapMtx_, portMAX_DELAY);
    // Guard against the app calling resetStats() between samples — clamp to 0.
    self->snap_.txBps    = (s.tx >= self->prevTx_) ? (uint32_t)(s.tx - self->prevTx_) : 0;
    self->snap_.rxBps    = (s.rx >= self->prevRx_) ? (uint32_t)(s.rx - self->prevRx_) : 0;
    self->snap_.txTotal  = s.tx;
    self->snap_.rxTotal  = s.rx;
    self->snap_.errTotal = s.discCount;
    self->snap_.errCount = (uint32_t)s.frameErrs;
    self->snap_.lostMsgs = d.lostMsgs;
    strncpy(self->snap_.state, StateToStr(self->link_.getState()), 3);
    self->snap_.state[3] = '\0';
    self->snap_.rssi     = (int32_t)WiFi.RSSI();
    self->snap_.freeHeap = esp_get_free_heap_size();
    self->snap_.uptimeS  = millis() / 1000;
    self->snap_.baudRate = self->link_.getCurrentBaud();
    // Read fill mode via the optional hook. Default 0 (sequential)
    // when no hook is registered (Pong side).
    self->snap_.fillMode = self->fillModeReader_ ? self->fillModeReader_() : 0;
    // v5.1.29: read device-side pause state for /stats. When no hook
    // is registered, default to 0 (not paused). The dashboard reads
    // /stats.msgPaused on every poll and reflects it in the button
    // label so the operator can see whether the device actually
    // respects the button click.
    self->snap_.msgPaused = self->msgPausedReader_ ? (self->msgPausedReader_() ? 1u : 0u) : 0u;
    // snap_.role is set once by setRole() in setup() and never
    // changes; the /stats handler reads it directly.
    xSemaphoreGive(self->snapMtx_);

    self->prevTx_ = s.tx;
    self->prevRx_ = s.rx;
}

// ---------------------------------------------------------------------------
// Log sink — called from Log::emit() in any task
// ---------------------------------------------------------------------------

void AutoLinkWeb::logSinkCb(char sev, const char* tag, const char* msg, void* ctx) {
    AutoLinkWeb* self = (AutoLinkWeb*)ctx;
    if (!self->logRing_) return;
    // 5 ms timeout: if the HTTP log handler holds the mutex briefly, we wait;
    // if something is badly wrong, we drop the entry rather than blocking the caller.
    if (xSemaphoreTake(self->logMtx_, pdMS_TO_TICKS(5)) != pdTRUE) return;

    const uint32_t idx  = self->logHead_ % RING_CAP;
    self->logRing_[idx].seq = self->logHead_;
    self->logRing_[idx].sev = sev;
    // "HH:MM:SS.mmm" = 12 chars + NUL = 13. The optional '*' suffix
    // (uptime fallback marker) adds one more, so 16 is the safe size.
    char ts[16];
    if (self->ntpSynced_) {
        struct timeval tv = {};
        if (gettimeofday(&tv, nullptr) == 0) {
            // Wall-clock with millisecond resolution. We deliberately
            // skip getLocalTime() and read tv_sec through localtime_r()
            // ourselves so the wall-clock seconds and the sub-second
            // millis come from the same instant — no skew between them.
            struct tm ti = {};
            localtime_r(&tv.tv_sec, &ti);
            int ms = (int)(tv.tv_usec / 1000);
            snprintf(ts, sizeof(ts), "%02d:%02d:%02d.%03d",
                     ti.tm_hour, ti.tm_min, ti.tm_sec, ms);
        } else {
            // NTP was synced but gettimeofday failed (very rare) — uptime
            uint32_t ms_total = millis();
            uint32_t s = ms_total / 1000;
            snprintf(ts, sizeof(ts), "%02lu:%02lu:%02lu.%03lu*",
                     (unsigned long)(s/3600), (unsigned long)(s%3600/60),
                     (unsigned long)(s%60), (unsigned long)(ms_total % 1000));
        }
    } else {
        // No NTP — uptime with * suffix so the reader knows it's not wall-clock
        uint32_t ms_total = millis();
        uint32_t s = ms_total / 1000;
        snprintf(ts, sizeof(ts), "%02lu:%02lu:%02lu.%03lu*",
                 (unsigned long)(s/3600), (unsigned long)(s%3600/60),
                 (unsigned long)(s%60), (unsigned long)(ms_total % 1000));
    }
    snprintf(self->logRing_[idx].line, LINE_CAP, "%s %c %s %s", ts, sev, tag, msg);
    self->logHead_++;

    xSemaphoreGive(self->logMtx_);
}

// ---------------------------------------------------------------------------
// HTTP handler: GET /
// ---------------------------------------------------------------------------

esp_err_t AutoLinkWeb::handleRoot(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    // sizeof - 1: exclude the NUL terminator of the string literal.
    httpd_resp_send(req, DASHBOARD_HTML, sizeof(DASHBOARD_HTML) - 1);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// HTTP handler: GET /stats
// ---------------------------------------------------------------------------

esp_err_t AutoLinkWeb::handleStats(httpd_req_t* req) {
    AutoLinkWeb* self = (AutoLinkWeb*)req->user_ctx;

    // The timer (1 Hz) writes to self->snap_; the handler copies it
    // under the mutex so the browser sees a consistent frame.
    // WebSnapshot is the single source of truth — no parallel struct
    // to keep in sync. The local copy is on the stack, no heap.
    Snapshot s;
    xSemaphoreTake(self->snapMtx_, portMAX_DELAY);
    s = self->snap_;
    xSemaphoreGive(self->snapMtx_);

    char buf[512];
    int  len = formatStatsJson(&s, (int)Log::log().getLevel(),
                                AUTOLINK_VERSION, buf, sizeof(buf));

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// HTTP handler: GET /logs?since=N
// Returns {"lines":[{"seq":N,"sev":"I","text":"..."},...]}
// Client polls with ?since=lastSeq for incremental updates.
// ---------------------------------------------------------------------------

esp_err_t AutoLinkWeb::handleLogs(httpd_req_t* req) {
    AutoLinkWeb* self = (AutoLinkWeb*)req->user_ctx;

    // Parse optional ?since=N query parameter.
    char     query[48] = {};
    uint32_t since     = 0;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[20] = {};
        if (httpd_query_key_value(query, "since", val, sizeof(val)) == ESP_OK) {
            since = (uint32_t)strtoul(val, nullptr, 10);
        }
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "close");
    // First chunk includes the current `head` (the seq of the next
    // entry to be assigned). The client uses this on first poll to
    // skip the boot-time backlog — after the first response the
    // client sets lastSeq = head and renders nothing. Subsequent
    // polls get only entries that arrive after page load.
    char head[32];
    snprintf(head, sizeof(head), "{\"head\":%lu,\"lines\":[",
             (unsigned long)self->logHead_);
    httpd_resp_sendstr_chunk(req, head);

    bool first = true;
    if (xSemaphoreTake(self->logMtx_, pdMS_TO_TICKS(50)) == pdTRUE) {
        const uint32_t total = self->logHead_;
        // Walk only the entries that fit in the ring, starting at 'since'.
        uint32_t start = (total > (uint32_t)RING_CAP) ? (total - RING_CAP) : 0;
        if (since > start) start = since;

        // chunk: JSON wrapper + worst-case 2× escaped line + closing characters.
        char chunk[LINE_CAP * 2 + 64];

        for (uint32_t i = start; i < total; i++) {
            const LogEntry& e = self->logRing_[i % RING_CAP];
            if (e.seq != i) continue; // defensive: slot was overwritten

            if (!first) httpd_resp_sendstr_chunk(req, ",");
            first = false;

            int pos = snprintf(chunk, sizeof(chunk),
                "{\"seq\":%lu,\"sev\":\"%c\",\"text\":\"",
                (unsigned long)i, e.sev);

            // JSON-escape the log line in-place.
            for (const char* p = e.line; *p && pos < (int)(sizeof(chunk) - 4); p++) {
                switch (*p) {
                    case '"':  chunk[pos++] = '\\'; chunk[pos++] = '"';  break;
                    case '\\': chunk[pos++] = '\\'; chunk[pos++] = '\\'; break;
                    case '\n': chunk[pos++] = '\\'; chunk[pos++] = 'n';  break;
                    case '\r': chunk[pos++] = '\\'; chunk[pos++] = 'r';  break;
                    default:   chunk[pos++] = *p;                         break;
                }
            }
            chunk[pos++] = '"';
            chunk[pos++] = '}';
            if (httpd_resp_send_chunk(req, chunk, pos) != ESP_OK) {
                // Client disconnected mid-response; release the mutex and bail.
                xSemaphoreGive(self->logMtx_);
                return ESP_FAIL;
            }
        }
        xSemaphoreGive(self->logMtx_);
    }

    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_send_chunk(req, nullptr, 0); // terminate chunked transfer
    return ESP_OK;
}

// HTTP handler: POST /reset
// Calls resetStats(), resetErrors(), and resetDiag() on the AutoLink
// instance, then zeroes the sampler's prevTx_/prevRx_ so the next B/s
// reading is 0 rather than a spurious spike caused by the counters
// restarting from 0.
esp_err_t AutoLinkWeb::handleReset(httpd_req_t* req) {
    AutoLinkWeb* self = (AutoLinkWeb*)req->user_ctx;
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

// HTTP handler: POST /level?lv=N
// Sets the singleton Log level to N (0=None..5=Verbose). The dashboard's
// log-level radio group POSTs here whenever the user picks a different
// level. Invalid or missing values are rejected with 400. The chosen
// level is persisted to NVS so the next reboot restores it.
esp_err_t AutoLinkWeb::handleLevel(httpd_req_t* req) {
    char     query[48] = {};
    char     val[8]    = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK
        || httpd_query_key_value(query, "lv", val, sizeof(val)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "missing ?lv=", 13);
        return ESP_OK;
    }
    // Parse + validate via the host-testable core helper. Returns:
    //   >= 0: valid level, applied to the Log singleton
    //   -1:  missing or malformed input
    //   -2:  lv == NONE (rejected — silences the logger)
    //   -3:  out of range (> VERBOSE)
    int rc = applyLogLevel(parseLevelQuery(val));
    if (rc < 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        if (rc == -2) {
            httpd_resp_send(req, "lv=0 (NONE) is rejected: silences all logs "
                                  "and is unrecoverable without reflash", 79);
        } else {
            httpd_resp_send(req, "lv must be 1..5 (ERROR..VERBOSE)", 33);
        }
        return ESP_OK;
    }
    int lv = rc;
    Log::log().info(TAG, "Log level set to %d via web", lv);
    // Persist to NVS. A write failure (NVS full, Preferences layer
    // unavailable) logs a warning but still returns ok — the in-memory
    // level is set either way; the user just won't get persistence.
    {
        Preferences prefs;
        if (prefs.begin("autolink", false)) {  // read-write
            if (prefs.putUChar("log_level", (uint8_t)lv) == 0) {
                Log::log().warning(TAG,
                    "failed to persist log level %d to NVS", lv);
            }
            prefs.end();
        } else {
            Log::log().warning(TAG,
                "could not open NVS namespace to persist log level");
        }
    }
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, "ok", 2);
    return ESP_OK;
}

// HTTP handler: POST /mode?m=seq|rand
// Dashboard's Sequential/Random radio POSTs here. Ping side has a
// fillMode/setFillMode pair wired via the hook; Pong side leaves the
// hook null (mode is meaningless when echoing) so we 404.
esp_err_t AutoLinkWeb::handleMode(httpd_req_t* req) {
    char query[48] = {};
    char val[8]    = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK
        || httpd_query_key_value(query, "m", val, sizeof(val)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "missing ?m=", 12);
        return ESP_OK;
    }
    auto* self = static_cast<AutoLinkWeb*>(req->user_ctx);
    if (!self->fillModeWriter_) {
        // No hook registered — likely the Pong side, where the mode
        // selector is meaningless. Return 404 so the dashboard's
        // "save" attempt can be detected and ignored.
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "no fill mode (Pong?)", 22);
        return ESP_OK;
    }
    uint8_t mode = 99;
    if (strcmp(val, "seq") == 0)        mode = 0;
    else if (strcmp(val, "rand") == 0)  mode = 1;
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

// HTTP handler: POST /pausemsg?p=1|0
// v5.1.29: device-side message pause. Ping registers a writer hook
// that flips its `paused_` flag; when true, Ping's loop() returns
// before pumping bytes. The Pause/Resume button in the dashboard now
// hits this endpoint instead of being a JS-only toggle. The previous
// behavior had Ping blasting bytes from boot regardless of button
// state — the button only paused log polling, not message sending.
esp_err_t AutoLinkWeb::handleMsgPause(httpd_req_t* req) {
    char query[48] = {};
    char val[8]    = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK
        || httpd_query_key_value(query, "p", val, sizeof(val)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "missing ?p=", 12);
        return ESP_OK;
    }
    auto* self = static_cast<AutoLinkWeb*>(req->user_ctx);
    if (!self->msgPausedWriter_) {
        // No hook registered — likely the Pong side. 404 so the
        // dashboard's button click can be detected and ignored.
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "no msg pause (Pong?)", 22);
        return ESP_OK;
    }
    bool paused = false;
    if (strcmp(val, "1") == 0 || strcmp(val, "true") == 0) paused = true;
    else if (strcmp(val, "0") == 0 || strcmp(val, "false") == 0) paused = false;
    else {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_send(req, "p must be '1', '0', 'true', or 'false'", 38);
        return ESP_OK;
    }
    self->msgPausedWriter_(paused);
    Log::log().info(TAG, "Message pause set to %s via web", paused ? "PAUSED" : "RESUMED");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, paused ? "paused" : "resumed", paused ? 6 : 7);
    return ESP_OK;
}

// HTTP handler: POST /reboot
// Sends the response first, then restarts the chip after a short delay so the
// "ok" reply reaches the browser before the connection drops. esp_restart()
// never returns; the device boots fresh and the dashboard JS polls /stats
// until it answers again, then reloads.
esp_err_t AutoLinkWeb::handleReboot(httpd_req_t* req) {
    Log::log().info("ALinkWeb", "Reboot requested via web — restarting in 200 ms");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, "rebooting", 9);
    vTaskDelay(pdMS_TO_TICKS(200));  // let the TCP response flush
    esp_restart();
    return ESP_OK;  // unreachable
}

} // namespace autolink
#endif // ARDUINO
