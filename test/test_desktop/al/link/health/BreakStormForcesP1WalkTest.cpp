// A BREAK storm (N suppressed BREAKs in one window)
// must take action: the link layer's preferredBaud_
// is cleared and a P1 walk is forced, so the
// preserved-baud fast path cannot re-lock into the
// mismatch. The HAL's only job is timing — it fires
// onBreakStorm() and the link decides what to do.
//
// Pin: link is in OK; the onBreakStorm() event clears
// preferredBaud_ and forces the sweep to P1.
// Toggle off (HAL logs but never fires the hook) ->
// red: sweep phase stays where it was.
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include "MockHal.h"
#    include "TestCfg.h"
#    include "NullArqCache.h"
#    include "accessors/LinkTestAccessor.h"

int main() {
    using namespace autolink;
    std::cout << "=== BREAK storm forces P1 walk ===\n";
    AutoLinkConfig cfg;
    testBaseCfg(cfg);
    cfg.allowedBauds[0] = 115200;
    cfg.allowedBauds[1] = 57600;
    cfg.allowedBauds[2] = 38400;
    cfg.allowedBaudsCount = 3;
    cfg.idleTimeoutMs = 0;

    MockHal mHal, sHal;
    NullArqCache cache;
    Link link(mHal, cache, true, cfg);
    link.begin();
    LinkTestAccessor t(link);

    t.forceStateNoLock(State::OK);
    t.setSpdI(2);
    mHal.spd = cfg.allowedBauds[2];

    link.onBreakStorm();
    mHal.pumpClock(2);
    sHal.pumpClock(2);
    auto phase = t.sweepPhase();
    std::cout << "  phase after storm = " << (int)phase << "\n";
    assert(phase == SweepPhase::PHASE1 &&
           "Storm hook must force sweep to PHASE1");
    int spdI = link.getCurrentSpdIndex();
    assert(spdI == cfg.allowedBaudsCount - 1 &&
           "Sweep must park at slowest baud (P1 entry)");

    std::cout << "  PASS (storm forced P1 walk, spdI = slowest)\n";
    std::cout << "=== BreakStormForcesP1Walk: PASS ===\n";
    return 0;
}

#endif
