// Source-level regression test for AutoLinkWeb::handleRoot.
// AutoLinkWeb.cpp is `#ifdef ARDUINO` so it can't run on
// host; this pins the chunked-send contract by reading the
// source.
//
// The bug: handleRoot used to call
//   httpd_resp_send(req, DASHBOARD_HTML, sizeof(DASHBOARD_HTML) - 1);
// with a ~28 KB payload. ESP-IDF's httpd send buffer tops
// out around 4096 bytes, so the single-shot path silently
// truncated or stalled mid-frame and the browser saw a
// malformed / empty response. The fix is the chunked path
// (`httpd_resp_send_chunk` in ≤4096-byte slices, terminated
// with a null-length chunk) and a bumped httpd stack size
// (16384 vs 8192 / 6144) so the httpd task doesn't overflow
// while walking the loop. 12288 might also work but 16384
// gives headroom.
//
// Each assertion below is the structural pin. Toggling the
// fix off (re-introducing `httpd_resp_send(req, DASHBOARD_HTML, ...)`,
// dropping the null chunk terminator, or reverting
// cfg.stack_size to 6144 or 8192) flips at least one of them.
#ifndef ARDUINO

#    include <cassert>
#    include <fstream>
#    include <iostream>
#    include <sstream>
#    include <string>

namespace {

std::string readFile(const std::string &path) {
    std::ifstream f(path);
    if (!f.good())
        return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string projectRoot() {
    std::string base = ".";
    for (int i = 0; i < 8; i++) {
        std::ifstream pf(base + "/AGENTS.md");
        if (pf.good())
            return base;
        base += "/..";
    }
    return ".";
}

// Extracts the body of `AutoLinkWeb::handleRoot` from
// AutoLinkWeb.cpp so the assertions stay scoped to that
// function (a stray `httpd_resp_send` in another handler
// must not satisfy this gate). Walks back from the function
// header to find the opening brace, then forward to the
// matching close.
std::string extractHandleRootBody(const std::string &src) {
    auto headerPos = src.find("AutoLinkWeb::handleRoot(httpd_req_t *req)");
    if (headerPos == std::string::npos)
        return "";
    auto braceOpen = src.find('{', headerPos);
    if (braceOpen == std::string::npos)
        return "";
    int depth = 1;
    std::size_t i = braceOpen + 1;
    for (; i < src.size(); i++) {
        if (src[i] == '{')
            depth++;
        else if (src[i] == '}') {
            depth--;
            if (depth == 0)
                return src.substr(braceOpen, i + 1 - braceOpen);
        }
    }
    return "";
}

// Scoped to the httpd config block in setupHttpAndLogging_:
// walks back from `cfg.lru_purge_enable` to its enclosing
// brace-balanced block (the arm that initialises
// `httpd_config_t cfg = HTTPD_DEFAULT_CONFIG(); ...`).
// Same shape as UriHandlerAlignmentTest::extractEnclosingBracedBlock.
// Walk from a function signature (e.g. "void setup()")
// to its matching closing brace and return the body
// in between (signature line inclusive).
std::string extractFnBody(const std::string &src, const std::string &sig) {
    auto sigPos = src.find(sig);
    if (sigPos == std::string::npos)
        return "";
    auto openBrace = src.find('{', sigPos);
    if (openBrace == std::string::npos)
        return "";
    int depth = 0;
    for (std::size_t i = openBrace; i < src.size(); i++) {
        if (src[i] == '{')
            depth++;
        else if (src[i] == '}') {
            depth--;
            if (depth == 0)
                return src.substr(sigPos, i - sigPos + 1);
        }
    }
    return "";
}

std::string extractHttpConfigBlock(const std::string &src) {
    auto anchor = src.find("cfg.lru_purge_enable");
    if (anchor == std::string::npos)
        return "";
    int back = 0;
    std::size_t openPos = std::string::npos;
    for (std::size_t i = anchor; i > 0; i--) {
        if (src[i] == '}') {
            back++;
        } else if (src[i] == '{') {
            if (back == 0) {
                openPos = i;
                break;
            }
            back--;
        }
    }
    if (openPos == std::string::npos)
        return "";
    int depth = 1;
    std::size_t closePos = std::string::npos;
    for (std::size_t i = openPos + 1; i < src.size(); i++) {
        if (src[i] == '{')
            depth++;
        else if (src[i] == '}') {
            depth--;
            if (depth == 0) {
                closePos = i;
                break;
            }
        }
    }
    if (closePos == std::string::npos)
        return "";
    return src.substr(openPos, closePos + 1 - openPos);
}

void test_handle_root_uses_chunked_send() {
    std::cout
        << "\n=== handleRoot routes the dashboard through httpd_resp_send_chunk ==="
        << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/web/AutoLinkWeb.cpp");
    assert(!src.empty());

    std::string body = extractHandleRootBody(src);
    assert(!body.empty());

    // Pin 1: the buggy single-shot path is gone. The old
    // call site was
    //   httpd_resp_send(req, DASHBOARD_HTML, sizeof(DASHBOARD_HTML) - 1);
    // which fires once with the full payload and silently
    // truncates on a 4096-byte send buffer. Any reversion
    // to that exact shape (or to a new
    // `httpd_resp_send(req, DASHBOARD_HTML, ...)` form)
    // must trip this assertion.
    auto badCall = body.find("httpd_resp_send(req, DASHBOARD_HTML");
    if (badCall != std::string::npos) {
        std::cout << "  FAIL: handleRoot still contains "
                     "`httpd_resp_send(req, DASHBOARD_HTML, ...)` — "
                     "re-introduces the silent-truncation bug.\n";
    }
    assert(badCall == std::string::npos);

    // Pin 2: the chunked path is in place. Must call
    // `httpd_resp_send_chunk(req, p, ...)` (the data path)
    // and a terminating `httpd_resp_send_chunk(req,
    // nullptr, 0)` (closes the chunked-encoded body so the
    // browser flushes its parser).
    auto dataChunk = body.find("httpd_resp_send_chunk(req, p,");
    auto termChunk = body.find("httpd_resp_send_chunk(req, nullptr, 0)");
    if (dataChunk == std::string::npos) {
        std::cout << "  FAIL: handleRoot never calls "
                     "`httpd_resp_send_chunk(req, p, ...)` — chunked "
                     "loop missing.\n";
    }
    assert(dataChunk != std::string::npos);
    if (termChunk == std::string::npos) {
        std::cout << "  FAIL: handleRoot does not terminate with "
                     "`httpd_resp_send_chunk(req, nullptr, 0)` — "
                     "chunked body never closes.\n";
    }
    assert(termChunk != std::string::npos);

    // Pin 3: the chunk size is bounded. ESP-IDF's httpd
    // send buffer tops out around 4096 bytes; chunks above
    // that re-introduce the truncation. The contract is
    // `const size_t CHUNK = 4096;` somewhere in the
    // function. (We don't pin the loop variable's name or
    // the comparison style — only the cap.)
    auto chunkCap = body.find("CHUNK = 4096");
    if (chunkCap == std::string::npos) {
        std::cout << "  FAIL: handleRoot does not declare "
                     "`const size_t CHUNK = 4096;` — chunk size "
                     "unbounded / mismatched.\n";
    }
    assert(chunkCap != std::string::npos);

    std::cout << "  PASS (chunked loop, terminator, and 4096-byte cap "
                 "all present)\n";
}

void test_handle_root_sets_required_headers() {
    std::cout << "\n=== handleRoot still sets content-type + cache headers ==="
              << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/web/AutoLinkWeb.cpp");
    assert(!src.empty());

    std::string body = extractHandleRootBody(src);
    assert(!body.empty());

    // Pin: the response headers (text/html + no-store +
    // Connection: close) must remain. The chunked rewrite
    // touches the body path only — silently dropping a
    // header would change how the browser caches the page
    // or how it negotiates connection lifecycle.
    assert(body.find("text/html; charset=utf-8") != std::string::npos);
    assert(body.find("Cache-Control") != std::string::npos);
    assert(body.find("no-store") != std::string::npos);
    assert(body.find("Connection") != std::string::npos);

    std::cout << "  PASS (text/html + Cache-Control: no-store + "
                 "Connection: close all present)\n";
}

void test_httpd_stack_size_is_at_least_16384() {
    std::cout << "\n=== httpd task stack size bumped for chunked 28 KB send ==="
              << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/web/AutoLinkWeb.cpp");
    assert(!src.empty());

    // Scope to the httpd config block (same shape as
    // UriHandlerAlignmentTest) so a stray `stack_size = 16384`
    // mention in a comment elsewhere doesn't satisfy this
    // pin.
    std::string block = extractHttpConfigBlock(src);
    assert(!block.empty());

    auto pos = block.find("cfg.stack_size");
    assert(pos != std::string::npos);
    auto eq = block.find('=', pos);
    assert(eq != std::string::npos);
    auto semi = block.find(';', eq);
    assert(semi != std::string::npos);
    std::string value = block.substr(eq + 1, semi - eq - 1);
    auto trim = [](std::string s) {
        size_t a = s.find_first_not_of(" \t");
        size_t b = s.find_last_not_of(" \t");
        if (a == std::string::npos)
            return std::string();
        return s.substr(a, b - a + 1);
    };
    value = trim(value);

    int stackSize = std::atoi(value.c_str());
    // 6144 / 8192 (the old values) are enough for the small-
    // response path but the httpd worker stack still
    // exhausts under sustained chunked sends on a 28 KB
    // payload — the chunked loop's local state plus the
    // httpd internal send buffer frame can push the task
    // over its stack at runtime even though the build is
    // clean. 12288 might also work but 16384 gives
    // headroom; pin the lower bound at exactly 16384 so a
    // future regression to 6144 or 8192 trips here.
    if (stackSize < 16384) {
        std::cout << "  FAIL: cfg.stack_size = " << stackSize
                  << " (need >= 16384 for chunked 28 KB send).\n";
    }
    assert(stackSize >= 16384);
    assert(value == "16384");

    std::cout << "  PASS (cfg.stack_size = " << value
              << " in httpd config block)\n";
}

// Pin: setSink() must be called from begin(), not from
// setupHttpAndLogging_(). The web GUI must start capturing
// log messages before any meaningful work happens, so the
// app version line is the first entry in the live log.
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
void test_link_begin_defers_kickoff_when_paused() {
    std::cout << "\n=== Link::begin() defers kickoff when linkPaused_ ==="
              << std::endl;
    std::string hSrc = readFile(projectRoot() + "/src/al/link/Link.h");
    std::string cSrc = readFile(projectRoot() + "/src/al/link/Link.cpp");
    assert(!hSrc.empty());
    assert(!cSrc.empty());

    // Public declaration exists.
    assert(hSrc.find("void kickoff();") != std::string::npos);

    // kickoff() implementation exists.
    assert(cSrc.find("void Link::kickoff()") != std::string::npos);

    // kickoff() must be idempotent (kickedOff_ guard).
    auto kickoffBody = extractFnBody(cSrc, "void Link::kickoff()");
    assert(!kickoffBody.empty());
    assert(kickoffBody.find("kickedOff_") != std::string::npos);

    // Link::begin() must check linkPaused_ and skip the
    // kickoff when paused.
    auto beginBody = extractFnBody(cSrc, "void Link::begin()");
    assert(!beginBody.empty());
    assert(beginBody.find("linkPaused_") != std::string::npos);

    // The kickedOff_ flag is set false at the start of begin().
    assert(beginBody.find("kickedOff_ = false") != std::string::npos);

    // The branch shape must be "if (linkPaused_) return;
    // kickoff();" — i.e. when paused, begin() does NOT
    // call kickoff. Find an early-return guarded by
    // linkPaused_ that precedes the kickoff() call.
    auto pausedCheck = beginBody.find("linkPaused_)");
    auto kickoffCall = beginBody.find("\n    kickoff();");
    assert(pausedCheck != std::string::npos);
    assert(kickoffCall != std::string::npos);
    assert(pausedCheck < kickoffCall);
    // The early-return must be inside the same function
    // (between pausedCheck and kickoffCall).
    auto earlyReturn = beginBody.find("return;", pausedCheck);
    assert(earlyReturn != std::string::npos);
    assert(earlyReturn < kickoffCall);

    std::cout << "  PASS (Link::kickoff is public + idempotent; "
                 "begin() defers when linkPaused_)\n";
}

// Pin: Ping::setup() falls through to auto-kickoff when
// the web monitor never came up. The user explicitly
// asked for this: if the GUI can't start, don't leave
// the device silent — drive the wire.
void test_ping_falls_through_when_gui_down() {
    std::cout << "\n=== Ping::setup falls through to kickoff when "
                 "GUI is down ==="
              << std::endl;
    std::string src = readFile(projectRoot() + "/src/al/pingpong/Ping.h");
    assert(!src.empty());

    auto setupBody = extractFnBody(src, "void setup()");
    assert(!setupBody.empty());

    // The fall-through branch must check the web monitor's
    // isUp() predicate.
    assert(setupBody.find("isUp()") != std::string::npos);
    // And it must call kickoff() on the comm to drive
    // the wire even when the GUI is not up.
    assert(setupBody.find("comm_.kickoff()") != std::string::npos);
    // And it must flip paused_ off so loop() actually
    // sends (not just sits in paused mode).
    assert(setupBody.find("paused_ = false") != std::string::npos);

    std::cout << "  PASS (Ping falls through to kickoff when mon.isUp() "
                 "is false)\n";
}

} // namespace

int main() {
    std::cout << "=== Running AutoLinkWeb HandleRoot Chunked-Send Tests ==="
              << std::endl;
    test_handle_root_uses_chunked_send();
    test_handle_root_sets_required_headers();
    test_httpd_stack_size_is_at_least_16384();
    test_begin_wires_log_sink_first();
    test_begin_logs_version_line_after_sink();
    test_begin_blocks_until_httpd_up();
    test_setup_httpd_retries_on_failure();
    test_setup_httpd_pre_delay();
    test_fail_block_preserves_begin_lifetime_resources();
    test_link_begin_defers_kickoff_when_paused();
    test_ping_falls_through_when_gui_down();
    test_wifi_task_retries_forever_when_creds_given();
    std::cout << "\n=== HandleRoot Chunked-Send Tests Completed ==="
              << std::endl;
    return 0;
}

#endif