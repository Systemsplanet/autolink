// AutoLinkWeb.h — optional WiFi web monitor for AutoLink (Arduino/ESP32 only).
//
// Include this header alongside AutoLink.h when you want the live dashboard.
// Omitting it keeps WiFi headers entirely out of builds that don't need them.
#pragma once
#ifdef ARDUINO

#include "AutoLink.h"
#include "al/web/AutoLinkWebCore.h"   // WebSnapshot (used directly by /stats handler)
#include <WiFi.h>
#include "esp_http_server.h"
#include "esp_timer.h"
#include "freertos/semphr.h"

namespace autolink {

// ---------------------------------------------------------------------------
// AutoLinkWeb — optional WiFi web monitor for AutoLink.
//
// Connects to WiFi and serves a mobile-friendly real-time dashboard showing
// TX/RX B/s, cumulative totals, error counts, lifetime disconnects, link-
// state pill, WiFi RSSI, free heap, uptime, and a live scrolling log panel.
//
// Usage:
//   AutoLink    comm(UART_NUM_2, 16, 17, true);
//   AutoLinkWeb mon(comm);
//
//   void setup() {
//       comm.begin();
//       mon.begin("YourSSID", "password");       // default port 8765
//       // mon.begin("YourSSID", "password", 80); // custom port
//   }
//
// If WiFi connection fails the HTTP server does not start, isUp() returns
// false, and the UART core continues unaffected. Nothing in loop() needs
// to change — the monitor is fully self-contained.
//
// Endpoints (open, no authentication):
//   GET  /        mobile HTML dashboard (all CSS + JS inline, no CDN)
//   GET  /stats   JSON snapshot updated once per second
//   GET  /logs    JSON log ring; use ?since=N for incremental polling
//   POST /reset   calls resetStats() + resetErrors() on the AutoLink instance
//
// B/s sampling: the monitor snapshots the cumulative getStats() counters on
// its own 1 Hz timer and computes deltas. It never calls resetStats(), so
// sketches that call it for their own logging won't corrupt the B/s display
// (the reading will be 0 B/s for one interval, then recover).
// ---------------------------------------------------------------------------
class AutoLinkWeb {
public:
    explicit AutoLinkWeb(AutoLink& link);
    ~AutoLinkWeb();

    // Tell the monitor which role the sketch is running (Ping vs
    // Pong). Dashboard renders the label at the top of the page.
    // Safe to call once from setup() before begin(); "" clears.
    void setRole(const char* role);

    // Optional fill-mode hook for the Ping side. AutoLinkWeb doesn't
    // depend on UtilPing, so we take two C function pointers: one to
    // read the current mode (1 Hz timer) and one to set it (POST /mode).
    // Both null by default; if null, mode field is 0 and /mode 404s.
    using FillModeReader = uint8_t (*)();
    using FillModeWriter = void (*)(uint8_t);
    void setFillModeHook(FillModeReader r, FillModeWriter w) {
        fillModeReader_ = r; fillModeWriter_ = w;
    }

    // Message-pause hook (v5.1.29): Ping registers a reader that the
    // /stats snapshot calls once per second to read the current
    // paused state. The dashboard reads it from /stats and reflects it
    // in the Pause/Resume button. POST /pausemsg?p=1|0 calls the
    // writer hook to flip the device-side state. Previously the
    // Pause button was purely cosmetic (toggled a JS variable that
    // only affected log polling) — Ping kept blasting bytes regardless.
    using MsgPausedReader = bool (*)();
    using MsgPausedWriter = void (*)(bool);
    void setMsgPauseHook(MsgPausedReader r, MsgPausedWriter w) {
        msgPausedReader_ = r; msgPausedWriter_ = w;
    }

    // Attempt WiFi connection and start the HTTP server.
    // Logs the SSID and the *length* of the password — never the password itself.
    // Blocks in setup() for up to ~12 s while connecting; returns immediately
    // on success or failure.
    // Returns true when the server is up; false if WiFi failed (server disabled).
    bool begin(const char* ssid, const char* password, uint16_t port = 8765);

    bool   isUp() const { return enabled_; }
    String ip()   const;

private:
    // ---- tunables ----
    static constexpr int         RING_CAP       = 200;    // log ring capacity (entries)
    static constexpr int         LINE_CAP       = 180;    // max bytes per log line (incl. NUL)
    static constexpr uint32_t    WIFI_TIMEOUT_MS    = 12000; // max ms to wait per WiFi attempt
    static constexpr uint32_t    WIFI_RETRY_BACKOFF_MS = 5000; // backoff between failed attempts
    static constexpr int         WIFI_RETRY_MAX_ATTEMPTS = 0;   // 0 = retry forever; >0 = cap
    static constexpr uint32_t    WIFI_RETRY_LOG_INTERVAL_MS = 30000; // log progress once per 30 s
    static constexpr const char* TAG            = "ALinkWeb";

    // ---- internal types ----
    struct LogEntry {
        uint32_t seq;
        char     sev;            // 'E', 'I', 'D'
        char     line[LINE_CAP]; // formatted "[E][Tag] message"
    };

    // Stats snapshot — written by 1 Hz timer, read by /stats handler.
    // We use WebSnapshot directly (defined in AutoLinkWebCore.h) so the
    // timer and the handler share the same layout — no field-by-field
    // copy. The previous design had a parallel `Snapshot` type that
    // drifted silently; this is the single source of truth.
    using Snapshot = WebSnapshot;

    // ---- members ----
    AutoLink&          link_;
    uint16_t           port_      = 8765;
    bool               enabled_   = false;
    bool               ntpSynced_ = false; // true once SNTP wall-clock is valid

    Snapshot           snap_      = {};

    // Optional hooks to the Ping side (set via setFillModeHook).
    // When null, the mode field is 0 (sequential default) and /mode 404s.
    FillModeReader     fillModeReader_ = nullptr;
    FillModeWriter     fillModeWriter_ = nullptr;

    // Optional hooks for message-pause (set via setMsgPauseHook).
    // When null, the dashboard treats Pause as JS-only (cosmetic) and
    // /pausemsg returns 404. When set, the device actually pauses.
    MsgPausedReader    msgPausedReader_ = nullptr;
    MsgPausedWriter    msgPausedWriter_ = nullptr;
    uint64_t           prevTx_    = 0;
    uint64_t           prevRx_    = 0;
    SemaphoreHandle_t  snapMtx_   = nullptr;
    esp_timer_handle_t statTimer_ = nullptr;

    // Log ring — written by the Log sink (any task), read by /logs handler.
    LogEntry*          logRing_   = nullptr; // heap-allocated in begin()
    uint32_t           logHead_   = 0;       // next-write slot (monotonic counter)
    SemaphoreHandle_t  logMtx_    = nullptr;

    httpd_handle_t     server_    = nullptr;

    // 1 Hz stats snapshot (esp_timer task).
    static void statTimerCb(void* arg);

    // Log sink registered with Log::setSink() on success.
    // Called from Log::emit() in any task — must be fast and non-blocking.
    static void logSinkCb(char sev, const char* tag, const char* msg, void* ctx);

    // HTTP handlers.
    static esp_err_t handleRoot (httpd_req_t* req);
    static esp_err_t handleStats(httpd_req_t* req);
    static esp_err_t handleLogs (httpd_req_t* req);
    static esp_err_t handleReset(httpd_req_t* req); // POST /reset — calls resetStats()+resetErrors()
    static esp_err_t handleLevel(httpd_req_t* req); // POST /level?lv=N — set log level (0=Error,1=Info,2=Debug)
    static esp_err_t handleMode (httpd_req_t* req); // POST /mode?m=seq|rand — fill mode (Ping only)
    static esp_err_t handleMsgPause(httpd_req_t* req); // POST /pausemsg?p=1|0 — message pause (Ping only)
    static esp_err_t handleReboot(httpd_req_t* req); // POST /reboot — esp_restart()
};

} // namespace autolink
#endif // ARDUINO
