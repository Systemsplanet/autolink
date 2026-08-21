// AL97-1: the pre-draw gate's neededChunks_ computation charged a
// FRESH draw 1 chunk regardless of fill mode. RANDOM's pickMsgSize_
// self-clamps to whatever is free, so 1 is always right for it. But
// SEQUENTIAL's draw size (seqSize_) is already known at gate time,
// is NOT clamped to the free window (the ramp 1..maxMsg is the test
// contract SEQUENTIAL exists to exercise — pickMsgSize_ deliberately
// has no live-free-window clamp on that branch, unlike RANDOM's),
// and can cost far more than 1 chunk. Gating it as 1 let every ramp
// size past the free window reach sendMsg blind and get rejected —
// a field capture at sustained 512000-baud ASYNC traffic logged 74
// GbnWindowFull rejections directly attributable to this gap (e.g.
// n=504 => chunksForMsgLen(504)=4 chunks, admitted past a gate that
// only checked for 1).
//
// Fix: a fresh SEQUENTIAL draw's gate cost is
// chunksForMsgLen(seqSize_), same formula PingGateChecksRetained-
// DrawChunkCostTest already pins for the RETAINED-draw case. RANDOM
// keeps its cost at 1 (unchanged) since its self-clamp makes that
// correct.
//
// Same source-grep pattern as PingGateChecksRetainedDrawChunkCostTest
// and its neighbors in this directory: Ping.h's send loop is
// Arduino-gated, so this suite verifies the gate's shape by source
// inspection rather than executing Ping::loop() directly (see
// BackpressureCooldownSkipsWindowFullTest.cpp for the rationale).
//
// Toggle off (revert the SEQUENTIAL branch to a flat `1`) -> red.
#include <cassert>
#include <fstream>
#include <iostream>
#include <string>
#include "TestPaths.h"

using namespace autolink;

static std::string readGateBlock() {
    std::ifstream f(testRepoPath("src/al/pingpong/Ping.h"));
    std::string src((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
    auto loopPos = src.find("void loop()");
    assert(loopPos != std::string::npos);
    auto sendLoopPos =
        src.find("while (count_ < WINDOW && sentThisLoop < maxTx)", loopPos);
    assert(sendLoopPos != std::string::npos);
    auto gateEnd = src.find("if (havePendingDraw_)", sendLoopPos);
    assert(gateEnd != std::string::npos);
    return src.substr(sendLoopPos, gateEnd - sendLoopPos);
}

void test_fresh_sequential_draw_costs_real_chunks() {
    std::cout << "\n=== Test: a fresh SEQUENTIAL draw's gate cost is "
                 "chunksForMsgLen(seqSize_), not a flat 1 ===\n";
    std::string gate = readGateBlock();
    assert(!gate.empty());

    assert(gate.find("havePendingDraw_ ? chunksForMsgLen(pendingDrawLen_)") !=
               std::string::npos &&
           "the retained-draw branch must be unchanged (still pinned "
           "by PingGateChecksRetainedDrawChunkCostTest)");
    assert(gate.find("fillMode_ == FillMode::SEQUENTIAL") != std::string::npos &&
           "the needed-chunks computation must branch on fillMode_ for "
           "the fresh-draw case — a fresh SEQUENTIAL draw is already "
           "sized (seqSize_) and must not be treated as a flat 1-chunk "
           "cost the way a fresh RANDOM draw correctly is");
    assert(gate.find("chunksForMsgLen(seqSize_)") != std::string::npos &&
           "a fresh SEQUENTIAL draw's chunk cost must be computed from "
           "seqSize_ via chunksForMsgLen, matching sendMsg's real "
           "admission test (inflight + chunksForMsgLen(n) <= window)");

    std::cout << "  PASS (fresh SEQUENTIAL draw gated on its real chunk "
                 "cost)\n";
}

void test_fresh_random_draw_still_costs_one() {
    std::cout << "\n=== Test: a fresh RANDOM draw's gate cost stays 1 "
                 "(self-clamped by pickMsgSize_) ===\n";
    std::string gate = readGateBlock();

    // The ternary's else-branch for a non-SEQUENTIAL fresh draw must
    // still resolve to 1 — RANDOM's pickMsgSize_ already clamps to
    // whatever is free, so the gate doesn't need to re-derive a cost
    // for it. This is a narrower check than a bare source-string
    // search for "1" (which would match unrelated code): confirm the
    // ternary's shape is `(fillMode_ == SEQUENTIAL ? ... : 1)`.
    auto seqPos = gate.find("fillMode_ == FillMode::SEQUENTIAL");
    assert(seqPos != std::string::npos);
    auto elsePos = gate.find(": 1)", seqPos);
    assert(elsePos != std::string::npos &&
           "the non-SEQUENTIAL fresh-draw branch of the ternary must "
           "still resolve to a flat 1 — RANDOM's pickMsgSize_ "
           "self-clamps to the free window, so re-deriving a chunk "
           "cost for it here would be redundant, not wrong, but a "
           "different value here signals an unintended behavior "
           "change for RANDOM");

    std::cout << "  PASS (fresh RANDOM draw still gated at 1)\n";
}

int main() {
    std::cout << "=== Running PingSequentialFreshDrawChunkCost Tests "
                 "===\n";
    test_fresh_sequential_draw_costs_real_chunks();
    test_fresh_random_draw_still_costs_one();
    std::cout << "\n=== PingSequentialFreshDrawChunkCost Tests "
                 "Completed Successfully ===\n";
    return 0;
}
