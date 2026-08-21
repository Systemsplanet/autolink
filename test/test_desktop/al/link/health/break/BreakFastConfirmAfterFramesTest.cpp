// The second-BREAK fast-confirm path must be gated on
// breakSuspectSeen_ == 0. If a qualifying frame has
// been observed since the arm, the second BREAK is
// treated as a re-arm of the confirm deadline, not an
// immediate reset. Without this gate, a HAL that
// debounces at 120 ms can deliver a second BREAK
// during the two-frame-clear window and tear down a
// healthy link.
//
// Pin: arm the suspect (first BREAK), record a
// qualifying frame (breakSuspectSeen_ advances), then
// deliver a second BREAK. The link must still be in
// OK and the timer re-armed. Toggle off (revert to
// immediate reset) -> red: link tears down to SWP.
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include "MockHal.h"
#    include "TestCfg.h"
#    include "NullArqCache.h"
#    include "accessors/LinkTestAccessor.h"

int main() {
    using namespace autolink;
    std::cout << "=== Second BREAK after frame: re-arm, not reset ===\n";
    AutoLinkConfig cfg;
    testBaseCfg(cfg);
    cfg.allowedBauds[0] = 115200;
    cfg.allowedBaudsCount = 1;
    cfg.idleTimeoutMs = 0;

    MockHal mHal, sHal;
    NullArqCache cache;
    Link link(mHal, cache, true, cfg);
    link.begin();
    LinkTestAccessor t(link);

    t.forceStateNoLock(State::OK);
    t.setSpdI(0);
    mHal.spd = 115200;

    link.onBreak();
    t.setBreakSuspectSeenForTest(1);
    int armedBefore = mHal.lastTimerMs;

    mHal.now = LinkBreakConsts::BREAK_COALESCE_MS + 50;
    sHal.now = LinkBreakConsts::BREAK_COALESCE_MS + 50;
    link.onBreak();
    mHal.pumpClock(2);
    sHal.pumpClock(2);

    assert(link.getState() == State::OK &&
           "Second BREAK after a qualifying frame must re-arm, not reset");
    assert(mHal.timerStartCalls > 0 && mHal.lastTimerMs >= armedBefore / 2 &&
           "Confirm timer must be re-armed on second BREAK after frame");

    std::cout << "  PASS (link stayed OK, confirm re-armed)\n";
    std::cout << "=== BreakFastConfirmAfterFrames: PASS ===\n";
    return 0;
}

#endif
