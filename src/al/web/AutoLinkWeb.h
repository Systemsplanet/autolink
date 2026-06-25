// ARDUINO-only glue: WiFi STA connect, httpd server,
// dashboard JSON snapshot. The protocol-agnostic core
// (ring log + JSON formatter + state) lives in
// AutoLinkWebCore and is host-tested.
#pragma once
#ifdef ARDUINO

#    include "AutoLink.h"
#    include "al/web/AutoLinkWebCore.h"
#    include <WiFi.h>
#    include "esp_http_server.h"
#    include "esp_timer.h"
#    include "freertos/semphr.h"

namespace autolink
{
class AutoLinkWeb
{
public:
    explicit AutoLinkWeb(AutoLink &link);
    ~AutoLinkWeb();


    void setRole(const char *role);


    using FillModeReader = uint8_t (*)();
    using FillModeWriter = void (*)(uint8_t);
    void setFillModeHook(FillModeReader r,
                         FillModeWriter w)
    {
        fillModeReader_ = r;
        fillModeWriter_ = w;
    }


    using MsgPausedReader = bool (*)();
    using MsgPausedWriter = void (*)(bool);
    void setMsgPauseHook(MsgPausedReader r,
                         MsgPausedWriter w)
    {
        msgPausedReader_ = r;
        msgPausedWriter_ = w;
    }


    bool begin(const char *ssid, const char *password,
               uint16_t port = 8765);

    bool isUp() const { return enabled_; }
    String ip() const;

private:
    static constexpr int RING_CAP = 200;
    static constexpr int LINE_CAP = 180;
    static constexpr uint32_t WIFI_BG_TIMEOUT_MS =
        10000;
    static constexpr uint32_t WIFI_BG_TICK_MS = 250;
    static constexpr uint32_t WIFI_TIMEOUT_MS = 12000;
    static constexpr const char *TAG = "ALinkWeb";

    struct LogEntry {
        uint32_t seq;
        char sev;
        char line[LINE_CAP];
    };

    using Snapshot = WebSnapshot;

    AutoLink &link_;
    uint16_t port_ = 8765;
    bool enabled_ = false;
    bool ntpSynced_ = false;

    char *ssid_ = nullptr;
    char *pass_ = nullptr;

    Snapshot snap_ = {};

    FillModeReader fillModeReader_ = nullptr;
    FillModeWriter fillModeWriter_ = nullptr;

    MsgPausedReader msgPausedReader_ = nullptr;
    MsgPausedWriter msgPausedWriter_ = nullptr;
    uint64_t prevTx_ = 0;
    uint64_t prevRx_ = 0;
    SemaphoreHandle_t snapMtx_ = nullptr;
    esp_timer_handle_t statTimer_ = nullptr;

    LogEntry *logRing_ = nullptr;
    uint32_t logHead_ = 0;
    SemaphoreHandle_t logMtx_ = nullptr;

    httpd_handle_t server_ = nullptr;

    TaskHandle_t wifiTask_ = nullptr;
    static void wifiTaskThunk_(void *arg);

    bool setupHttpAndLogging_();

    static void statTimerCb(void *arg);


    static void logSinkCb(char sev, const char *tag,
                          const char *msg, void *ctx);

    static esp_err_t handleRoot(httpd_req_t *req);
    static esp_err_t handleStats(httpd_req_t *req);
    static esp_err_t handleLogs(httpd_req_t *req);
    static esp_err_t handleReset(httpd_req_t *req);
    static esp_err_t handleLevel(httpd_req_t *req);
    static esp_err_t handleMode(httpd_req_t *req);
    static esp_err_t handleMsgPause(httpd_req_t *req);
    static esp_err_t handleReboot(httpd_req_t *req);
};

} // namespace autolink
#endif