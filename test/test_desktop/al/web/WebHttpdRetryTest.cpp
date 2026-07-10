// Auto-generated split of the original
// HandleRootChunkedTest.cpp. Each TU in this
// split covers a single concern (handleRoot
// chunked send / begin lifecycle / httpd retry
// budget / Link kickoff defer) and includes
// the shared helpers via
// HandleRootChunkedTestCommon.h.
#include "HandleRootChunkedTestCommon.h"
#include "al/util/Log.h"

using namespace autolink;

void test_setup_httpd_retries_on_failure() {
    std::cout << "\n=== setupHttpAndLogging_ retries httpd_start ==="
              << std::endl;
    std::string src = readFile(projectRoot() + "/src/al/web/AutoLinkWeb.cpp");
    assert(!src.empty());

    auto setupPos = src.find("bool AutoLinkWeb::setupHttpAndLogging_(");
    assert(setupPos != std::string::npos);

    // Find the matching closing brace by counting braces.
    size_t scan = setupPos;
    int depth = 0;
    bool foundOpen = false;
    size_t endPos = std::string::npos;
    for (; scan < src.size(); scan++) {
        if (src[scan] == '{') {
            depth++;
            foundOpen = true;
        } else if (src[scan] == '}') {
            depth--;
            if (foundOpen && depth == 0) {
                endPos = scan;
                break;
            }
        }
    }
    assert(endPos != std::string::npos);
    std::string body = src.substr(setupPos, endPos - setupPos + 1);

    auto httpdPos = body.find("httpd_start(&server_");
    assert(httpdPos != std::string::npos);

    // The retry constants must be referenced in setupHttpAndLogging_.
    assert(body.find("HTTPD_RETRY_MAX") != std::string::npos);
    assert(body.find("HTTPD_RETRY_PRE_MS") != std::string::npos);

    // There must be a for-loop wrapping httpd_start with
    // bounded attempts (not a single if).
    auto loopStart = body.rfind("for", httpdPos);
    assert(loopStart != std::string::npos);
    assert(loopStart < httpdPos);

    // Each attempt must sleep HTTPD_RETRY_PRE_MS as a
    // per-attempt prefix BEFORE httpd_start (TIME_WAIT
    // settle). The delay must appear inside the for-loop
    // body and before the httpd_start call.
    auto delayPos = body.find("vTaskDelay(pdMS_TO_TICKS(HTTPD_RETRY_PRE_MS))");
    assert(delayPos != std::string::npos);
    assert(loopStart < delayPos);
    assert(delayPos < httpdPos);

    std::cout << "  PASS (httpd_start wrapped in HTTPD_RETRY_MAX "
                 "attempt loop with per-attempt HTTPD_RETRY_PRE_MS settle)\n";
}

// Pin: wifiTaskThunk_() must retry WiFi forever (with
// capped exponential backoff) when ssid+password are
// provided. The previous behavior gave up after
// WIFI_BG_TIMEOUT_MS (10 s) — that left the device
// unreachable on a flaky network until reboot.

void test_wifi_task_retries_forever_when_creds_given() {
    std::cout << "\n=== wifiTaskThunk_ retries forever when creds given ==="
              << std::endl;
    std::string src = readFile(projectRoot() + "/src/al/web/AutoLinkWeb.cpp");
    assert(!src.empty());

    auto wifiPos = src.find("void AutoLinkWeb::wifiTaskThunk_(");
    assert(wifiPos != std::string::npos);

    // Find the closing brace.
    size_t scan = wifiPos;
    int depth = 0;
    bool foundOpen = false;
    size_t endPos = std::string::npos;
    for (; scan < src.size(); scan++) {
        if (src[scan] == '{') {
            depth++;
            foundOpen = true;
        } else if (src[scan] == '}') {
            depth--;
            if (foundOpen && depth == 0) {
                endPos = scan;
                break;
            }
        }
    }
    assert(endPos != std::string::npos);
    std::string body = src.substr(wifiPos, endPos - wifiPos + 1);

    // The credentials check (haveCreds) must branch the
    // behavior between offline-mode (no creds) and
    // retry-forever (creds present).
    assert(body.find("haveCreds") != std::string::npos);
    assert(body.find("WIFI_RETRY_BACKOFF_MS_MIN") != std::string::npos);
    assert(body.find("WIFI_RETRY_BACKOFF_MS_MAX") != std::string::npos);

    // The retry path must have a backoff that doubles and
    // caps at WIFI_RETRY_BACKOFF_MS_MAX.
    assert(body.find("backoffMs = backoffMs * 2") != std::string::npos);
    assert(body.find("backoffMs > WIFI_RETRY_BACKOFF_MS_MAX") !=
           std::string::npos);

    // And the outer loop must be `while (true)` — the
    // retry must be unbounded when creds are given.
    assert(body.find("while (true)") != std::string::npos);

    std::cout << "  PASS (wifiTaskThunk_ retries forever with capped "
                 "backoff when creds given)\n";
}

// Pin: setupHttpAndLogging_() must wait HTTPD_RETRY_PRE_MS
// before the first httpd_start attempt. On reboot the
// prior boot's httpd socket is in TIME_WAIT for up to
// ~60 s — a bare call right after boot races that and
// returns EADDRINUSE / ESP_ERR_HTTPD_ALLOC. The pre-delay
// gives the kernel time to clear the socket.

void test_setup_httpd_pre_delay() {
    std::cout << "\n=== setupHttpAndLogging_ waits HTTPD_RETRY_PRE_MS "
                 "before first httpd_start ==="
              << std::endl;
    std::string src = readFile(projectRoot() + "/src/al/web/AutoLinkWeb.cpp");
    assert(!src.empty());

    auto setupPos = src.find("bool AutoLinkWeb::setupHttpAndLogging_(");
    assert(setupPos != std::string::npos);
    size_t scan = setupPos;
    int depth = 0;
    bool foundOpen = false;
    size_t endPos = std::string::npos;
    for (; scan < src.size(); scan++) {
        if (src[scan] == '{') {
            depth++;
            foundOpen = true;
        } else if (src[scan] == '}') {
            depth--;
            if (foundOpen && depth == 0) {
                endPos = scan;
                break;
            }
        }
    }
    assert(endPos != std::string::npos);
    std::string body = src.substr(setupPos, endPos - setupPos + 1);

    auto prePos = body.find("HTTPD_RETRY_PRE_MS");
    assert(prePos != std::string::npos);
    auto delayPos = body.find("vTaskDelay(pdMS_TO_TICKS(HTTPD_RETRY_PRE_MS))");
    assert(delayPos != std::string::npos);
    // Find the actual httpd_start() function call (not
    // any prose mention). Pattern: "httpd_start(" with
    // an open paren immediately after.
    auto httpdPos = body.find("httpd_start(&server_");
    assert(httpdPos != std::string::npos);

    // The pre-delay must happen BEFORE the first
    // httpd_start() call.
    assert(delayPos < httpdPos);

    std::cout << "  PASS (HTTPD_RETRY_PRE_MS pre-delay fires before "
                 "first httpd_start)\n";
}

// Pin: the fail: block must NOT destroy the begin()-
// lifetime resources (logRing_, snapMtx_, logMtx_).
// Destroying them makes the bg-task retry of
// setupHttpAndLogging_ dereference a null logMtx_
// inside logSinkCb on the very next log call. They
// are owned by the AutoLinkWeb instance and freed
// only in the destructor.

int main() {
    std::cout << "=== Running WebHttpdRetryTest ===" << std::endl;

    Log::log().setLevel(Log::DEBUG);
    test_setup_httpd_retries_on_failure();
    test_wifi_task_retries_forever_when_creds_given();
    test_setup_httpd_pre_delay();

    std::cout << "\n=== WebHttpdRetryTest Completed ===" << std::endl;
    return 0;
}
