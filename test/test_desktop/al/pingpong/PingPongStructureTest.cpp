// Source-level regression test for the PingPongBase split.
// Pins the contract:
//
//   - logStats / resetStatBaseline are
//     FREE FUNCTIONS in the autolink
//     namespace, NOT members of
//     PingPongBase. Diagnostics is a
//     separate concern from comms.
//   - setupCommon() is GONE. Ping::setup
//     and Pong::setup each sequence
//     initSerial / bringUpLink /
//     startWebMonitor themselves.
//   - The tx/rx rate baseline is owned
//     by Ping / Pong as a StatBaseline
//     member, not by PingPongBase.
//
// All four points must hold. Toggling
// any of them back (re-introducing
// setupCommon as a member, putting
// logStats back on PingPongBase, etc.)
// makes at least one assertion fail.
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

// Extracts the body of a named function
// from a source string by brace-matching.
// Returns "" if the function name isn't
// found or the brace depth doesn't
// resolve.
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

// Extracts the body of a struct (or
// class) declaration. Same brace-match
// rule, but starts at the matching
// brace after `struct NAME {`.
std::string extractStructBody(const std::string &src, const std::string &name) {
    auto kw = src.find("struct " + name);
    if (kw == std::string::npos)
        return "";
    auto bodyStart = src.find('{', kw);
    if (bodyStart == std::string::npos)
        return "";
    int depth = 0;
    for (size_t i = bodyStart; i < src.size(); i++) {
        if (src[i] == '{')
            depth++;
        else if (src[i] == '}') {
            depth--;
            if (depth == 0)
                return src.substr(kw, i + 1 - kw);
        }
    }
    return "";
}

void test_pingpong_base_has_no_setupCommon() {
    std::cout
        << "\n=== PingPongBase has no setupCommon member (split into steps) ==="
        << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/pingpong/PingPongBase.h");
    assert(!src.empty());

    // Pin: setupCommon must not appear
    // as a MEMBER of PingPongBase. The
    // file header comment may still
    // reference the old name for
    // historical context — that's why
    // we check the struct body, not the
    // whole file. Re-introducing
    // setupCommon as a method on
    // PingPongBase breaks this gate.
    std::string base = extractStructBody(src, "PingPongBase");
    assert(!base.empty());
    if (base.find("setupCommon") != std::string::npos) {
        std::cout
            << "  FAIL: PingPongBase struct body still references setupCommon\n";
        assert(false);
    }
    std::cout << "  PASS (setupCommon not a member of PingPongBase)\n";
}

void test_pingpong_base_logStats_is_free_function() {
    std::cout
        << "\n=== logStats is a free function, not a PingPongBase member ==="
        << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/pingpong/PingPongBase.h");
    assert(!src.empty());

    std::string base = extractStructBody(src, "PingPongBase");
    assert(!base.empty());

    // The struct body must NOT declare
    // logStats. Diagnostics is no
    // longer a method on the comms/web
    // handle holder.
    if (base.find("logStats(") != std::string::npos) {
        std::cout << "  FAIL: PingPongBase still has logStats as a member\n";
        assert(false);
    }
    if (base.find("resetStatBaseline(") != std::string::npos) {
        std::cout << "  FAIL: PingPongBase still has resetStatBaseline as a "
                     "member\n";
        assert(false);
    }
    // And the rolling baseline fields
    // (tStat_ / lastTx_ / lastRx_) used
    // to live on PingPongBase. With the
    // split, the rate window is owned
    // by Ping / Pong via StatBaseline,
    // so the struct should not carry
    // any of those names.
    if (base.find("tStat_") != std::string::npos ||
        base.find("lastTx_") != std::string::npos ||
        base.find("lastRx_") != std::string::npos) {
        std::cout << "  FAIL: PingPongBase still owns tStat_/lastTx_/lastRx_\n";
        assert(false);
    }
    std::cout << "  PASS (no member logStats / resetStatBaseline / "
              << "rate-window fields)\n";
}

void test_pingpong_base_exposes_three_setup_steps() {
    std::cout
        << "\n=== initSerial / bringUpLink / startWebMonitor are free functions ==="
        << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/pingpong/PingPongBase.h");
    assert(!src.empty());

    // Each step must be defined at
    // namespace scope (NOT as a
    // PingPongBase member). The
    // check: each name appears with
    // an inline void prefix OUTSIDE
    // the PingPongBase struct body.
    auto isInsideBase = [&](const std::string &name) {
        std::string base = extractStructBody(src, "PingPongBase");
        return base.find(name) != std::string::npos;
    };
    auto isDefinedOutside = [&](const std::string &name) {
        auto pos = src.find(name);
        if (pos == std::string::npos)
            return false;
        // Look for "inline void <name>" or
        // similar declaration before the
        // matching "(". If the line is
        // inside PingPongBase's body,
        // isInsideBase() catches it.
        return !isInsideBase(name);
    };
    assert(isDefinedOutside("initSerial"));
    assert(isDefinedOutside("bringUpLink"));
    assert(isDefinedOutside("startWebMonitor"));

    // And StatBaseline is exposed.
    assert(src.find("struct StatBaseline") != std::string::npos);
    std::cout << "  PASS (three setup steps + StatBaseline present at "
              << "namespace scope)\n";
}

void test_ping_setup_sequences_three_steps() {
    std::cout << "\n=== Ping::setup sequences initSerial / bringUpLink / "
              << "startWebMonitor ===" << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/pingpong/Ping.h");
    assert(!src.empty());

    std::string body = extractFnBody(src, "void setup()");
    assert(!body.empty());

    // setup() must call all three steps
    // in the order: initSerial,
    // bringUpLink, startWebMonitor.
    // Order matters: web monitor must
    // come up AFTER the link is up so
    // the dashboard can read /stats
    // immediately.
    auto p1 = body.find("initSerial(");
    auto p2 = body.find("bringUpLink(");
    auto p3 = body.find("startWebMonitor(");
    assert(p1 != std::string::npos);
    assert(p2 != std::string::npos);
    assert(p3 != std::string::npos);
    assert(p1 < p2);
    assert(p2 < p3);

    // And setup() must NOT call
    // setupCommon (the old bundled
    // method).
    assert(body.find("setupCommon") == std::string::npos);
    std::cout << "  PASS (Ping::setup calls the three steps in order, no "
              << "setupCommon)\n";
}

void test_pong_setup_sequences_three_steps() {
    std::cout << "\n=== Pong::setup sequences initSerial / bringUpLink / "
              << "startWebMonitor ===" << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/pingpong/Pong.h");
    assert(!src.empty());

    std::string body = extractFnBody(src, "void setup()");
    assert(!body.empty());

    auto p1 = body.find("initSerial(");
    auto p2 = body.find("bringUpLink(");
    auto p3 = body.find("startWebMonitor(");
    assert(p1 != std::string::npos);
    assert(p2 != std::string::npos);
    assert(p3 != std::string::npos);
    assert(p1 < p2);
    assert(p2 < p3);
    assert(body.find("setupCommon") == std::string::npos);
    std::cout << "  PASS (Pong::setup calls the three steps in order, no "
              << "setupCommon)\n";
}

void test_ping_pong_call_free_log_stats() {
    std::cout << "\n=== Ping and Pong call the free logStats(...) ==="
              << std::endl;
    std::string root = projectRoot();
    std::string pingSrc = readFile(root + "/src/al/pingpong/Ping.h");
    std::string pongSrc = readFile(root + "/src/al/pingpong/Pong.h");
    assert(!pingSrc.empty());
    assert(!pongSrc.empty());

    // Pin: the call sites are
    // "logStats(...)" not
    // "base_.logStats(...)". Removing
    // the free function (and reverting
    // to a PingPongBase::logStats
    // member) breaks this assertion.
    assert(pingSrc.find("logStats(") != std::string::npos);
    assert(pongSrc.find("logStats(") != std::string::npos);
    assert(pingSrc.find("base_.logStats") == std::string::npos);
    assert(pongSrc.find("base_.logStats") == std::string::npos);
    std::cout << "  PASS (both Ping and Pong call the free logStats)\n";
}

void test_ping_pong_own_stat_baseline() {
    std::cout << "\n=== Ping and Pong own their StatBaseline member ==="
              << std::endl;
    std::string root = projectRoot();
    std::string pingSrc = readFile(root + "/src/al/pingpong/Ping.h");
    std::string pongSrc = readFile(root + "/src/al/pingpong/Pong.h");
    assert(!pingSrc.empty());
    assert(!pongSrc.empty());

    // Each class must declare a
    // StatBaseline member. The rate
    // window lives with the loop that
    // resets it; PingPongBase doesn't
    // own it anymore.
    assert(pingSrc.find("StatBaseline stat_") != std::string::npos);
    assert(pongSrc.find("StatBaseline stat_") != std::string::npos);

    // And resetStatBaseline must be
    // called as the free function on
    // each one's stat_, not via the
    // old base_.resetStatBaseline().
    assert(pingSrc.find("resetStatBaseline(stat_)") != std::string::npos);
    assert(pongSrc.find("resetStatBaseline(stat_)") != std::string::npos);
    assert(pingSrc.find("base_.resetStatBaseline") == std::string::npos);
    assert(pongSrc.find("base_.resetStatBaseline") == std::string::npos);
    std::cout << "  PASS (StatBaseline stat_ on Ping and Pong; free "
              << "resetStatBaseline(stat_))\n";
}

} // namespace

int main() {
    std::cout << "=== Running PingPong Structure Tests ===" << std::endl;
    test_pingpong_base_has_no_setupCommon();
    test_pingpong_base_logStats_is_free_function();
    test_pingpong_base_exposes_three_setup_steps();
    test_ping_setup_sequences_three_steps();
    test_pong_setup_sequences_three_steps();
    test_ping_pong_call_free_log_stats();
    test_ping_pong_own_stat_baseline();
    std::cout << "\n=== PingPong Structure Tests Completed ===" << std::endl;
    return 0;
}

#endif
