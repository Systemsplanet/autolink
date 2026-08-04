// Production log level regression pin: the default
// `initSerial` in PingPongBase.h must set the runtime log
// level to INFO, not DEBUG.
//
// The prior default (DEBUG) emitted per-echo lines at the
// ASYNC pipeline rate (MAX_TX_PER_LOOP per loop), and
// although the per-echo lines themselves were demoted to
// verbose in a prior pass, the configured level was still
// set to DEBUG — so any `Log::log().debug(...)` call inside
// the wire path (and a future regression that re-raises a
// per-echo line back to debug) would flood the log sink and
// starve it of the very state-transition lines needed to
// diagnose a wedge.
//
// Pinned by:
//   Pin 1: source-grep on initSerial's body — must call
//          `setLevel(Log::INFO)`, not `Log::DEBUG`.
//   Pin 2: source-grep on the esp_log_level_set default —
//          must be ESP_LOG_INFO, not ESP_LOG_VERBOSE /
//          ESP_LOG_DEBUG.
#include <cassert>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

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

std::string extractFnBody(const std::string &src, const std::string &name) {
    auto start = src.find(name);
    if (start == std::string::npos)
        return "";
    auto bodyStart = src.find('{', start);
    if (bodyStart == std::string::npos)
        return "";
    int depth = 0;
    for (size_t i = bodyStart; i < src.size(); i++) {
        if (src[i] == '{')
            depth++;
        else if (src[i] == '}') {
            depth--;
            if (depth == 0)
                return src.substr(start, i + 1 - start);
        }
    }
    return "";
}
} // namespace

int main() {
    std::cout << "=== Production Log Level Tests ===" << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/pingpong/PingPongBase.h");
    assert(!src.empty() && "PingPongBase.h must be readable");

    std::string body = extractFnBody(src, "initSerial");
    assert(!body.empty() && "initSerial function must exist in PingPongBase.h");

    // Pin 1: setLevel(Log::INFO) must be in initSerial's body.
    // The prior default of Log::DEBUG flooded the log with
    // per-echo lines (before they were demoted to verbose)
    // and would re-flood again if a future regression
    // re-raised any per-echo line to debug.
    std::cout << "\n=== Pin 1: initSerial defaults to Log::INFO ==="
              << std::endl;
    assert(body.find("setLevel(Log::INFO)") != std::string::npos &&
           "initSerial must call setLevel(Log::INFO) as the production "
           "default — DEBUG floods the log on ASYNC pipeline runs and "
           "starves it of state-transition lines");
    assert(body.find("setLevel(Log::DEBUG)") == std::string::npos &&
           "initSerial must NOT call setLevel(Log::DEBUG) as a default "
           "— DEBUG is for deep-tracing only, not the production boot");
    std::cout << "  PASS (setLevel(Log::INFO) in initSerial, no DEBUG default)"
              << std::endl;

    // Pin 2: the esp_log_level_set call must use ESP_LOG_INFO.
    // The ESP-IDF log level controls what the on-device
    // ESP_LOG* macros emit; ESP_LOG_DEBUG would let every
    // per-echo `Log::log().debug(...)` through on the
    // device-side sink (the same flood the runtime level
    // avoids).
    std::cout << "\n=== Pin 2: esp_log_level_set default is ESP_LOG_INFO ==="
              << std::endl;
    assert(body.find("esp_log_level_set(\"*\", ESP_LOG_INFO)") !=
               std::string::npos &&
           "initSerial must set the ESP-IDF log level to ESP_LOG_INFO "
           "for the same reason Pin 1 sets the runtime level to "
           "Log::INFO — DEBUG lets every per-echo log line through on "
           "the device-side sink");
    assert(body.find("esp_log_level_set(\"*\", ESP_LOG_DEBUG)") ==
               std::string::npos &&
           "initSerial must NOT default the ESP-IDF log level to "
           "ESP_LOG_DEBUG");
    assert(body.find("esp_log_level_set(\"*\", ESP_LOG_VERBOSE)") ==
               std::string::npos &&
           "initSerial must NOT default the ESP-IDF log level to "
           "ESP_LOG_VERBOSE — verbose is the deep-trace level, not "
           "the production default");
    std::cout << "  PASS (esp_log_level_set default is ESP_LOG_INFO)"
              << std::endl;

    std::cout << "\n=== All 2 production log level pins PASS ===" << std::endl;
    return 0;
}
