// LinkBreakConsts::BREAK_CONFIRM_MS must be baud-derived, not fixed.
// At 9600 a single chunk's flight time is ~260 ms; a
// fixed 150 ms window makes the two-frame-clear path
// unreachable because two qualifying frames cannot
// fit inside the window. Every BREAK at a downshifted
// baud confirms into a reset.
//
// Pin: 9600 locked baud. After the first BREAK arms
// the confirm window, the arm must exceed the time it
// takes to deliver two chunks of (MAX_CHUNK+MSG_HDR)
// bytes at 9600. Toggle off (revert to fixed 150 ms) ->
// red: arm is too short to fit two chunks.
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include "MockHal.h"
#    include "TestCfg.h"
#    include "NullArqCache.h"
#    include "accessors/LinkTestAccessor.h"
#    include "al/link/LinkWire.h"
#    include "al/link/sweep/LinkDecision.h"

int main() {
    using namespace autolink;
    std::cout << "=== LinkBreakConsts::BREAK_CONFIRM_MS is baud-derived ===\n";
    AutoLinkConfig cfg;
    testBaseCfg(cfg);
    cfg.allowedBauds[0] = 9600;
    cfg.allowedBaudsCount = 1;
    cfg.idleTimeoutMs = 0;

    MockHal mHal, sHal;
    NullArqCache cache;
    Link link(mHal, cache, true, cfg);
    link.begin();
    LinkTestAccessor t(link);

    t.forceStateNoLock(State::OK);
    t.setSpdI(0);
    mHal.spd = 9600;

    link.onBreak();
    int armedMs = mHal.lastTimerMs;
    uint32_t twoChunkMs = (uint32_t)(MAX_CHUNK + MSG_HDR) * 20u * 1000u / 9600u;
    int expectedMin = (int)twoChunkMs;
    std::cout << "  expected confirm >= " << expectedMin
              << " ms at 9600, actual = " << armedMs << " ms\n";
    assert(
        armedMs >= expectedMin &&
        "LinkBreakConsts::BREAK_CONFIRM_MS must cover two chunks at the locked baud");
    assert(armedMs > (int)LinkBreakConsts::BREAK_CONFIRM_MS &&
           "At 9600, confirm must exceed the fixed 150 ms floor");

    std::cout << "  PASS (confirm window covers two-chunk flight at 9600)\n";
    std::cout << "=== BreakBaudAwareConfirm: PASS ===\n";
    return 0;
}

#endif
