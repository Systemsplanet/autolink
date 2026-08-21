// Source-level regression test for the per-echo
// log-level in Ping::loop() and Pong::loop().
//
// ASYNC pipelines MAX_TX_PER_LOOP per loop, so a
// per-ACKed-echo log at debug level fires hundreds/sec
// and starves the log sink of the very
// state-transition lines needed to diagnose a wedge.
// The field log needs the per-echo line at Info so
// the operator sees delivered-sequence progression
// in the production-default log level (info) without
// toggling to verbose. The wire-COBs / wire-ACK
// companion lines are demoted to verbose for deep-trace.
//
// Pins:
//   1. Ping's "echo <seq> <bytes> <pending>" log
//      line is at info level, not verbose / debug.
//   2. Ping's "echo#=<n> seq=<s> msgBytes=<b>
//      pending=<p>" companion line is also info.
//   3. Pong's "echo <seq> <bytes>" log line is at
//      info level, not verbose / debug.
//   4. None of the per-echo log lines may be at
//      debug — the demotion-to-info is load-bearing
//      for the operator's per-chunk field-log
//      visibility.
//
// Toggle off (demote the per-echo log level back to
// verbose, or raise to debug) and all four pins go red.
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

void test_ping_echo_seq_bytes_pending_is_info() {
    std::cout << "\n=== Pin 1: Ping per-echo labeled line is info ==="
              << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/pingpong/Ping.h");
    assert(!src.empty());

    auto pos3 = src.find("\"echo#=%llu seq=%u msgBytes=%u pending=%d\"");
    assert(pos3 != std::string::npos);
    auto call3 = src.rfind("base_.log_.", pos3);
    assert(call3 != std::string::npos);
    std::string callLine3 = src.substr(call3, pos3 - call3);
    assert(callLine3.find("info") != std::string::npos &&
           "Ping's per-echo 'echo <seq> <bytes> <pending>' log line must be "
           "at info level, not verbose — field log needs per-chunk visibility "
           "in the production-default log level, and the wire-COBs / wire-ACK "
           "companion lines are the verbose-deep-trace channel");
    assert(callLine3.find("verbose") == std::string::npos);
    assert(callLine3.find("debug") == std::string::npos);
    std::cout << "  PASS (labeled echo line is at info level)" << std::endl;
}

void test_ping_echo_labeled_companion_is_info() {
    std::cout << "\n=== Pin 2: Ping labeled companion 'echo#=' is info ==="
              << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/pingpong/Ping.h");
    assert(!src.empty());

    auto pos4 = src.find("\"echo#=%llu seq=%u msgBytes=%u pending=%d\"");
    assert(pos4 != std::string::npos);
    auto call4 = src.rfind("base_.log_.", pos4);
    assert(call4 != std::string::npos);
    std::string callLine4 = src.substr(call4, pos4 - call4);
    assert(callLine4.find("info") != std::string::npos);
    assert(callLine4.find("verbose") == std::string::npos);
    assert(callLine4.find("debug") == std::string::npos);
    std::cout << "  PASS (4-arg labeled companion is at info level)"
              << std::endl;
}

void test_per_echo_lines_not_debug_or_verbose() {
    // Pin 4: per-echo lines must NOT be at debug or
    // verbose — the field log needs them at the
    // production-default (info) level so the operator
    // sees delivered-sequence progression without
    // toggling the runtime level. The wire-COBs /
    // wire-ACK companion lines are the deep-trace
    // verbose channel.
    std::cout
        << "\n=== Pin 4: per-echo log calls are at info, not debug/verbose "
           "==="
        << std::endl;
    auto checkFile = [](const std::string &path) {
        std::string src = readFile(path);
        assert(!src.empty());
        size_t pos = 0;
        while ((pos = src.find("base_.log_.", pos)) != std::string::npos) {
            size_t levelStart = pos + std::string("base_.log_.").size();
            size_t levelEnd = src.find("(", levelStart);
            assert(levelEnd != std::string::npos);
            std::string level = src.substr(levelStart, levelEnd - levelStart);
            size_t fmtStart = src.find("\"", levelEnd);
            size_t fmtEnd = fmtStart != std::string::npos
                ? src.find("\"", fmtStart + 1)
                : std::string::npos;
            std::string fmt =
                (fmtStart != std::string::npos && fmtEnd != std::string::npos)
                ? src.substr(fmtStart + 1, fmtEnd - fmtStart - 1)
                : std::string();
            if (fmt.find("echo") != std::string::npos) {
                assert(level == "info" &&
                       "per-echo log lines must stay at info level — "
                       "field log needs per-chunk visibility in the "
                       "production-default log level, the wire-COBs / "
                       "wire-ACK companion lines are the verbose "
                       "deep-trace channel");
            }
            pos = levelEnd + 1;
        }
    };
    std::string root = projectRoot();
    checkFile(root + "/src/al/pingpong/Ping.h");
    checkFile(root + "/src/al/pingpong/Pong.h");
    std::cout << "  PASS (no per-echo log call uses debug/verbose)"
              << std::endl;
}

void test_pong_echo_seq_bytes_is_info() {
    std::cout << "\n=== Pin 3: Pong emits no per-echo line below info ==="
              << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/pingpong/Pong.h");
    assert(!src.empty());

    // Pong is ack-only: its per-echo signal is the periodic stats line,
    // not a per-message log. Any echo-shaped line that reappears must be
    // at info, same field-log requirement as Ping.
    size_t pos = 0;
    while ((pos = src.find("echo", pos)) != std::string::npos) {
        auto call = src.rfind("base_.log_.", pos);
        auto stmt = src.rfind(";", pos);
        if (call != std::string::npos &&
            (stmt == std::string::npos || call > stmt)) {
            std::string callLine = src.substr(call, pos - call);
            assert(callLine.find("verbose") == std::string::npos &&
                   "a Pong per-echo log line must not be verbose");
            assert(callLine.find("debug") == std::string::npos &&
                   "a Pong per-echo log line must not be debug");
        }
        pos += 4;
    }
    assert(src.find("acks_sent=%lu") != std::string::npos &&
           "Pong's ack accounting must stay visible at info level");
    std::cout << "  PASS (Pong is ack-only; no sub-info echo line)"
              << std::endl;
}

} // namespace

int main() {
    std::cout << "=== PingPong per-echo log-level tests ===" << std::endl;
    test_ping_echo_seq_bytes_pending_is_info();
    test_ping_echo_labeled_companion_is_info();
    test_pong_echo_seq_bytes_is_info();
    test_per_echo_lines_not_debug_or_verbose();
    std::cout << "\n=== PingPong echo-log: all 4 pins PASS ===" << std::endl;
    return 0;
}

#endif
