// Regression pin: AutoLink::begin() must call hal->begin()
// before link->begin().
//
// link->begin() can fire kickoff() synchronously (any Link
// that isn't constructed paused — e.g. the Pong/slave role,
// which never sets linkPaused_) and immediately drives the
// HAL: hw.startTimer() to arm the P1 listener, hw.setSpd() /
// hw.clearAppBuf() to prime the UART. EspHal::begin() is what
// installs the UART driver and xTimerCreate()s the sweep
// timer; EspHal::startTimer() silently no-ops when
// timer_handle is still null. Call link->begin() first and
// the slave's P1 listener timer never gets armed — it sits in
// Phase 1 forever, never logging another sweep tick, exactly
// the field symptom this pins.
//
// Source-grep only: hal->begin() is ARDUINO-gated and
// MockHal::begin() is a no-op, so the ordering bug has no
// runtime signature in the host suite. Toggle the fix off
// (swap the two calls back) and this pin goes red.
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
    return base;
}

std::string extractBeginBody(const std::string &src) {
    auto fnPos = src.find("void begin() {");
    assert(fnPos != std::string::npos);
    auto brace = src.find('{', fnPos);
    assert(brace != std::string::npos);
    int depth = 0;
    bool foundOpen = false;
    size_t endPos = std::string::npos;
    for (size_t i = brace; i < src.size(); i++) {
        if (src[i] == '{') {
            depth++;
            foundOpen = true;
        } else if (src[i] == '}') {
            depth--;
            if (foundOpen && depth == 0) {
                endPos = i;
                break;
            }
        }
    }
    assert(endPos != std::string::npos);
    return src.substr(fnPos, endPos - fnPos + 1);
}

void test_hal_begin_precedes_link_begin() {
    std::cout << "\n=== AutoLink::begin() calls hal->begin() before "
                 "link->begin() ==="
              << std::endl;
    std::string src = readFile(projectRoot() + "/include/AutoLink.h");
    assert(!src.empty());

    std::string body = extractBeginBody(src);
    assert(body.find("hal->begin();") != std::string::npos);
    assert(body.find("link->begin();") != std::string::npos);

    auto halPos = body.find("hal->begin();");
    auto linkPos = body.find("link->begin();");
    assert(halPos < linkPos &&
           "hal->begin() (installs the UART driver + creates the sweep "
           "timer) must run before link->begin() (which can kick off "
           "synchronously and drive the HAL); otherwise hw.startTimer() "
           "silently no-ops on a null timer_handle and the sweep never "
           "starts");
    std::cout << "  PASS (hal->begin() precedes link->begin())" << std::endl;
}

} // namespace

int main() {
    std::cout << "=== AutoLink Begin Order Regression ===" << std::endl;
    test_hal_begin_precedes_link_begin();
    std::cout << "\n=== Done ===" << std::endl;
    return 0;
}
#endif
