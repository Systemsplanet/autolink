// Sweep-timeout arms in LinkTimers.cpp::onTimerSwp_unlocked.
//
// Both arms call pure decision helpers that are already
// table-tested in LinkDecisionTest (decideMasterPhase3Timeout
// / decidePongPhase2Timeout). The gap was the *Link* arms
// themselves — neither path fires during the happy-path
// sweep tests because those tests deliver PONG/timeout
// events that drive the sweep forward. The arms only fire
// when the dwell timer expires with no incoming frames,
// which a clock-injected host test can stage by pinning
// Link in P2/P3 via LinkTestAccessor, planting a short
// dwell timer arm in MockHal, and pumpClock()'ing past it.
//
// Pins:
//   1. Master P3 timeout, intermediate baud -> advance
//      to next baud (Stay) and re-arm. The Stay arm
//      computes next = phase3Baud_() + 1; with
//      phase3Baud_ = 2 of 5 bauds and baudCount = 5,
//      decideMasterPhase3Timeout(3, 5) == Stay.
//   2. Master P3 timeout, next baud past end -> fallback
//      lock at slowest baud. phase3Baud_ = 4 (last of 5);
//      decideMasterPhase3Timeout(5, 5) == FallbackLockSlowest
//      -> lockOk_unlocked(slowest=4, "p3-fallback").
//   3. Pong P2 timeout, spdI was 3 -> arm decrements to
//      2, decides Stay, re-arms timer. assert spdI==2
//      and timerActive afterwards.
//   4. Pong P2 timeout, spdI was 0 -> arm decrements to
//      -1, DropToPhase1 fires, sweep returns to P1
//      (slowest baud).
//
// Each pin explicitly stages a bad PING/timeout event
// that the sweep arm interprets as "the dwell expired
// with no response"; the assertions verify the link's
// reaction matches the decision helper's verdict.
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include "MockHal.h"
#    include "TestCfg.h"
#    include "LinkTestAccessor.h"
#    include "NullArqCache.h"
#    include "al/AutoLinkConfig.h"
#    include "al/util/Log.h"
#    include "al/link/sweep/LinkSweep.h"

using namespace autolink;

static void mkCfg(AutoLinkConfig &cfg) {
    testBaseCfg(cfg);
    cfg.allowedBauds[0] = 115200;
    cfg.allowedBauds[1] = 57600;
    cfg.allowedBauds[2] = 38400;
    cfg.allowedBauds[3] = 19200;
    cfg.allowedBauds[4] = 9600;
    cfg.allowedBaudsCount = 5;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 0;
}

// Pin 1: master P3 timeout, intermediate baud (3 of 5)
// drives advance-to-next-baud (Stay). After the arm
// fires, spdI must move to next+1 and a PING must be
// on the wire (sweep re-arm).
static void test_master_p3_timeout_intermediate_advances() {
    std::cout << "\n=== Pin 1: master P3 timeout, intermediate baud -> Stay ==="
              << std::endl;
    AutoLinkConfig cfg;
    mkCfg(cfg);
    NullArqCache cache;
    MockHal mHal;
    Link ping(mHal, cache, true /*master*/, cfg);
    ping.begin();
    LinkTestAccessor pingT(ping);

    // enterPhase3 sets phase3Baud_=chosenBaud inside
    // the sweep state machine. Link itself implements
    // ISweepCtx, so we can pass ping in directly as the
    // ctx. This sidesteps driving the full SWP
    // handshake while still landing phase3Baud_=2.
    // The sweep arm reads phase3Baud_() + 1 = 3, so
    // decideMasterPhase3Timeout(3, 5) -> Stay.
    pingT.sweep().enterPhase3(ping, 2);
    assert(pingT.sweepPhase() == SweepPhase::PHASE3);
    assert(pingT.phase3Baud() == 2);
    // Plant a 100 ms dwell arm via MockHal directly.
    // (The sweep enterPhase3 already arms dw3 via
    // hw.startTimer; clear and re-arm explicitly to
    // make the timer a known short value.)
    pingT.sweep().dwells().phase3 = 100;
    mHal.now = 0;
    mHal.startTimer(100);
    mHal.pumpClock(150); // dwell + safety margin

    // The P3 Stay arm does sweep_.reset() (phase ->
    // NONE), spdI = next = 3, emits PING, re-arms a
    // phase2 dwell. spdI advances even though phase
    // dropped to NONE — the baud advance is the testable
    // assertion of the Stay path (the dwell re-emission
    // is gated by MockHal's txBuf visibility through the
    // PING frame).
    assert(ping.getCurrentSpdIndex() == 3);
    assert(!mHal.txBuf.empty() &&
           "master P3 Stay must re-emit PING to keep the sweep alive");
    std::cout << "  PASS (advanced spdI 2->3, re-armed PING)" << std::endl;
}

// Pin 2: master P3 timeout, next baud already past end
// -> FallbackLockSlowest path. Asserts the link locks
// at the slowest baud with state=OK.
static void test_master_p3_timeout_at_end_falls_back_to_slowest() {
    std::cout << "\n=== Pin 2: master P3 timeout at baud list end -> "
                 "FallbackLockSlowest ==="
              << std::endl;
    AutoLinkConfig cfg;
    mkCfg(cfg);
    NullArqCache cache;
    MockHal mHal;
    Link ping(mHal, cache, true /*master*/, cfg);
    ping.begin();
    LinkTestAccessor pingT(ping);

    // phase3Baud_=4 (last of 5); next = 5, >= baudCount=5
    // -> FallbackLockSlowest path.
    pingT.sweep().enterPhase3(ping, 4);
    assert(pingT.phase3Baud() == 4);
    pingT.sweep().dwells().phase3 = 100;
    mHal.now = 0;
    mHal.startTimer(100);
    mHal.pumpClock(150);

    // Fallback path: lockOk_unlocked(slowest=4, "p3-fallback")
    // -> state OK, spdI = slowest.
    assert(pingT.sweepPhase() == SweepPhase::NONE);
    assert(ping.getState() == State::OK);
    assert(ping.getCurrentSpdIndex() == cfg.allowedBaudsCount - 1);
    std::cout << "  PASS (locked at slowest baud, state=OK)" << std::endl;
}

// Pin 3: pong P2 timeout, spdI=3 -> arm decrements to
// 2 (Stay path), timer re-arms for the next P2 dwell.
static void test_pong_p2_timeout_intermediate_advances() {
    std::cout << "\n=== Pin 3: pong P2 timeout, intermediate baud -> "
                 "Stay (spdI decrements) ==="
              << std::endl;
    AutoLinkConfig cfg;
    mkCfg(cfg);
    NullArqCache cache;
    MockHal mHal;
    Link pong(mHal, cache, false /*pong*/, cfg);
    pong.begin();
    LinkTestAccessor pongT(pong);

    pongT.sweep().setPhase(SweepPhase::PHASE2);
    pongT.setSpdI(3);
    pongT.sweep().dwells().phase2Slave[3] = 100;
    mHal.now = 0;
    mHal.startTimer(100);
    mHal.pumpClock(150);

    // After Stay: spdI = 2 (the arm decrements before
    // the Stay verdict is computed with the new spdI;
    // verify the end-state, not the in-arm intermediate).
    assert(pongT.sweepPhase() == SweepPhase::PHASE2);
    assert(pong.getCurrentSpdIndex() == 2);
    assert(mHal.timerActive && "Stay path must re-arm for the next dwell");
    std::cout << "  PASS (decremented spdI 3->2, re-armed)" << std::endl;
}

// Pin 4: pong P2 timeout, spdI=0 -> decrement inside
// arm makes spdI=-1 -> DropToPhase1 -> enterPhase1
// fires, sweep returns to PHASE1.
static void test_pong_p2_timeout_at_top_drops_to_phase1() {
    std::cout << "\n=== Pin 4: pong P2 timeout at spdI=0 -> "
                 "DropToPhase1 ==="
              << std::endl;
    AutoLinkConfig cfg;
    mkCfg(cfg);
    NullArqCache cache;
    MockHal mHal;
    Link pong(mHal, cache, false /*pong*/, cfg);
    pong.begin();
    LinkTestAccessor pongT(pong);

    pongT.sweep().setPhase(SweepPhase::PHASE2);
    pongT.setSpdI(0);
    pongT.sweep().dwells().phase2Slave[0] = 100;
    mHal.now = 0;
    mHal.startTimer(100);
    mHal.pumpClock(150);

    // After DropToPhase1: sweep returns to P1. setSpdI
    // is whatever enterPhase1 sets (slowest baud).
    assert(pongT.sweepPhase() == SweepPhase::PHASE1);
    assert(pong.getCurrentSpdIndex() == cfg.allowedBaudsCount - 1);
    std::cout << "  PASS (returned to P1 slowest, spdI=4)" << std::endl;
}

int main() {
    std::cout << "=== Running Sweep-Timeout Arms Tests ===" << std::endl;
    Log::log().setLevel(Log::WARNING);
    test_master_p3_timeout_intermediate_advances();
    test_master_p3_timeout_at_end_falls_back_to_slowest();
    test_pong_p2_timeout_intermediate_advances();
    test_pong_p2_timeout_at_top_drops_to_phase1();
    std::cout << "\n=== Sweep-Timeout Arms Tests Completed Successfully ==="
              << std::endl;
    return 0;
}

#endif
