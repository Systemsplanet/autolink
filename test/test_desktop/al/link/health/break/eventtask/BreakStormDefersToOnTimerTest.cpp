// The HAL's BREAK-storm hook fires from the UART
// event task. Two contracts: (a) the work runs at
// the top of onTimer() (not inline in the hook), so
// the UART event task isn't blocked in setSpd /
// startTimer; (b) the storm escalation routes through
// reset_unlocked(preservePreferredBaud=false,
// ResetReason::PeerBaudMismatch) so the ARQ cache,
// seq space and counters clear atomically, and
// preferredBaud_ is wiped (the proven baud is no
// longer trustworthy under a storm).
//
// Pin: drive a link to OK, set preferredBaud_ to a
// non-default value, fire onBreakStorm, observe
// preferredBaud_ unchanged (deferred), call onTimer
// once, observe preferredBaud_ == NO_PREFERRED_BAUD
// and state == SWP. Toggle off (revert the
// deferral — run reset inline) -> red: the storm
// path now sets preferredBaud_ from the UART event
// task, which is the deadlock the P3 branch comment
// in LinkTimersSwp.cpp warns about.
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include "MockHal.h"
#    include "TestCfg.h"
#    include "NullArqCache.h"
#    include "accessors/LinkTestAccessor.h"

int main() {
    using namespace autolink;
    std::cout << "=== BREAK storm defers to onTimer ===\n";
    AutoLinkConfig cfg;
    testBaseCfg(cfg);
    cfg.allowedBauds[0] = 115200;
    cfg.allowedBauds[1] = 9600;
    cfg.allowedBaudsCount = 2;
    cfg.idleTimeoutMs = 0;

    NullArqCache cache;
    MockHal mHal;
    Link link(mHal, cache, true, cfg);
    link.begin();
    LinkTestAccessor t(link);
    t.forceStateNoLock(State::OK);
    t.setSpdI(0);
    t.setPreferredBaudForTest(1);
    mHal.spd = 115200;

    // Fire the storm hook: nothing must change yet
    // (the work is deferred).
    link.onBreakStorm();
    assert(t.preferredBaudForTest() == 1 &&
           "preferredBaud_ must survive the inline hook "
           "(work is deferred to onTimer)");

    // The deferred flag is set; state and preferredBaud_
    // are unchanged.
    assert(t.breakStormPendingForTest() == true);

    // Consume the flag in onTimer.
    link.onTimer();
    assert(t.preferredBaudForTest() == 0xFF &&
           "preferredBaud_ must be cleared after onTimer "
           "consumes the storm escalation");
    assert(link.getState() == State::SWP &&
           "storm escalation must drive state to SWP via "
           "reset_unlocked");
    assert(t.breakStormPendingForTest() == false &&
           "deferred flag must clear after consumption");

    std::cout << "  PASS (storm deferred, preferredBaud_ "
                 "cleared via reset_unlocked)\n";
    std::cout << "=== BreakStormDefersToOnTimer: PASS ===\n";
    return 0;
}

#endif
