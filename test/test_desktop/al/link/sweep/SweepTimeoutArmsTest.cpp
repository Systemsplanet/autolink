// Sweep-timeout arms in LinkTimers.cpp::onTimerSwp_unlocked.
//
// Both arms call pure decision helpers that are already
// table-tested in LinkDecisionTest (decideMasterPhase3Timeout
// / decidePongPhase2Timeout). The gap was *Link* arms
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
#    include "al/util/log/Log.h"
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
// -> FallbackLockSlowest *denied* (the current release item 8: p3-
// fallback now refuses OK when sweepPongCount_ < 1, the
// field-log signature of master declaring OK against a
// peer that never answered). The link falls back to a
// P1 walk from the slowest baud, with the existing
// backoff — the next round will try again from a fresh
// slowest-baud position. Pinned by FallbackRequiresPongTest.
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

    // Fallback denied: P1 walk from slowest baud, state
    // stays SWP, phase = PHASE1, spdI = slowest, timer
    // re-armed (a fresh P1 dwell).
    assert(ping.getState() == State::SWP);
    assert(pingT.sweepPhase() == SweepPhase::PHASE1);
    assert(ping.getCurrentSpdIndex() == cfg.allowedBaudsCount - 1);
    std::cout << "  PASS (P3 fallback denied without PONG — P1 walk, "
                 "spdI=slowest, state=SWP)"
              << std::endl;
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

// Pin 4: pong P2 timeout, spdI=0 -> the slave
// stays at the proven baud (spdI=0) and lets the
// master PING (every idleTimeoutMs/2) promote it
// to P3. Guarded by spdI <= 0 in LinkTimersSwp.cpp
// so a single-baud config doesn't decrement into
// negative space and fall back to P1.
static void test_pong_p2_timeout_at_top_stays_in_p2() {
    std::cout << "\n=== Pin 4: pong P2 timeout at spdI=0 -> "
                 "stay in P2 at proven baud ==="
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

    // After the spdI<=0 multi-baud wrap (item 2): sweep stays
    // in PHASE2, spdI wraps to slowest (N-1). The wrap
    // path setSpd at N-1 BEFORE returning, so the
    // master's P1/fallback baud is actually listened at.
    // The single-baud guard (cfg.allowedBaudsCount<=1)
    // still keeps spdI=0; this test exercises the
    // multi-baud path.
    assert(pongT.sweepPhase() == SweepPhase::PHASE2 &&
           "Pin 4: slave P2 at spdI=0 in multi-baud must stay in "
           "PHASE2, not fall to PHASE1 (the item 2 wrap keeps the "
           "walk alive instead of camping at 512000)");
    assert(pong.getCurrentSpdIndex() == cfg.allowedBaudsCount - 1 &&
           "Pin 4: spdI must land on slowest (N-1), the baud the "
           "master's P1/fallback is parked on");
    std::cout << "  PASS (stayed in P2, wrapped spdI to slowest)" << std::endl;
}

int main() {
    std::cout << "=== Running Sweep-Timeout Arms Tests ===" << std::endl;
    Log::log().setLevel(Log::WARNING);
    test_master_p3_timeout_intermediate_advances();
    test_master_p3_timeout_at_end_falls_back_to_slowest();
    test_pong_p2_timeout_intermediate_advances();
    test_pong_p2_timeout_at_top_stays_in_p2();
    std::cout << "\n=== Sweep-Timeout Arms Tests Completed Successfully ==="
              << std::endl;
    return 0;
}

#endif
