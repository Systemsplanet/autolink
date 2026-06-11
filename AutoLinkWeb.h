// AutoLinkWeb.h — optional WiFi web monitor for AutoLink (Arduino/ESP32 only).
//
// Include this header alongside AutoLink.h when you want the live dashboard.
// Omitting it keeps WiFi headers entirely out of builds that don't need them.
#pragma once
#ifdef ARDUINO

#include "AutoLink.h"
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
    static constexpr int         RING_CAP       = 48;     // log ring capacity (entries)
    static constexpr int         LINE_CAP       = 180;    // max bytes per log line (incl. NUL)
    static constexpr uint32_t    WIFI_TIMEOUT_MS = 12000; // max ms to wait for WiFi
    static constexpr const char* TAG            = "ALinkWeb";

    // ---- internal types ----
    struct LogEntry {
        uint32_t seq;
        char     sev;            // 'E', 'I', 'D'
        char     line[LINE_CAP]; // formatted "[E][Tag] message"
    };

    struct Snapshot {
        char     state[4];       // "OK", "SWP", "LCK" + NUL
        int      errCount;       // current per-link error counter
        uint32_t txBps, rxBps;   // bytes/s since last sample
        uint64_t txTotal, rxTotal, errTotal; // cumulative
        int32_t  rssi;           // WiFi RSSI dBm
        uint32_t freeHeap;       // ESP heap bytes free
        uint32_t uptimeS;        // millis()/1000
    };

    // ---- members ----
    AutoLink&          link_;
    uint16_t           port_      = 8765;
    bool               enabled_   = false;

    // Stats snapshot — written by 1 Hz timer, read by /stats handler.
    Snapshot           snap_      = {};
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
};

} // namespace autolink
#endif // ARDUINO
