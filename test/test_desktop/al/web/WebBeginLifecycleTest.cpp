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

void test_begin_wires_log_sink_first() {
    std::cout
        << "\n=== setSink fires in begin() before setupHttpAndLogging_ ==="
        << std::endl;
    std::string src = readFile(projectRoot() + "/src/al/web/AutoLinkWeb.cpp");
    assert(!src.empty());

    auto beginPos = src.find("bool AutoLinkWeb::begin(");
    auto setupPos = src.find("bool AutoLinkWeb::setupHttpAndLogging_(");
    assert(beginPos != std::string::npos);
    assert(setupPos != std::string::npos);

    auto setSinkPos = src.find("setSink(logSinkCb, this)");
    assert(setSinkPos != std::string::npos);

    // The first occurrence of setSink must be inside
    // begin(), not inside setupHttpAndLogging_().
    assert(setSinkPos < setupPos);
    assert(setSinkPos > beginPos);

    // And there must be no second setSink call inside
    // setupHttpAndLogging_() (the responsibility moved to
    // begin() so the sink is wired once, before any
    // worker-thread log emission).
    auto tail = src.substr(setupPos);
    assert(tail.find("setSink(logSinkCb, this)") == std::string::npos);

    std::cout << "  PASS (setSink wired in begin, not in "
                 "setupHttpAndLogging_)\n";
}

// Pin: begin() must emit the version line (the line that
// shows up as `AutoLink: vX.Y.Z`) immediately after the
// sink is wired, so it's the first line in the live log
// when the user opens the GUI.

void test_begin_logs_version_line_after_sink() {
    std::cout << "\n=== begin() emits version line right after setSink ==="
              << std::endl;
    std::string src = readFile(projectRoot() + "/src/al/web/AutoLinkWeb.cpp");
    assert(!src.empty());

    auto beginPos = src.find("bool AutoLinkWeb::begin(");
    auto setupPos = src.find("bool AutoLinkWeb::setupHttpAndLogging_(");
    assert(beginPos != std::string::npos);
    assert(setupPos != std::string::npos);

    std::string body = src.substr(beginPos, setupPos - beginPos);

    auto setSinkPos = body.find("setSink(logSinkCb, this)");
    assert(setSinkPos != std::string::npos);

    auto versionPos = body.find("AUTOLINK_VERSION");
    assert(versionPos != std::string::npos);
    assert(versionPos > setSinkPos);

    // The version log must precede any log.info call that
    // carries meaningful boot diagnostics. The first such
    // call in begin() is the NVS open info log — the
    // version line must precede it.
    auto nvsLogPos = body.find("NVS open returned", versionPos);
    assert(nvsLogPos != std::string::npos);
    assert(versionPos < nvsLogPos);

    std::cout << "  PASS (version line is first log emitted by begin())\n";
}

// Pin: begin() must block until the web GUI is loaded,
// up to HTTPD_BEGIN_QUICK_MS, before returning. The
// sketch's setup() should not progress to opening the
// serial port / driving the link until httpd_start has
// succeeded at least once.

void test_begin_blocks_until_httpd_up() {
    std::cout << "\n=== begin() blocks until httpd is up (or times out) ==="
              << std::endl;
    std::string src = readFile(projectRoot() + "/src/al/web/AutoLinkWeb.cpp");
    assert(!src.empty());

    auto beginPos = src.find("bool AutoLinkWeb::begin(");
    auto setupPos = src.find("bool AutoLinkWeb::setupHttpAndLogging_(");
    assert(beginPos != std::string::npos);
    assert(setupPos != std::string::npos);

    std::string body = src.substr(beginPos, setupPos - beginPos);

    // The synchronous httpd-setup loop must exist in begin().
    assert(body.find("HTTPD_BEGIN_QUICK_MS") != std::string::npos);
    assert(body.find("setupHttpAndLogging_()") != std::string::npos);
    assert(body.find("isUp()") != std::string::npos);

    // The loop must guard on a millis()-based deadline.
    assert(body.find("millis() - httpStartMs < HTTPD_BEGIN_QUICK_MS") !=
           std::string::npos);

    // And the while-condition must include `!isUp()` so
    // the loop actually waits for the httpd to come up,
    // not just sleeps the full 5 s and returns.
    auto whilePos = body.find("while (!isUp()");
    assert(whilePos != std::string::npos);
    auto deadlinePos =
        body.find("millis() - httpStartMs < HTTPD_BEGIN_QUICK_MS");
    assert(deadlinePos != std::string::npos);
    // Deadline must be part of the same while-condition
    // expression — find the closing `)` after `while`.
    auto whileClose = body.find('\n', whilePos);
    assert(whileClose != std::string::npos);
    assert(deadlinePos < whileClose);

    std::cout << "  PASS (begin() blocks on isUp() bounded by "
                 "HTTPD_BEGIN_QUICK_MS)\n";
}

// Pin: setupHttpAndLogging_() must wrap httpd_start in a
// retry loop. ESP_ERR_HTTPD_TASK (and ESP_ERR_NO_MEM) on
// first call is a known transient during boot when NTP,
// esp_timer and WiFi are all initializing — a single
// failed call should not tear down the web monitor.
//
// As of the latest release each attempt sleeps
// HTTPD_RETRY_PRE_MS before httpd_start to let the
// prior boot's TIME_WAIT socket clear. The retry
// budget was also widened (HTTPD_RETRY_MAX 3→8,
// HTTPD_RETRY_PRE_MS 750ms→5s, HTTPD_BEGIN_QUICK_MS
// 5s→12s) so the synchronous quick-start path in
// begin() can cover typical TIME_WAIT without
// bouncing out to the bg retry loop.

void test_fail_block_preserves_begin_lifetime_resources() {
    std::cout << "\n=== fail: block preserves logRing_/snapMtx_/logMtx_ ==="
              << std::endl;
    std::string src = readFile(projectRoot() + "/src/al/web/AutoLinkWeb.cpp");
    assert(!src.empty());

    auto failPos = src.find("fail:");
    assert(failPos != std::string::npos);

    // The fail block ends at the next "}\n\n}" or at the
    // matching closing brace of setupHttpAndLogging_().
    // We just need the body — find the next occurrence
    // of "return false;" after failPos.
    auto retPos = src.find("return false;", failPos);
    assert(retPos != std::string::npos);
    std::string failBody = src.substr(failPos, retPos - failPos);

    // These three must NOT appear in the fail block.
    assert(failBody.find("vSemaphoreDelete(snapMtx_)") == std::string::npos);
    assert(failBody.find("vSemaphoreDelete(logMtx_)") == std::string::npos);
    assert(failBody.find("free(logRing_)") == std::string::npos);

    // And statTimer_ + server_ cleanup MUST still happen.
    assert(failBody.find("statTimer_") != std::string::npos);
    assert(failBody.find("server_") != std::string::npos);

    std::cout << "  PASS (fail: cleans only statTimer_/server_, "
                 "leaves lifetime resources intact)\n";
}

// Pin: Link::kickoff() must exist as a public method
// and Link::begin() must skip the auto-kickoff when
// linkPaused_ is true. This is the protocol-side half
// of the "Ping silent until Start" gate.

int main() {
    std::cout << "=== Running WebBeginLifecycleTest ===" << std::endl;

    Log::log().setLevel(Log::DEBUG);
    test_begin_wires_log_sink_first();
    test_begin_logs_version_line_after_sink();
    test_begin_blocks_until_httpd_up();
    test_fail_block_preserves_begin_lifetime_resources();

    std::cout << "\n=== WebBeginLifecycleTest Completed ===" << std::endl;
    return 0;
}
