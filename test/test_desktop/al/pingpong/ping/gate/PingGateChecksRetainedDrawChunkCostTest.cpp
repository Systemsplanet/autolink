// AL88-5's havePendingDraw_ retains a draw across loop() calls
// after a GbnWindowFull rejection, so a redraw doesn't discard
// valid, already-CRC'd data. But the pre-draw gate that runs before
// deciding whether to reuse or redraw checked free_ (a message-slot
// count) against a floor of 1, while sendMsg's actual admission
// test is inflight + chunksForMsgLen(n) <= window (a chunk count).
// A retained multi-chunk draw passed the gate on a single free
// slot, was rejected again by sendMsg on the same chunk shortfall
// it failed on before, and got retained again unchanged — spinning
// at whatever rate loop() is called (GbnWindowFull deliberately
// skips the backpressure cooldown — see
// BackpressureCooldownSkipsWindowFullTest — so nothing throttled
// the retry), logging three lines per attempt. Field capture:
// 10,042 rejections of the same 951-byte draw in 1.1 s, tripping
// the ESP log ring's overflow guard 38 times.
//
// Fix: gate on the retained draw's real chunk cost
// (chunksForMsgLen(pendingDrawLen_)) when havePendingDraw_ is true,
// so a draw that no longer fits just waits — silently, no sendMsg
// call, no log line — for window to free up, instead of
// re-attempting a draw already known not to fit.
//
// Ping.h's send loop is Arduino-gated; see
// BackpressureCooldownSkipsWindowFullTest.cpp for why this suite
// follows the source-grep pattern used elsewhere in this file's
// test group instead of executing Ping::loop() directly. A real
// compile (not just source-grep) was also verified: `#define
// ARDUINO 10607` + `#include "al/pingpong/Ping.h"` under the
// ARDUINO stub set (test/scripts/env/install_system_stubs.py)
// syntax-checks clean with this change.
//
// Toggle off (revert the gate to `free_ < 1`) -> red.
#include <cassert>
#include <iostream>
#include "TestPaths.h"

using namespace autolink;

void test_gate_uses_chunk_cost_not_message_count() {
    std::cout << "\n=== Test: pre-draw gate checks chunksForMsgLen, not a "
                 "flat free_ < 1 ===\n";
    std::ifstream f(testRepoPath("src/al/pingpong/Ping.h"));
    assert(f.good());
    std::string src((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
    assert(!src.empty());

    auto loopPos = src.find("void loop()");
    assert(loopPos != std::string::npos);
    auto sendLoopPos =
        src.find("while (count_ < WINDOW && sentThisLoop < maxTx)", loopPos);
    assert(sendLoopPos != std::string::npos);
    // The gate is the first block after the send-loop header, before
    // the AL88-5 havePendingDraw_ branch that decides reuse-vs-redraw.
    auto gateEnd = src.find("if (havePendingDraw_)", sendLoopPos);
    assert(gateEnd != std::string::npos);
    std::string gate = src.substr(sendLoopPos, gateEnd - sendLoopPos);

    assert(gate.find("effWindow_() - base_.comm_.arqPendingCount()") !=
               std::string::npos &&
           "the gate must still compute the free window the same way");
    assert(gate.find("chunksForMsgLen(pendingDrawLen_)") != std::string::npos &&
           "the gate must size a RETAINED draw's admission cost in "
           "chunks (chunksForMsgLen(pendingDrawLen_)), not treat every "
           "draw as a single-slot cost — a multi-chunk retained draw "
           "that no longer fits must not pass on free_ >= 1 alone");
    assert(gate.find("havePendingDraw_ ?") != std::string::npos &&
           "the needed-chunks computation must branch on "
           "havePendingDraw_ — a fresh draw isn't sized yet at this "
           "point in the loop");

    std::cout << "  PASS (gate sizes a retained draw's real chunk cost)\n";
}

void test_no_flat_free_less_than_one_gate_remains() {
    std::cout << "\n=== Test: the old flat `free_ < 1` gate is gone ===\n";
    std::ifstream f(testRepoPath("src/al/pingpong/Ping.h"));
    std::string src((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());

    auto loopPos = src.find("void loop()");
    auto sendLoopPos =
        src.find("while (count_ < WINDOW && sentThisLoop < maxTx)", loopPos);
    auto gateEnd = src.find("if (havePendingDraw_)", sendLoopPos);
    std::string gate = src.substr(sendLoopPos, gateEnd - sendLoopPos);

    assert(gate.find("if (free_ < 1)") == std::string::npos &&
           "a bare `free_ < 1` admission check must not remain in the "
           "gate — every path must compare against the actual chunk "
           "cost of the message about to be attempted");

    std::cout << "  PASS (no flat free_ < 1 check remains)\n";
}

int main() {
    std::cout << "=== Running PingGateChecksRetainedDrawChunkCost Tests "
                 "===\n";
    test_gate_uses_chunk_cost_not_message_count();
    test_no_flat_free_less_than_one_gate_remains();
    std::cout << "\n=== PingGateChecksRetainedDrawChunkCost Tests "
                 "Completed Successfully ===\n";
    return 0;
}
