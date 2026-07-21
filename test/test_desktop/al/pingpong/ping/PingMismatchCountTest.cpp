// Regression pin for  (Ping-side echo
// diagnostic): mismatchCount_ must be incremented in the
// `got < 0` (CRC/desync) branch of Ping::loop so the
// periodic logStats() line surfaces a meaningful count.

#include <cassert>
#include <iostream>
#include <string>

namespace {

std::string projectRoot() {
    const char *env = std::getenv("AUTOLINK_TEST_ROOT");
    return env ? env : "../..";
}

std::string readFile(const std::string &path) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return std::string();
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
        out.append(buf, n);
    std::fclose(f);
    return out;
}

// Walk one brace-balanced block starting at `start`.
std::string extractBranch(const std::string &body, size_t start) {
    // Find the opening '{' from `start` forward.
    size_t open = body.find('{', start);
    if (open == std::string::npos)
        return std::string();
    int depth = 0;
    for (size_t i = open; i < body.size(); i++) {
        if (body[i] == '{')
            depth++;
        else if (body[i] == '}') {
            depth--;
            if (depth == 0)
                return body.substr(open, i + 1 - open);
        }
    }
    return std::string();
}

// Pin 1: mismatchCount_ bump lives in the got<0 branch
// of Ping::loop. The field must actually be incremented,
// not just declared and passed to logStats().
void test_mismatch_count_bumps_on_got_neg() {
    std::cout << "\n=== Pin 1: got<0 branch increments mismatchCount_ ==="
              << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/pingpong/Ping.h");
    assert(!src.empty());

    // Locate the loop body and the got<0 branch.
    auto loopPos = src.find("void loop()");
    assert(loopPos != std::string::npos);
    auto gotNegPos = src.find("if (got < 0)", loopPos);
    assert(gotNegPos != std::string::npos);

    // Pull the branch body and assert mismatchCount_++
    // lives between the `if (got < 0) {` opening and the
    // matching closing brace.
    std::string branch = extractBranch(src, gotNegPos);
    assert(!branch.empty());
    auto bumpPos = branch.find("mismatchCount_++");
    assert(bumpPos != std::string::npos);
    // The increment must come AFTER the diag log that
    // surfaces the rejection ("recv rejected (CRC/desync)")
    // so the operator sees the type before the counter
    // ticks.
    auto diagLog = branch.find("recv rejected (CRC/desync)");
    assert(diagLog != std::string::npos);
    assert(diagLog < bumpPos);
    // And the bump must come BEFORE clearQueue_() so the
    // counter survives the queue wipe (otherwise the
    // periodic logStats reads zero forever, defeating the
    // counter's purpose).
    auto clearPos = branch.find("clearQueue_()");
    assert(clearPos != std::string::npos);
    assert(bumpPos < clearPos);

    std::cout << "  PASS (got<0 -> mismatchCount_++ after diag log, "
                 "before clearQueue_)"
              << std::endl;
}

// Pin 2: the labeled-companion echo log line is present
// at both echo sites (the gap-stop branch and the main
// loop's tail queue drain). The two existing tests pin
// the legacy "echo %u %u %d" format AND the second
// argument being queue_[head_].len — the labeled line
// supplements them, not replaces them.
void test_labeled_echo_companion_at_both_sites() {
    std::cout << "\n=== Pin 2: labeled echo companion at both ack sites ==="
              << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/pingpong/Ping.h");
    assert(!src.empty());

    // The labeled format must be present.
    auto lblPos = src.find("echo#=%llu seq=%u msgBytes=%u pending=%d");
    assert(lblPos != std::string::npos);

    // Pull the loop body and count occurrences of the labeled
    // call. We expect exactly 2 sites to mirror the two legacy
    // "echo %u %u %d" sites (gap-stop + tail drain).
    auto loopPos = src.find("void loop()");
    assert(loopPos != std::string::npos);
    std::string body = src.substr(loopPos);
    int lblSites = 0;
    size_t searchFrom = 0;
    while (true) {
        auto p = body.find("\"echo#=%llu seq=%u msgBytes=%u pending=%d\"",
                           searchFrom);
        if (p == std::string::npos)
            break;
        lblSites++;
        searchFrom = p + 1;
    }
    assert(lblSites == 2);

    // Cross-check with the legacy format — both must be at 2 sites
    // so an operator gets one labeled line per legacy echo.
    int echoSites = 0;
    searchFrom = 0;
    while (true) {
        auto p = body.find("\"echo %u %u %d\"", searchFrom);
        if (p == std::string::npos)
            break;
        echoSites++;
        searchFrom = p + 1;
    }
    assert(echoSites == 2);

    std::cout
        << "  PASS (2 labeled echo sites; 2 legacy echo sites; format-string pair)"
        << std::endl;
}

// Pin 3: the labeled companion reads its echo number
// from successEchoCount_ (the monotonic-per-ack counter),
// not from a slot field. This is what makes the line
// monotonic across both fill modes — if the bench ever
// shows echo#= jumping, that's the counter that's wrong
// (real corruption), not the msgBytes field.
void test_labeled_echo_reads_success_echo_count() {
    std::cout << "\n=== Pin 3: labeled echo reads successEchoCount_ "
                 "(monotonic across fill modes) ==="
              << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/pingpong/Ping.h");
    assert(!src.empty());

    auto loopPos = src.find("void loop()");
    assert(loopPos != std::string::npos);
    std::string body = src.substr(loopPos);

    auto lblPos = body.find("\"echo#=%llu seq=%u msgBytes=%u pending=%d\"");
    assert(lblPos != std::string::npos);
    // Look 400 chars forward — enough to cover the
    // (successEchoCount_, queue_[head_].seq, ..., ...)
    // arg list without being so long we catch a
    // next-call-site's argument list.
    std::string tail = body.substr(lblPos, 400);
    auto cntPos = tail.find("successEchoCount_");
    assert(cntPos != std::string::npos);
    // Also ensure it's BEFORE any further format string, so
    // we know we sampled the labeled line's arguments, not
    // a downstream call.
    auto nextFmt = tail.find("\"", cntPos);
    auto nextLogCall = tail.find("log_.debug(\"Ping\"", cntPos);
    assert(nextLogCall == std::string::npos || nextFmt == std::string::npos ||
           nextFmt < nextLogCall);

    std::cout << "  PASS (labeled echo reads successEchoCount_ as first arg)"
              << std::endl;
}

} // namespace

int main() {
    std::cout << "=== Running Ping Mismatch Count Tests ===" << std::endl;
    test_mismatch_count_bumps_on_got_neg();
    test_labeled_echo_companion_at_both_sites();
    test_labeled_echo_reads_success_echo_count();
    std::cout << "\n=== Ping Mismatch Count Tests Completed ===" << std::endl;
    return 0;
}
