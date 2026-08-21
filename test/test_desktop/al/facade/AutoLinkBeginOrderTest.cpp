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
    // H2: AutoLink::begin() now
    // returns bool (was void).
    // Match either signature.
    auto fnPos = src.find("bool begin() {");
    if (fnPos == std::string::npos)
        fnPos = src.find("void begin() {");
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
    // J4: the config-aware
    // begin is the
    // canonical call.
    // Match either
    // hal->begin(cfg_)
    // (the J4 shape) or
    // hal->begin() (the
    // legacy no-arg
    // form) — both are
    // valid expressions
    // of the post-cap
    // gate. The
    // ordering invariant
    // (HAL before Link)
    // holds either way.
    // Skip C++ comments so
    // a comment that
    // contains the token
    // doesn't fake the
    // first-match position.
    auto stripComments = [](std::string s) {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size();) {
            if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '/') {
                while (i < s.size() && s[i] != '\n')
                    i++;
            } else {
                out += s[i++];
            }
        }
        return out;
    };
    std::string code = stripComments(body);
    // Link::begin() is the single owner of the HAL begin, so the
    // ordering constraint moved into it: hw.begin(cfg) must be the first
    // statement, before anything that drives the HAL. Otherwise
    // hw.startTimer() silently no-ops on a null timer_handle and the
    // sweep never starts.
    assert(code.find("hal->begin(") == std::string::npos &&
           "the facade must not begin the HAL — Link::begin() owns it");
    assert(code.find("link->begin()") != std::string::npos);
    std::string core = readFile(projectRoot() + "/src/al/link/LinkCore.cpp");
    assert(!core.empty());
    auto fn = core.find("bool Link::begin() {");
    assert(fn != std::string::npos);
    std::string fnBody = stripComments(core.substr(fn, 1200));
    auto hwBegin = fnBody.find("hw.begin(cfg)");
    assert(hwBegin != std::string::npos &&
           "Link::begin() must begin the HAL itself");
    auto firstOther = fnBody.find("hw.", fnBody.find("{"));
    assert(firstOther == hwBegin &&
           "hw.begin(cfg) must be the first HAL call in Link::begin()");
    std::cout << "  PASS (Link::begin() begins the HAL before driving it)"
              << std::endl;
}

} // namespace

int main() {
    std::cout << "=== AutoLink Begin Order Regression ===" << std::endl;
    test_hal_begin_precedes_link_begin();
    std::cout << "\n=== Done ===" << std::endl;
    return 0;
}
#endif
