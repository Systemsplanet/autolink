
#pragma once

#include "al/web/AutoLinkWebCore.h"

#ifdef ARDUINO

#    include "AutoLink.h"
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

    // Cap the link's maxMsg BEFORE the link begins, so the
    // buffer floors are sized for the smaller ask. The web
    // monitor's default cap of 2048 fits the dashboard's /stats
    // JSON comfortably and frees ~9 KB across streamBuf + txBuf
    // on a 41 KB post-alloc free heap (rxBuf is unchanged — it
    // depends on the ARQ pipeline window, not maxMsg). Without
    // this cap, the heap accounting cannot leave enough room for
    // httpd / WiFi on a 41 KB device and the GUI wedged on
    // boot. Pinned by EspHalHeapAccountingTest.
    //
    // The ctor applies the default cap; setLinkMaxMsg() called
    // before link.begin() overrides it. After link.begin() the
    // buffer floors are committed and a later setLinkMaxMsg() is
    // a no-op.
    static constexpr size_t kDefaultWebMaxMsgCap = 2048;
    void setLinkMaxMsg(size_t cap);

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
    // Single source of truth for the web log ring's capacity and
    // per-line size lives in AutoLinkWebCore.h (WEB_RING_CAP /
    // WEB_LINE_CAP) so the Arduino-only httpd path here and the
    // host-testable AutoLinkWebCore.cpp path can't drift apart —
    // the two duplicated these as separate literals.
    static constexpr int RING_CAP = WEB_RING_CAP;
    static constexpr int LINE_CAP = WEB_LINE_CAP;
    static_assert(RING_CAP == WEB_RING_CAP && LINE_CAP == WEB_LINE_CAP,
                  "AutoLinkWeb ring caps must alias AutoLinkWebCore.h");
    static constexpr uint32_t WIFI_BG_TIMEOUT_MS = 10000;
    static constexpr uint32_t WIFI_BG_TICK_MS = 250;
    static constexpr uint32_t WIFI_TIMEOUT_MS = 12000;
    static constexpr uint32_t WIFI_BEGIN_QUICK_MS = 5000;
    static constexpr uint32_t HTTPD_BEGIN_QUICK_MS = 75000;
    static constexpr uint32_t WIFI_RETRY_BACKOFF_MS_MIN = 1000;
    static constexpr uint32_t WIFI_RETRY_BACKOFF_MS_MAX = 30000;
    static constexpr uint32_t HTTPD_RETRY_DELAY_MS = 250;
    static constexpr uint32_t HTTPD_RETRY_MAX = 14;
    static constexpr uint32_t HTTPD_RETRY_BG_MS = 1000;
    static constexpr uint32_t HTTPD_RETRY_PRE_MS = 5000;
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
    // Cap applied to the link's maxMsg before begin() so the
    // heap accounting can fit on the field device. See
    // setLinkMaxMsg() above.
    size_t webMaxMsgCap_ = kDefaultWebMaxMsgCap;

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
    // AL87-02: logSinkCb silently returned when the 5ms mutex
    // take failed — the line vanished with no counter, no warning,
    // nothing in /stats. Distinct from AL87-01 (ring overrun,
    // which at least still has a seq gap to detect): this is a
    // producer-side drop the ring never even saw. Sticky-warned
    // once, then counted forever after. Pinned by
    // WebLogProducerDropCountedTest.
    uint32_t logDropped_ = 0;
    bool logDropWarned_ = false;

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
    static esp_err_t handleFillMode(httpd_req_t *req);
    static esp_err_t handleMsgPause(httpd_req_t *req);
    static esp_err_t handleDelay(httpd_req_t *req);
    static esp_err_t handleReboot(httpd_req_t *req);
    static esp_err_t handleOtaFw(httpd_req_t *req);
    static esp_err_t handleOtaGui(httpd_req_t *req);

    bool fsOk_ = false;
    bool mountFs_();

    static void drainBody_(httpd_req_t *req, size_t remaining);
};

} // namespace autolink
#endif
