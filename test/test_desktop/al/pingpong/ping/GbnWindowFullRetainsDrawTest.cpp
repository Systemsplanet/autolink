// AL88-5: a draw rejected with GbnWindowFull carries no wire fault —
// the window was simply full at that instant. Redrawing (especially
// under RANDOM fill, which picks a new random size on every call)
// discards valid, already-CRC'd data for no reason tied to the
// rejection. This suite pins that a rejected draw is retained
// (sendBuf_/len/crc) and replayed on the next attempt rather than
// redrawn, and that the retained state is invalidated on a real
// session reset (clearQueue_) rather than carried across link
// sessions.
//
// Ping.h's send loop is Arduino-gated; see
// BackpressureCooldownSkipsWindowFullTest.cpp for why this suite
// follows the source-grep pattern used elsewhere in this file's test
// group instead of executing Ping::loop() directly.
//
// Toggle off (drop havePendingDraw_ / always redraw) -> red.

#include <cassert>
#include <iostream>
#include "TestPaths.h"

using namespace autolink;

void test_pending_draw_state_declared() {
    std::cout << "\n=== Test: Ping declares pending-draw retention state "
                 "===\n";
    std::ifstream f(testRepoPath("src/al/pingpong/Ping.h"));
    assert(f.good());
    std::string src((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
    assert(!src.empty());

    assert(src.find("bool havePendingDraw_") != std::string::npos &&
           "Ping must declare havePendingDraw_ to track a draw rejected "
           "by GbnWindowFull across loop() calls");
    assert(src.find("int pendingDrawLen_") != std::string::npos);
    assert(src.find("uint16_t pendingDrawCrc_") != std::string::npos);
}

void test_send_loop_reuses_retained_draw_before_drawing_fresh() {
    std::cout << "\n=== Test: send loop reuses a retained draw instead of "
                 "redrawing ===\n";
    std::ifstream f(testRepoPath("src/al/pingpong/Ping.h"));
    std::string src((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());

    auto loopPos = src.find("void loop()");
    assert(loopPos != std::string::npos);
    auto sendLoopPos =
        src.find("while (count_ < WINDOW && sentThisLoop < maxTx)", loopPos);
    assert(sendLoopPos != std::string::npos);
    auto sendLoopEnd = src.find("queue_[tail_].len = n", sendLoopPos);
    assert(sendLoopEnd != std::string::npos);
    std::string sendLoopSlice =
        src.substr(sendLoopPos, sendLoopEnd - sendLoopPos);

    auto branchPos = sendLoopSlice.find("if (havePendingDraw_)");
    assert(branchPos != std::string::npos &&
           "the send loop must branch on havePendingDraw_ before drawing "
           "a fresh message — sizing n/crc from pendingDrawLen_/"
           "pendingDrawCrc_ in that branch, and only calling "
           "pickMsgSize_/fillBuf_ in the else");
    std::string branch = sendLoopSlice.substr(branchPos, 400);
    assert(branch.find("pendingDrawLen_") != std::string::npos);
    assert(branch.find("pendingDrawCrc_") != std::string::npos);
    assert(branch.find("pickMsgSize_") != std::string::npos &&
           "the else arm of the pending-draw branch must still call "
           "pickMsgSize_ for a fresh draw when nothing is retained");

    std::cout << "  PASS (send loop prefers a retained draw over a fresh "
                 "one)\n";
}

void test_draw_retained_only_on_gbn_window_full() {
    std::cout << "\n=== Test: draw is retained on GbnWindowFull, dropped on "
                 "other failures ===\n";
    std::ifstream f(testRepoPath("src/al/pingpong/Ping.h"));
    std::string src((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());

    auto setPos = src.find("havePendingDraw_ = true;");
    assert(setPos != std::string::npos &&
           "havePendingDraw_ must be set true somewhere on a rejected send");
    // The nearest enclosing branch must be the GbnWindowFull arm, not
    // the general failure path — an `else` immediately preceding this
    // assignment (paired with the cooldown-arming `if` that excludes
    // GbnWindowFull) is the shape the fix takes.
    std::string before = src.substr(0, setPos);
    auto elsePos = before.rfind("} else {");
    assert(elsePos != std::string::npos);
    std::string window = src.substr(elsePos, setPos - elsePos);
    assert(window.find("pendingDrawLen_ = n;") != std::string::npos &&
           window.find("pendingDrawCrc_ = crc;") != std::string::npos &&
           "the retention branch must stash the current n/crc, or a "
           "later successful reuse would replay stale bytes");

    // Any other failure branch must clear havePendingDraw_ rather than
    // leave a stale draw straddling an unrelated cause (e.g. the link
    // going NotOk).
    assert(src.find("havePendingDraw_ = false;") != std::string::npos &&
           "a non-window-full failure must clear havePendingDraw_ so a "
           "stale draw is never replayed against a different failure "
           "cause or a reset session");

    std::cout << "  PASS (retention is GbnWindowFull-specific)\n";
}

void test_clear_queue_invalidates_pending_draw() {
    std::cout << "\n=== Test: clearQueue_() invalidates a retained draw "
                 "===\n";
    std::ifstream f(testRepoPath("src/al/pingpong/Ping.h"));
    std::string src((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());

    auto fnPos = src.find("void clearQueue_() {");
    assert(fnPos != std::string::npos);
    auto fnEnd = src.find("\n    }", fnPos);
    assert(fnEnd != std::string::npos);
    std::string body = src.substr(fnPos, fnEnd - fnPos);
    assert(body.find("havePendingDraw_ = false;") != std::string::npos &&
           "clearQueue_() must invalidate a retained draw — it belongs to "
           "the session being torn down (link-lost, pipeline stall) and "
           "replaying it into a fresh session risks stale bytes racing "
           "the app's local seq/queue state");

    std::cout << "  PASS (clearQueue_ invalidates the retained draw)\n";
}

int main() {
    std::cout << "=== Running GbnWindowFullRetainsDraw Tests ===" << std::endl;
    test_pending_draw_state_declared();
    test_send_loop_reuses_retained_draw_before_drawing_fresh();
    test_draw_retained_only_on_gbn_window_full();
    test_clear_queue_invalidates_pending_draw();
    std::cout << "\n=== GbnWindowFullRetainsDraw Tests Completed "
                 "Successfully ==="
              << std::endl;
    return 0;
}
