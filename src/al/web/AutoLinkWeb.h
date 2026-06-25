// Arduino-only: WiFi STA, esp_http_server,
// live dashboard. Core in AutoLinkWebCore.
#pragma once
#ifdef ARDUINO

#    include "AutoLink.h"
#    include "al/web/AutoLinkWebCore.h"
#    include <WiFi.h>
#    include "esp_http_server.h"
#    include "esp_timer.h"
#    include "freertos/semphr.h"
#    include <functional>

namespace autolink {
class AutoLinkWeb {
public:
    explicit AutoLinkWeb(AutoLink &link);
    ~AutoLinkWeb();

    void setRole(const char *role);

    using FillModeReader = std::function<uint8_t()>;
    using FillModeWriter = std::function<void(uint8_t)>;
    void setFillModeHook(FillModeReader r, FillModeWriter w) {
        fillModeReader_ = std::move(r);
        fillModeWriter_ = std::move(w);
    }

    using MsgPausedReader = std::function<bool()>;
    using MsgPausedWriter = std::function<void(bool)>;
    void setMsgPauseHook(MsgPausedReader r, MsgPausedWriter w) {
        msgPausedReader_ = std::move(r);
        msgPausedWriter_ = std::move(w);
    }

    using TxDelayReader = std::function<int()>;
    using TxDelayWriter = std::function<void(int)>;
    void setTxDelayHook(TxDelayReader r, TxDelayWriter w) {
        txDelayReader_ = std::move(r);
        txDelayWriter_ = std::move(w);
    }

    bool begin(const char *ssid, const char *password, uint16_t port = 8765);

    bool isUp() const { return enabled_; }
    String ip() const;

private:
    static constexpr int RING_CAP = 200;
    static constexpr int LINE_CAP = 180;
    static constexpr uint32_t WIFI_BG_TIMEOUT_MS = 10000;
    static constexpr uint32_t WIFI_BG_TICK_MS = 250;
    static constexpr uint32_t WIFI_TIMEOUT_MS = 12000;
    static constexpr uint32_t WIFI_BEGIN_QUICK_MS = 5000;
    static constexpr uint32_t HTTPD_BEGIN_QUICK_MS =
        75000; // 14 * 5s = 70s covers lwIP TIME_WAIT (~60s)
    static constexpr uint32_t WIFI_RETRY_BACKOFF_MS_MIN = 1000;
    static constexpr uint32_t WIFI_RETRY_BACKOFF_MS_MAX = 30000;
    static constexpr uint32_t HTTPD_RETRY_DELAY_MS = 250;
    static constexpr uint32_t HTTPD_RETRY_MAX = 14;
    static constexpr uint32_t HTTPD_RETRY_BG_MS = 1000;
    static constexpr uint32_t HTTPD_RETRY_PRE_MS =
        5000; // TIME_WAIT is ~60s worst-case; 5s per attempt
    static constexpr const char *TAG = "ALinkWeb";

    struct LogEntry {
        uint32_t seq;
        char sev;
        char line[LINE_CAP];
    };

    using Snapshot = WebSnapshot;

    AutoLink &link_;
    Log &log_;
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

    TxDelayReader txDelayReader_ = nullptr;
    TxDelayWriter txDelayWriter_ = nullptr;
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

    static void logSinkCb(char sev, const char *tag, const char *msg,
                          void *ctx);

    static esp_err_t handleRoot(httpd_req_t *req);
    static esp_err_t handleStats(httpd_req_t *req);
    static esp_err_t handleLogs(httpd_req_t *req);
    static esp_err_t handleReset(httpd_req_t *req);
    static esp_err_t handleLevel(httpd_req_t *req);
    static esp_err_t handleMode(httpd_req_t *req);
    static esp_err_t handleMsgPause(httpd_req_t *req);
    static esp_err_t handleDelay(httpd_req_t *req);
    static esp_err_t handleReboot(httpd_req_t *req);
};

} // namespace autolink
#endif