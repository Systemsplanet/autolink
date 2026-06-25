// Source-level regression test for Ping::setPaused resume
// contract. Ping is #ifdef ARDUINO so we can't construct
// one on host, but we can pin the proactive clearQueue_()
// call that runs when paused_ flips from true to false.
//
// Contract:
//   - setPaused(false) calls clearQueue_() BEFORE any
//     echo matching can run, so a stale pre-pause echo
//     doesn't bump mismatchCount_.
//   - Without the clear, matchEcho_ falls through to its
//     reactive "CRC/length mismatch" branch and clears
//     there -- logging a spurious mismatch in the process.
//
// Toggling the clearQueue_() call out of setPaused makes
// the source-level grep below fail.
#ifndef ARDUINO

#    include <cassert>
#    include <fstream>
#    include <iostream>
#    include <sstream>
#    include <string>
#    include "NullArqCache.h"

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

// Returns the body of Ping::setPaused. The function is
// small enough (~25 lines) that a hand slice is more
// readable than regex.
std::string extractSetPausedBody(const std::string &pingSrc) {
    auto start = pingSrc.find("void setPaused(bool p)");
    if (start == std::string::npos)
        return "";
    auto bodyStart = pingSrc.find('{', start);
    if (bodyStart == std::string::npos)
        return "";
    int depth = 0;
    for (size_t i = bodyStart; i < pingSrc.size(); i++) {
        if (pingSrc[i] == '{')
            depth++;
        else if (pingSrc[i] == '}') {
            depth--;
            if (depth == 0)
                return pingSrc.substr(start, i + 1 - start);
        }
    }
    return "";
}

void test_setPaused_false_clears_pending_table() {
    std::cout << "\n=== setPaused(false) proactively clears pending table ==="
              << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/pingpong/Ping.h");
    assert(!src.empty());

    std::string body = extractSetPausedBody(src);
    assert(!body.empty());

    // The "if (!p)" branch must call clearQueue_() so the
    // pending/expectation table is empty before the next
    // matchEcho_() runs. Pin it.
    auto ifFalsePos = body.find("if (!p)");
    assert(ifFalsePos != std::string::npos);
    auto clearPos = body.find("clearQueue_()", ifFalsePos);
    assert(clearPos != std::string::npos);

    // The clear must come BEFORE the existing stat baseline
    // reset -- we want the queue dropped first, then the
    // counters zeroed for the new window. resetStatBaseline
    // is a free function; call form is resetStatBaseline(stat_).
    auto resetPos = body.find("resetStatBaseline(stat_)", ifFalsePos);
    assert(resetPos != std::string::npos);
    assert(clearPos < resetPos);

    std::cout << "  PASS (clearQueue_() runs before stat baseline reset)"
              << std::endl;
}

void test_setPaused_false_avoids_reactive_mismatch_path() {
    std::cout
        << "\n=== setPaused(false) does not rely on reactive mismatch recovery ==="
        << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/pingpong/Ping.h");
    assert(!src.empty());

    // matchEcho_() still has the reactive "mismatch -> clear"
    // branch as a safety net, but the resume path itself
    // shouldn't depend on it. The comment in setPaused must
    // reference the proactive clear.
    auto body = extractSetPausedBody(src);
    assert(body.find("Proactive resume") != std::string::npos);
    assert(body.find("reactive") != std::string::npos ||
           body.find("Reactive") != std::string::npos);

    std::cout << "  PASS (comment explicitly documents the proactive path)"
              << std::endl;
}

} // namespace

int main() {
    std::cout << "=== Running Ping Resume Source Tests ===" << std::endl;
    test_setPaused_false_clears_pending_table();
    test_setPaused_false_avoids_reactive_mismatch_path();
    std::cout << "\n=== Ping Resume Source Tests Completed ===" << std::endl;
    return 0;
}

#endif
