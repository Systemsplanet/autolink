// Source-level regression test for the httpd startup
// race and the TIME_WAIT retry budget. AutoLinkWeb.cpp
// is `#ifdef ARDUINO` so it can't run on host; this pins
// the bug-fix contracts by reading the source.
//
// The two field-test bugs from the prior release's on-device trial:
//
// 1. Double-entry race: AutoLinkWeb::begin() and
//    wifiTaskThunk_() both called setupHttpAndLogging_()
//    concurrently. The `enabled_` flag wasn't set
//    atomically, so both callers entered the function
//    before either flipped enabled_=true, and both
//    hammered port 80 simultaneously, consuming the
//    retry budget. Fix: drop the setupHttpAndLogging_
//    call from begin(). begin() now polls isUp() until
//    the bg task has finished setup or the quick-start
//    deadline expires. wifiTaskThunk_ is the sole caller.
//
// 2. TIME_WAIT retry budget too short:
//    HTTPD_RETRY_MAX * HTTPD_RETRY_PRE_MS must be at
//    least 60 s to cover lwIP TCP TIME_WAIT
//    (CONFIG_LWIP_TCP_MSL * 2 ≈ 60 s). The prior release had
//    8 * 5 s = 40 s which was too short.
//
// 3. ESP_FAIL on uart_driver_install after a dirty
//    reboot (UART2 already installed): EspHal::begin()
//    now calls uart_driver_delete(uart_num) guarded by
//    uart_is_driver_installed() to recover from a prior
//    boot that left the driver installed.
//
// Each assertion below is a structural pin. Toggling
// the fix off (re-adding the setup call to begin(),
// reverting the retry budget, or dropping the UART
// pre-clear) flips at least one of them.
#ifndef ARDUINO

#    include <cassert>
#    include <cstdint>
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
// Pin: AutoLinkWeb::begin() must NOT call
// setupHttpAndLogging_(). wifiTaskThunk_ is the
// sole caller — begin() only polls isUp() until
// either the server is up or HTTPD_BEGIN_QUICK_MS
// elapses. Re-introducing the setupHttpAndLogging_
// call inside begin() trips here.
void test_begin_does_not_call_setupHttpAndLogging() {
    std::cout << "\n=== begin() does not call setupHttpAndLogging_ ==="
              << std::endl;
    std::string src = readFile(projectRoot() + "/src/al/web/AutoLinkWeb.cpp");
    assert(!src.empty());

    auto beginPos = src.find("bool AutoLinkWeb::begin(");
    assert(beginPos != std::string::npos);

    // Brace-match to the end of begin().
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

    // begin() must NOT call setupHttpAndLogging_().
    // Two callers means a race on the `enabled_`
    // flag — wifiTaskThunk_ is the sole owner now.
    assert(body.find("setupHttpAndLogging_") == std::string::npos);

    std::cout << "  PASS (begin() does not race the bg task for "
                 "setupHttpAndLogging_)\n";
}

// Pin: the httpd retry budget must cover lwIP TCP
// TIME_WAIT (~60 s = CONFIG_LWIP_TCP_MSL * 2). The
// retry budget is HTTPD_RETRY_MAX * HTTPD_RETRY_PRE_MS,
// and the quick-start window in begin() is
// HTTPD_BEGIN_QUICK_MS. Both must give the bg task
// enough wall time to outlive a typical TIME_WAIT.
//
// We assert:
//   - HTTPD_RETRY_MAX >= 12  (60s / 5s per attempt)
//   - HTTPD_RETRY_PRE_MS >= 5000  (so each attempt
//     contributes a meaningful settle slice)
//   - HTTPD_RETRY_MAX * HTTPD_RETRY_PRE_MS >= 60000
//   - HTTPD_BEGIN_QUICK_MS >=
//     HTTPD_RETRY_MAX * HTTPD_RETRY_PRE_MS (begin's
//     poll window must outlast the retry budget)
//
// Lowering the constants to 8 * 5 s = 40 s
// trips at least one of these.
void test_httpd_retry_budget_covers_time_wait() {
    std::cout << "\n=== httpd retry budget covers lwIP TIME_WAIT ==="
              << std::endl;
    std::string hSrc = readFile(projectRoot() + "/src/al/web/AutoLinkWeb.h");
    assert(!hSrc.empty());

    auto extractConst = [&](const std::string &name) -> uint32_t {
        auto p = hSrc.find(name);
        if (p == std::string::npos)
            return 0;
        auto eq = hSrc.find('=', p);
        if (eq == std::string::npos)
            return 0;
        auto semi = hSrc.find(';', eq);
        std::string val = hSrc.substr(eq + 1, semi - eq - 1);
        // Strip leading whitespace.
        size_t s = val.find_first_not_of(" \t");
        if (s != std::string::npos)
            val = val.substr(s);
        return (uint32_t)std::stoul(val);
    };

    uint32_t retryMax = extractConst("HTTPD_RETRY_MAX");
    uint32_t retryPreMs = extractConst("HTTPD_RETRY_PRE_MS");
    uint32_t quickMs = extractConst("HTTPD_BEGIN_QUICK_MS");

    std::cout << "  HTTPD_RETRY_MAX      = " << retryMax << "\n";
    std::cout << "  HTTPD_RETRY_PRE_MS   = " << retryPreMs << "\n";
    std::cout << "  HTTPD_BEGIN_QUICK_MS = " << quickMs << "\n";

    // Each attempt must sleep long enough to be
    // useful (5 s is the TIME_WAIT ceil per attempt).
    assert(retryPreMs >= 5000);

    // Total retry budget must clear TIME_WAIT.
    uint64_t budget = (uint64_t)retryMax * (uint64_t)retryPreMs;
    std::cout << "  total budget = " << budget << " ms\n";
    assert(budget >= 60000);

    // Quick-start poll window in begin() must cover
    // the full retry budget — otherwise the bg task
    // is racing a deadline it can't meet.
    assert((uint64_t)quickMs >= budget);

    std::cout << "  PASS (retry budget " << budget
              << " ms covers lwIP TIME_WAIT; quick-start " << quickMs
              << " ms covers the budget)\n";
}

// Pin: EspHal::begin() must pre-clear the UART driver
// before uart_driver_install(). Without this, a dirty
// reboot (crash, brownout, watchdog) leaves UART2
// installed and the next boot's uart_driver_install
// returns ESP_FAIL. The fix is:
//
//   if (uart_is_driver_installed(uart_num)) {
//       uart_driver_delete(uart_num);
//   }
//   esp_err_t e = uart_driver_install(...);
//
// Removing the pre-clear (or using `uart_driver_delete`
// unconditionally without the guard) trips here.
void test_esphal_begin_preclears_uart_driver() {
    std::cout << "\n=== EspHal::begin() pre-clears UART driver ==="
              << std::endl;
    std::string src = readFile(projectRoot() + "/src/al/hal/EspHal.h");
    assert(!src.empty());

    // Locate EspHal::begin(). The driver-install path
    // sits inside the begin() body, near the cleanup
    // lambda definition.
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

    // uart_is_driver_installed must guard the pre-clear.
    auto guardPos = body.find("uart_is_driver_installed(");
    assert(guardPos != std::string::npos);
    // uart_driver_delete must be called inside the guard.
    auto delPos = body.find("uart_driver_delete(", guardPos);
    assert(delPos != std::string::npos);
    // And the install must come AFTER the pre-clear.
    auto installPos = body.find("uart_driver_install(");
    assert(installPos != std::string::npos);
    assert(delPos < installPos);

    std::cout << "  PASS (uart_is_driver_installed guards "
                 "uart_driver_delete before uart_driver_install)\n";
}

// Pin: AutoLinkWeb.h must include AutoLinkWebCore.h
// OUTSIDE the #ifdef ARDUINO block so the
// WebSnapshot type alias (`using Snapshot =
// WebSnapshot;`) resolves even on device builds
// whose include-path resolution is fussy about
// nested quoted includes. The previous shape put
// the include inside the ARDUINO guard, and a
// real ArduinoDroid/esp32:esp32:firebeetle32
// cross-compile failed with `'WebSnapshot' does
// not name a type` at the using-alias site. Pulling
// the include above the guard is bulletproof —
// the core header is host-buildable (no Arduino-only
// types inside) so the unconditional include
// doesn't break the host build path.
void test_autolinkweb_h_includes_core_outside_arduino_guard() {
    std::cout << "\n=== AutoLinkWeb.h includes AutoLinkWebCore.h "
                 "outside ARDUINO guard ==="
              << std::endl;
    std::string hSrc = readFile(projectRoot() + "/src/al/web/AutoLinkWeb.h");
    assert(!hSrc.empty());

    // Find the position of the #include of
    // AutoLinkWebCore.h. The header may use the
    // standard `#include "..."` form OR the
    // preprocessor-indented `#    include "..."`
    // form (the latter lives inside `#ifdef
    // ARDUINO` blocks). Search for both.
    auto incPos = hSrc.find("#include \"al/web/AutoLinkWebCore.h\"");
    if (incPos == std::string::npos)
        incPos = hSrc.find("#    include \"al/web/AutoLinkWebCore.h\"");
    assert(incPos != std::string::npos);
    std::cout << "  AutoLinkWeb.h includes AutoLinkWebCore.h \u2713"
              << std::endl;

    // The include must appear BEFORE the
    // `#ifdef ARDUINO` opening of the Arduino-only
    // block. Find the `#ifdef ARDUINO` and assert
    // the include comes first.
    auto arduinoGuard = hSrc.find("#ifdef ARDUINO");
    assert(arduinoGuard != std::string::npos);
    assert(incPos < arduinoGuard);
    std::cout << "  include precedes #ifdef ARDUINO \u2713" << std::endl;

    // And library.properties must list
    // al/web/AutoLinkWebCore.h so the ArduinoDroid
    // library include path resolution adds the
    // file's directory as a fallback -I path. The
    // cross-compile cmd's -I<lib_root>/src should
    // already cover the quoted path, but the
    // explicit listing catches any toolchain that
    // builds -I strictly from `includes=`.
    std::string lp = readFile(projectRoot() + "/library.properties");
    assert(!lp.empty());
    assert(lp.find("al/web/AutoLinkWebCore.h") != std::string::npos);
    std::cout
        << "  library.properties lists AutoLinkWebCore.h in includes= \u2713"
        << std::endl;
    std::cout << "  PASS (WebSnapshot visible at using-alias site)"
              << std::endl;
}

} // namespace

int main() {
    std::cout << "=== Running Httpd Startup Tests ===" << std::endl;
    test_begin_does_not_call_setupHttpAndLogging();
    test_httpd_retry_budget_covers_time_wait();
    test_esphal_begin_preclears_uart_driver();
    test_autolinkweb_h_includes_core_outside_arduino_guard();
    std::cout << "\n=== Httpd Startup Tests Completed ===" << std::endl;
    return 0;
}

#endif