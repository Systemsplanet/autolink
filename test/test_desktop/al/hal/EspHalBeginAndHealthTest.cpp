// Source-level regression test for the begin() ESP_FAIL
// retry, the AutoLinkConfig tx buffer default, and the
// bringUpLink isHealthy() gate. Three fixes, three pins:
//
//   - EspHal::begin() must retry uart_driver_install once
//     after a 10 ms vTaskDelay on ESP_FAIL. Toggle off
//     (delete the vTaskDelay + retry call) -> this test
//     fails.
//   - AutoLinkConfig::txBufferSize default must be 256
//     (non-zero, sized for a couple of COBS frames at
//     default maxMsg). Toggle off (set back to 0) -> this
//     test fails.
//   - bringUpLink must call comm.isHealthy() after
//     comm.begin() and enter a halt loop on false. Toggle
//     off (delete the if (!comm.isHealthy()) block) -> this
//     test fails.
//
// EspHal.cpp / PingPongBase.h are `#ifdef ARDUINO` so they
// can't run on host; this gates the bug-fix contracts by
// reading the source, matching the pattern used by
// HttpdStartupTest.cpp.
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

void test_esphal_begin_retries_uart_driver_install_on_esp_fail() {
    std::cout << "\n=== EspHal::begin() retries uart_driver_install on "
                 "ESP_FAIL ==="
              << std::endl;
    std::string src = readFile(projectRoot() + "/src/al/hal/EspHal.h");
    assert(!src.empty());

    // Locate the body of begin().
    auto beginPos = src.find("void begin(");
    assert(beginPos != std::string::npos);

    size_t scan = beginPos;
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
    std::string body = src.substr(beginPos, endPos - beginPos + 1);

    // The retry contract: a single conditional retry that
    // triggers on ESP_FAIL, sleeps via vTaskDelay(10ms), and
    // re-invokes uart_driver_install. Pinning only the
    // presence of vTaskDelay inside begin() would also match
    // the existing 100 ms uart_event_task queue poll, so we
    // pin the retry-on-ESP_FAIL structure specifically.
    auto firstInstall = body.find("uart_driver_install(");
    assert(firstInstall != std::string::npos);
    // The retry condition must include `e == ESP_FAIL` — not
    // a literal false, not a missing condition. Pin the
    // exact phrasing so a 'if (false && e == ESP_FAIL)' style
    // dead-code bypass flips the test.
    auto condPos = body.find("e == ESP_FAIL", firstInstall);
    assert(condPos != std::string::npos);
    // Surrounding `if (` must sit just before the condition
    // (allow arbitrary whitespace).
    auto ifPos = body.rfind("if (", firstInstall);
    assert(ifPos != std::string::npos && ifPos < condPos);
    // No `false` / `0` / `false &&` guard between `if (` and
    // the condition.
    std::string condSlice = body.substr(ifPos, condPos - ifPos);
    assert(condSlice.find("false") == std::string::npos);
    // A 10 ms vTaskDelay must sit between the ESP_FAIL check
    // and the second uart_driver_install call.
    auto vTaskDelay = body.find("vTaskDelay", condPos);
    assert(vTaskDelay != std::string::npos);
    // pdMS_TO_TICKS(10) — the vTaskDelay call must wrap a
    // 10 ms duration. Check the call's argument list.
    auto vtOpen = body.find("(", vTaskDelay);
    auto vtClose = body.find(")", vtOpen);
    assert(vtOpen != std::string::npos && vtClose != std::string::npos);
    std::string vtArg = body.substr(vtOpen, vtClose - vtOpen);
    assert(vtArg.find("10") != std::string::npos);
    // The retry must invoke uart_driver_install again after
    // the delay. Look for a second occurrence past the delay.
    auto secondInstall = body.find("uart_driver_install(", vTaskDelay);
    assert(secondInstall != std::string::npos);

    // And the install-failure log path must still come after
    // the retry (otherwise the retry would be dead code).
    auto errLog = body.find("uart_driver_install: %s", firstInstall);
    assert(errLog != std::string::npos);
    assert(errLog > condPos);

    std::cout << "  PASS (retry on ESP_FAIL + 10ms vTaskDelay + "
                 "second uart_driver_install present)\n";
}

void test_autolink_config_default_tx_buffer_size_is_256() {
    std::cout << "\n=== AutoLinkConfig default txBufferSize == 256 ==="
              << std::endl;
    std::string src = readFile(projectRoot() + "/src/al/AutoLinkConfig.h");
    assert(!src.empty());

    // The field is a struct member — find it and read the
    // initializer. Tolerate any amount of whitespace between
    // the identifier and the literal.
    auto pos = src.find("txBufferSize");
    assert(pos != std::string::npos);

    auto eq = src.find('=', pos);
    assert(eq != std::string::npos);
    auto semi = src.find(';', eq);
    assert(semi != std::string::npos);
    std::string init = src.substr(eq + 1, semi - eq - 1);
    // Trim whitespace.
    size_t a = init.find_first_not_of(" \t\n\r");
    size_t b = init.find_last_not_of(" \t\n\r");
    if (a == std::string::npos)
        init = "";
    else
        init = init.substr(a, b - a + 1);
    assert(init == "256");

    // Sanity: the field must NOT be zero (the pre-fix default).
    assert(init != "0");

    std::cout << "  PASS (default txBufferSize = " << init << ")\n";
}

void test_bringUpLink_halts_on_isHealthy_false() {
    std::cout << "\n=== bringUpLink checks comm.isHealthy() after begin() "
                 "and halts on false ==="
              << std::endl;
    std::string src =
        readFile(projectRoot() + "/src/al/pingpong/PingPongBase.h");
    assert(!src.empty());

    // Find the bringUpLink free function body. It sits inside
    // the autolink namespace, gated on #ifdef ARDUINO.
    auto fnPos = src.find("inline void bringUpLink(");
    assert(fnPos != std::string::npos);

    size_t scan = fnPos;
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
    std::string body = src.substr(fnPos, endPos - fnPos + 1);

    // The isHealthy() call must happen AFTER comm.begin().
    auto beginCall = body.find("comm.begin()");
    assert(beginCall != std::string::npos);
    auto healthyCall = body.find("comm.isHealthy()", beginCall);
    assert(healthyCall != std::string::npos);

    // The branch must include a log call and a halt-style
    // infinite loop. Either `while (true)` with a delay or
    // an ESP-style `for (;;) delay(...)` would satisfy the
    // pin; we require the warning log + a while-true halt
    // because that's what the production shape uses.
    std::string branch = body.substr(healthyCall);
    assert(branch.find("log.error") != std::string::npos);
    assert(branch.find("while (true)") != std::string::npos);

    // And the "link layer up" log must be unreachable when
    // the branch fires — it must sit AFTER the isHealthy()
    // call, NOT before.
    auto linkUp = body.find("link layer up");
    assert(linkUp != std::string::npos);
    assert(linkUp > healthyCall);

    std::cout << "  PASS (isHealthy() check + log.error + halt come "
                 "after comm.begin(); link-up log is gated behind them)\n";
}

} // namespace

int main() {
    std::cout << "=== Running EspHal Begin + Health Gate Tests ==="
              << std::endl;
    test_esphal_begin_retries_uart_driver_install_on_esp_fail();
    test_autolink_config_default_tx_buffer_size_is_256();
    test_bringUpLink_halts_on_isHealthy_false();
    std::cout << "\n=== EspHal Begin + Health Gate Tests Completed ==="
              << std::endl;
    return 0;
}

#endif
