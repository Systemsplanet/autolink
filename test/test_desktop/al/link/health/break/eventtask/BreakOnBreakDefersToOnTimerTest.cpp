// onBreak()'s second-BREAK fast-confirm branch (a BREAK arriving
// outside BREAK_COALESCE_MS while one is already suspect) previously
// called reset_unlocked() inline. reset_unlocked() enters SWP via
// sweep_.enterPhase1/enterPhase3, and both call hw.setSpd()
// synchronously — the identical hazard BreakStormDefersToOnTimerTest
// already pins for onBreakStorm(): onBreak() also runs on the UART
// event task, which the ESP-IDF driver already holds its own lock
// inside while dispatching the event, so a reentrant setSpd()
// deadlocks the same shape.
//
// Fix: mirror onBreakStorm()'s defer — set a flag
// (breakConfirmPending_) and consume it at the top of onTimer(),
// where the state machine already owns the setSpd/startTimer
// sequence. onBreak() also arms a near-immediate (1 ms) timer so
// the deferred confirm still lands on the next tick rather than
// waiting on whatever the link's normal cadence next happened to
// schedule.
//
// Pin: arm the first BREAK, advance past the coalesce window, fire
// the second BREAK, observe breakConfirmPending_ == true and state
// still OK (deferred). Pump 1 ms (onBreak()'s own armed timer) and
// observe state == SWP, reason == HealthWatchdog,
// breakConfirmPending_ cleared. Toggle off (revert to the inline
// reset_unlocked call) -> red: state is already SWP immediately
// after the second onBreak(), before any timer pump.
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include "MockHal.h"
#    include "TestCfg.h"
#    include "NullArqCache.h"
#    include "accessors/LinkTestAccessor.h"
#    include "al/link/timers/LinkBreak.h"

int main() {
    using namespace autolink;
    std::cout << "=== onBreak() second-BREAK fast-confirm defers to "
                 "onTimer ===\n";
    AutoLinkConfig cfg;
    testBaseCfg(cfg);
    cfg.allowedBauds[0] = 115200;
    cfg.allowedBaudsCount = 1;
    cfg.idleTimeoutMs = 0;

    NullArqCache cache;
    MockHal mHal;
    Link link(mHal, cache, /*isMaster=*/true, cfg);
    link.begin();
    LinkTestAccessor t(link);
    t.forceStateNoLock(State::OK);
    t.setSpdI(0);
    mHal.spd = 115200;

    // First BREAK: arms suspicion, nothing deferred yet.
    link.onBreak();
    assert(link.getState() == State::OK &&
           "first BREAK must only arm suspicion");
    assert(t.breakSuspectMsForTest() != 0 &&
           "breakSuspectMs_ must be armed after the first BREAK");
    assert(t.breakConfirmPendingForTest() == false);

    // Advance past BREAK_COALESCE_MS so the second BREAK is an
    // independent event, well under BREAK_CONFIRM_MS (150 ms at
    // 115200) so the timeout-confirm path can't fire first.
    mHal.pumpClock(LinkBreakConsts::BREAK_COALESCE_MS + 5);

    // Second BREAK: must defer, not reset inline.
    link.onBreak();
    assert(t.breakConfirmPendingForTest() == true &&
           "second BREAK outside the coalesce window must set "
           "breakConfirmPending_ (work is deferred to onTimer)");
    assert(link.getState() == State::OK &&
           "state must be unchanged immediately after the second "
           "BREAK — the reset itself has not run yet");

    // onBreak() armed a 1 ms timer for the deferred confirm; pump
    // exactly that far (mirrors BreakSuspectKeepaliveTest Pin 1's
    // reliance on pumpClock's own one-shot timer firing, not a
    // manual onTimer() call, so this proves the real scheduling
    // path, not a test-forced one).
    mHal.pumpClock(1);
    assert(link.getState() == State::SWP &&
           "the deferred confirm must fire on the next tick (1 ms "
           "later), not wait out the full BREAK_CONFIRM_MS window");
    assert(t.lastResetReasonForTest() == ResetReason::HealthWatchdog);
    assert(t.breakConfirmPendingForTest() == false &&
           "deferred flag must clear after consumption");

    std::cout << "  PASS (second BREAK deferred, confirmed on the next "
                 "tick via reset_unlocked)\n";
    std::cout << "=== BreakOnBreakDefersToOnTimer: PASS ===\n";
    return 0;
}

#endif
