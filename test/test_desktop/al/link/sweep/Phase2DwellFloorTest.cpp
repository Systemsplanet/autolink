// Source-level regression test for the P2 dwell shape in
// LinkSweep::computeDwells.
//
// The slave-side per-baud dwell stays at a flat 250 ms so
// the slave's PONG reply lands inside the master's PING
// window on real hardware. The master-side per-baud dwell
// is the slave's full sweep time scaled by 1.1 so the master
// holds each baud long enough that a slave whose sweep
// started at any point still lands back on the master's
// current baud within the window.
//
// Pre-fix: the per-baud loop computed each dwell from
//   int rt = roundTripMs(...); int d = (int)(rt * 1.5) + 1;
//   if (d < 20) d = 20;
// and a second pass clamped anything still < 20 ms. At 115200
// roundTripMs returns ~1 ms, so the 20 ms clamp dominated —
// but the resulting 20 ms was still tighter than the typical
// FreeRTOS tick + UART event-task scheduling slack on real
// hardware, and the slave's PONG reply landed 1–2 ticks after
// the master's PING timer expired.
//
// Pre-pre-fix: a later change tightened the master dwell to a
// flat 250 ms (matching the slave). That made master dwell
// shorter than the slave's full sweep (250 × N), so the slave
// would P1-time-out while the master was still in P2 — a
// mutual-reset cascade. The current shape keeps the slave's
// 250 ms but inflates the master's per-baud dwell to 1.1×
// the slave's full sweep time.
//
// Each pin below is the structural guard. Toggle the fix off
// (revert computeDwells back to the per-baud rt/d shape,
// drop the slave back to the rt-based formula, or set the
// master dwell back to a flat constant) and at least one
// pin flips to red.
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

// Extracts the body of `LinkSweep::computeDwells` — only
// the text inside the outer { } braces, not any comment
// block above the signature.
std::string extractComputeDwellsBody(const std::string &sweepCpp) {
    auto fnPos = sweepCpp.find("void LinkSweep::computeDwells(");
    assert(fnPos != std::string::npos);
    auto brace = sweepCpp.find('{', fnPos);
    assert(brace != std::string::npos);
    int depth = 1;
    std::size_t endPos = std::string::npos;
    for (std::size_t i = brace + 1; i < sweepCpp.size(); i++) {
        if (sweepCpp[i] == '{')
            depth++;
        else if (sweepCpp[i] == '}') {
            depth--;
            if (depth == 0) {
                endPos = i;
                break;
            }
        }
    }
    assert(endPos != std::string::npos);
    return sweepCpp.substr(brace + 1, endPos - brace - 1);
}

// Pin 1: the per-baud loop must NOT call roundTripMs.
// Reverting to the rt/d formula trips this pin.
void test_phase2_dwell_no_round_trip_in_loop() {
    std::cout << "\n=== Pin 1: computeDwells per-baud loop has no "
                 "roundTripMs call ==="
              << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/link/sweep/LinkSweep.cpp");
    assert(!src.empty());
    std::string body = extractComputeDwellsBody(src);

    // Locate the per-baud for-loop body.
    auto forPos = body.find("for (int i = 0; i < N; i++)");
    assert(forPos != std::string::npos);
    auto openBrace = body.find('{', forPos);
    assert(openBrace != std::string::npos);
    auto closeBrace = body.find('}', openBrace);
    assert(closeBrace != std::string::npos);
    std::string loopBody = body.substr(openBrace, closeBrace - openBrace + 1);
    assert(loopBody.find("roundTripMs") == std::string::npos);
    std::cout << "  PASS (no roundTripMs inside the per-baud loop)"
              << std::endl;
}

// Pin 2: no "< " predicate (e.g. the old `if (d < 20)` floor
// clamp) can remain inside the function. The current shape
// fills both arrays to a flat constant.
void test_phase2_dwell_no_floor_predicate() {
    std::cout << "\n=== Pin 2: computeDwells has no < predicate ==="
              << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/link/sweep/LinkSweep.cpp");
    assert(!src.empty());
    std::string body = extractComputeDwellsBody(src);

    assert(body.find("if (d <") == std::string::npos);
    // No `if (... < ` predicate should remain at all — the
    // pre-fix shape had both an `if (d < 20)` clamp inside the
    // loop and a post-loop clamp block. Both gone now.
    assert(body.find("if (") == std::string::npos);
    std::cout << "  PASS (no < floor predicates remain)" << std::endl;
}

// Pin 3: slave per-baud dwell is the 250 ms literal; master
// per-baud dwell is derived from the slave's full sweep time
// via the 1.1 scale factor. Lowering either trips this pin.
void test_phase2_dwell_is_250_literal() {
    std::cout << "\n=== Pin 3: per-baud loop fills phase2 (1.1 * slave "
                 "sweep) / phase2Slave (250) ==="
              << std::endl;
    std::string root = projectRoot();
    std::string src = readFile(root + "/src/al/link/sweep/LinkSweep.cpp");
    assert(!src.empty());
    std::string body = extractComputeDwellsBody(src);

    // Slave stays flat 250 ms per baud.
    assert(body.find("phase2Slave[i] = 250;") != std::string::npos);
    // Master dwell must be derived from the slave's full sweep
    // via the 1.1 scale factor. Both must appear in the body.
    assert(body.find("250 * ctx.allowedBaudsCount()") != std::string::npos);
    assert(body.find("1.1f") != std::string::npos);
    std::cout << "  PASS (master=1.1*N*250, slave=250)" << std::endl;
}
} // namespace

int main() {
    std::cout << "=== Phase2 Dwell Floor 250ms regression ===" << std::endl;
    test_phase2_dwell_no_round_trip_in_loop();
    test_phase2_dwell_no_floor_predicate();
    test_phase2_dwell_is_250_literal();
    std::cout << "\n=== Done ===" << std::endl;
    return 0;
}
#endif
