// Split from AsyncRandomAdmissionTest.cpp (rule 20a: that file grew
// past the 15 KB cap once AL97-1 extended this pin to also cover a
// fresh SEQUENTIAL draw). Same fixture/root as that file's Pin 6 —
// see AsyncRandomAdmissionTest.cpp for the other five pins in this
// batch (RANDOM ceiling, maxLenForChunkBudget/maxLenForFreeWindow
// math).
//
// Pin: Ping::loop's send loop must gate admission on the live free
// window (effWindow_() - arqPendingCount()), sized against the real
// chunk cost of whatever's about to be attempted — a retained draw
// (chunksForMsgLen(pendingDrawLen_)) or a fresh SEQUENTIAL draw
// (chunksForMsgLen(seqSize_), AL97-1) — not a flat floor of 1.
// Revert any of: the live-window computation, either chunk-cost
// sizing, or the break check itself -> red.
#ifndef AUTOLINK_HOST_TEST
#    error "Build with -DAUTOLINK_HOST_TEST (see Makefile)"
#endif

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

void test_ping_loop_burst_gate_source_pin() {
    std::cout << "\n=== Ping::loop send loop holds a live free-window "
                 "burst gate ==="
              << std::endl;
    std::string src = readFile(projectRoot() + "/src/al/pingpong/Ping.h");
    assert(!src.empty() && "Ping.h must be readable");

    assert(
        src.find("int free_ = effWindow_() - base_.comm_.arqPendingCount();") !=
            std::string::npos &&
        "send loop must compute the live free window from "
        "effWindow_() (the runtime-clamped window), not the "
        "compile-time WINDOW constant");
    // Checked as two pieces rather than one concatenated literal,
    // which breaks on any whitespace reflow of the expression. See
    // PingGateChecksRetainedDrawChunkCostTest (retained draw) and
    // PingSequentialFreshDrawChunkCostTest (fresh SEQUENTIAL draw).
    assert(src.find("havePendingDraw_ ? chunksForMsgLen(pendingDrawLen_)") !=
               std::string::npos &&
           "send loop must size the pre-draw gate against the "
           "retained draw's real chunk cost, not a flat message-count "
           "floor of 1");
    assert(src.find("fillMode_ == FillMode::SEQUENTIAL") != std::string::npos &&
           src.find("chunksForMsgLen(seqSize_)") != std::string::npos &&
           "a fresh SEQUENTIAL draw must also be sized against its "
           "real chunk cost (chunksForMsgLen(seqSize_)), not the "
           "flat floor of 1 RANDOM correctly uses (RANDOM "
           "self-clamps in pickMsgSize_; SEQUENTIAL does not)");
    assert(src.find("if (free_ < neededChunks_) {") != std::string::npos &&
           "send loop must break out when the live free window can't "
           "fit the chunk cost of the message about to be attempted");

    // The gate must be inside the while-loop body, BEFORE the
    // pickMsgSize_/sendMsg path (it can't be a post-admit check —
    // that's the bug class the gate closes).
    auto loopBody = extractFnBody(src, "void loop()");
    assert(!loopBody.empty());
    auto burstPos = loopBody.find(
        "int free_ = effWindow_() - base_.comm_.arqPendingCount();");
    // A retained draw (havePendingDraw_) skips pickMsgSize_ entirely
    // and reuses pendingDrawLen_/pendingDrawCrc_ instead —
    // "n = pickMsgSize_(" (no `int` prefix; n is declared once above
    // the if/else) is the fresh-draw arm the gate must still precede.
    auto pickPos = loopBody.find("n = pickMsgSize_(");
    assert(burstPos != std::string::npos && pickPos != std::string::npos);
    assert(burstPos < pickPos &&
           "burst gate must precede the pickMsgSize_/sendMsg path");

    std::cout << "  PASS (live free-window burst gate precedes the draw)"
              << std::endl;
}

} // namespace

int main() {
    test_ping_loop_burst_gate_source_pin();
    std::cout << "\nAll AsyncPingLoopBurstGate pins passed." << std::endl;
    return 0;
}
