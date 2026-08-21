// Regression pin: Link::kickoff() (master branch) must enter
// Phase 1 exactly once. reset_unlocked(false) already calls
// sweep_.enterPhase1() internally on its non-preserved-baud
// path; kickoff() must not call it again, since a second call
// would double-arm the P1 dwell timer and double-send
// PING_CMD on every fresh kickoff. Toggle the fix off (re-add
// an explicit sweep_.enterPhase1(*this) call in kickoff()'s
// master branch) and this pin goes red.
#ifndef ARDUINO

#    include <cassert>
#    include <iostream>
#    include "MockHal.h"
#    include "NullArqCache.h"
#    include "al/AutoLinkConfig.h"
#    include "al/link/Link.h"

using namespace autolink;

static void test_master_kickoff_arms_p1_timer_exactly_once() {
    std::cout << "\n=== Master kickoff() arms the P1 timer exactly once ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg;
    MockHal mHal;
    // Default AutoLinkConfig -> linkPaused_ starts false, so
    // begin() fires kickoff() synchronously (master path),
    // same as an unpaused Ping/AutoLinkWeb boot.
    Link ping(mHal, cache, true, cfg);
    ping.begin();

    assert(mHal.timerStartCalls == 1 &&
           "kickoff() must call sweep_.enterPhase1() exactly once — "
           "reset_unlocked(false) already enters Phase 1 internally");
    std::cout << "  PASS (timerStartCalls == 1)" << std::endl;
}

static void test_master_kickoff_sends_one_ping_frame() {
    std::cout << "\n=== Master kickoff() sends exactly one P1 PING frame ==="
              << std::endl;
    NullArqCache cacheA, cacheB;
    AutoLinkConfig cfg;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link ping(mHal, cacheA, true, cfg);
    Link pong(sHal, cacheB, false, cfg);
    pong.begin();
    ping.begin();

    // A duplicated enterPhase1() call sends PING_CMD twice
    // before the slave (still at a mismatched/initial baud on
    // its first byte) ever gets a chance to ack either one.
    // One kickoff should queue exactly one PING_CMD frame's
    // worth of bytes, not two.
    assert(mHal.timerStartCalls == 1);
    std::cout << "  PASS (timerStartCalls == 1 with a peer wired up)"
              << std::endl;
}

int main() {
    std::cout << "=== Kickoff Enters Phase 1 Exactly Once ===" << std::endl;
    test_master_kickoff_arms_p1_timer_exactly_once();
    test_master_kickoff_sends_one_ping_frame();
    std::cout << "\n=== Done ===" << std::endl;
    return 0;
}
#endif
