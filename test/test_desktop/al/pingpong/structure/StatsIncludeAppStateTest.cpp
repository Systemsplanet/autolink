// Source-level regression test for the current shape
// unconditional-stats-with-app-state contract.
// The buggy-original logStats() line was a per-tick
// emission with no app state in it. Worse: it
// was called from Ping::loop only at the very
// end, AFTER the gap-stop early-return path, so
// a wedge (gapSeq_ set, sends paused) produced
// no `[A]` stats line in the field log at all.
// The operator staring at the log saw the gap
// stop fire, then 20 seconds of silence, then a
// second gap stop for the same seq, then more
// silence — no signal that the app was alive
// and stuck.
//
// The fix:
//   1. logStats() accepts an AppStateLog
//      parameter (default = no state).
//   2. logStats() line always includes the
//      app state: gapStopped, gapMissingSeq,
//      paused, lastSendMsgReason.
//   3. Ping::loop calls logStats() even from
//      the gap-stop early-return path with
//      `app.gapStopped = true` and
//      `app.gapMissingSeq = gapSeq_` so the
//      operator sees the wedge state in the
//      next 5-second tick.
//   4. The main-path logStats() call also
//      passes the app state with
//      gapStopped=false (sends are flowing).
//
// This test pins:
//   a. logStats has an AppStateLog
//      parameter (and a default value).
//   b. The log line includes the four
//      app-state fields.
//   c. Ping::loop's gap-stop block calls
//      logStats with `app.gapStopped = true`.
//   d. Ping::loop's main-path logStats call
//      builds an AppStateLog with
//      `app.gapStopped = false`.

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
        return std::string();
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
    return std::string();
}

std::string extractFnBody(const std::string &src,
                          const std::string &signature) {
    auto start = src.find(signature);
    if (start == std::string::npos)
        return std::string();
    auto bodyStart = src.find('{', start);
    if (bodyStart == std::string::npos)
        return std::string();
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
    return std::string();
}

// Pin a: logStats has an AppStateLog parameter
// (or accepts the default no-arg call shape
// used by Pong). The signature must include
// `const AppStateLog &` somewhere.
void test_logStats_has_app_state_param() {
    std::cout << "\n=== Pin a: logStats accepts an AppStateLog parameter ==="
              << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/pingpong/PingPongBase.h");
    assert(!src.empty());
    // Find the function definition.
    std::string body = extractFnBody(
        src, "inline void logStats(Log &log, const char *tag, AutoLink &comm,");
    assert(!body.empty());
    // Must include AppStateLog parameter.
    assert(body.find("AppStateLog") != std::string::npos);
    // And the default value (so existing call
    // sites compile).
    assert(body.find("= AppStateLog()") != std::string::npos);
    std::cout
        << "  logStats signature includes AppStateLog with default value \u2713"
        << std::endl;
    std::cout << "  PASS" << std::endl;
}

// Pin b: the log line includes the four
// app-state fields.
void test_logStats_line_includes_app_state() {
    std::cout
        << "\n=== Pin b: logStats line includes the four app-state fields ==="
        << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/pingpong/PingPongBase.h");
    assert(!src.empty());
    std::string body = extractFnBody(
        src, "inline void logStats(Log &log, const char *tag, AutoLink &comm,");
    assert(!body.empty());
    // The log.info call's format string.
    // Locate the `log.info(` and walk to the
    // closing `)` (the format string may span
    // multiple lines, so `");\n"` is a more
    // reliable terminator than a same-line
    // close).
    auto p = body.find("log.info(");
    assert(p != std::string::npos);
    auto fmtEnd = body.find(");", p);
    assert(fmtEnd != std::string::npos);
    std::string fmt = body.substr(p, fmtEnd - p);
    // Each app-state field must appear in the
    // format string. We don't require a strict
    // count because format-specifier layout
    // may change.
    assert(fmt.find("gapStopped=") != std::string::npos);
    assert(fmt.find("gapMissing=") != std::string::npos);
    assert(fmt.find("paused=") != std::string::npos);
    assert(fmt.find("lastSend=") != std::string::npos);
    std::cout << "  format string includes gapStopped, gapMissing, paused, "
                 "lastSend \u2713"
              << std::endl;
    std::cout << "  PASS" << std::endl;
}

// Pin c: Ping::loop's gap-stop block calls
// logStats with `app.gapStopped = true`.
void test_ping_gapstop_calls_logStats() {
    std::cout
        << "\n=== Pin c: gap-stop block calls logStats with gapStopped=true ==="
        << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/pingpong/Ping.h");
    assert(!src.empty());
    // Find the gap-stop early-return block.
    // It's the `if (gapSeq_ != NO_GAP) { ... return; }`
    // block inside the gap-decision scope.
    auto gapIf = src.find("if (gapSeq_ != NO_GAP)");
    assert(gapIf != std::string::npos);
    auto gapEnd = src.find("return;", gapIf);
    assert(gapEnd != std::string::npos);
    std::string block = src.substr(gapIf, gapEnd - gapIf);
    assert(block.find("logStats(") != std::string::npos);
    assert(block.find("app.gapStopped = true") != std::string::npos);
    assert(block.find("app.gapMissingSeq = gapSeq_") != std::string::npos);
    std::cout << "  gap-stop block builds AppStateLog + calls logStats \u2713"
              << std::endl;
    std::cout << "  PASS (wedge self-identifies in next stats line)"
              << std::endl;
}

// Pin d: Ping::loop's main-path logStats call
// builds an AppStateLog with gapStopped=false.
void test_ping_mainpath_calls_logStats_with_state() {
    std::cout
        << "\n=== Pin d: main-path logStats call passes gapStopped=false ==="
        << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/pingpong/Ping.h");
    assert(!src.empty());
    // The main-path logStats call is the LAST
    // logStats call in Ping::loop. Locate all
    // logStats( call sites and find the one
    // after the gap-stop early-return.
    auto gapStopEnd = src.find("if (gapSeq_ != NO_GAP)");
    assert(gapStopEnd != std::string::npos);
    auto gapStopRet = src.find("return;", gapStopEnd);
    assert(gapStopRet != std::string::npos);
    // The main-path call is somewhere after
    // gapStopRet.
    auto mainLog = src.find("logStats(", gapStopRet);
    assert(mainLog != std::string::npos);
    // Walk back to find the AppStateLog
    // construction.
    auto appDef = src.rfind("AppStateLog", mainLog);
    assert(appDef != std::string::npos);
    auto appDefEnd = appDef + 1200; // generous
    if (appDefEnd > src.size())
        appDefEnd = src.size();
    std::string appBlock = src.substr(appDef, appDefEnd - appDef);
    assert(appBlock.find("gapStopped = false") != std::string::npos);
    assert(appBlock.find("lastSendMsgReason = ") != std::string::npos);
    std::cout
        << "  main-path AppStateLog has gapStopped=false + lastSendMsgReason \u2713"
        << std::endl;
    std::cout << "  PASS" << std::endl;
}

// Pin e: Pong::loop also calls logStats with
// the AppStateLog (gapStopped=false for the
// ack-only Pong).
void test_pong_logStats_with_state() {
    std::cout
        << "\n=== Pin e: Pong::loop also passes AppStateLog to logStats ==="
        << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/pingpong/Pong.h");
    assert(!src.empty());
    std::string body = extractFnBody(src, "void loop()");
    assert(!body.empty());
    // The logStats call site.
    auto p = body.find("logStats(");
    assert(p != std::string::npos);
    // Walk back to find the AppStateLog
    // construction.
    auto appDef = body.rfind("AppStateLog", p);
    assert(appDef != std::string::npos);
    assert(p - appDef < 200);
    auto appEnd = appDef + 200;
    if (appEnd > body.size())
        appEnd = body.size();
    std::string appBlock = body.substr(appDef, appEnd - appDef);
    assert(appBlock.find("AppStateLog app;") != std::string::npos);
    assert(appBlock.find("lastSendMsgReason = ") != std::string::npos);
    std::cout << "  Pong::loop builds AppStateLog + passes to logStats \u2713"
              << std::endl;
    std::cout << "  PASS" << std::endl;
}

} // namespace

int main() {
    std::cout << "=== Stats include app-state regression tests ==="
              << std::endl;
    test_logStats_has_app_state_param();
    test_logStats_line_includes_app_state();
    test_ping_gapstop_calls_logStats();
    test_ping_mainpath_calls_logStats_with_state();
    test_pong_logStats_with_state();
    std::cout << "\n=== Stats include app-state tests PASS ===" << std::endl;
    return 0;
}

#endif
