// Field failure this pins: after a preserving reset the master
// camped at preferredBaud_ for only 2 attempts x ~250 ms t3 =
// ~750 ms. The peer's log stopped mid-stream and it did not answer
// for ~12 s, so the camp expired almost immediately, the master
// walked P1 all the way down to the slowest baud, and the pair
// relocked there instead of at the proven fast baud.
//
// The camp is now bounded by a wall-clock budget
// (resweepPrefDeadlineMs_, 3-5 s) rather than a count of t3-sized
// attempts, so camp length no longer scales with t3.
//
// Pin 1: the budget is seconds, not sub-second, and is independent
// of t3. Pin 2: the camp survives well past the old ~750 ms and
// keeps re-PINGing at the preserved baud. Pin 3: it is still
// bounded — a peer that never returns eventually reaches the P1
// walk rather than camping forever.
#ifndef AUTOLINK_HOST_TEST
#    error "Build with -DAUTOLINK_HOST_TEST (see Makefile)"
#endif

#include <iostream>
#include <cassert>
#include "MockHal.h"
#include "NullArqCache.h"
#include "LinkTestAccessor.h"
#include "al/link/Link.h"
#include "al/link/sweep/LinkSweep.h"
#include "al/AutoLinkConfig.h"

using namespace autolink;

namespace {

AutoLinkConfig makeCfg() {
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    cfg.allowedBaudsCount = 3;
    cfg.allowedBauds[0] = 512000;
    cfg.allowedBauds[1] = 115200;
    cfg.allowedBauds[2] = 9600;
    cfg.syncAckTimeoutMs = 200;
    cfg.idleTimeoutMs = 20000;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    return cfg;
}

// Drives onTimer until the sweep leaves PHASE3 or the wall clock
// budget runs out. Returns ms elapsed inside PHASE3.
uint32_t campDurationMs(Link &link, LinkTestAccessor &acc, MockHal &hal,
                        uint32_t capMs, int *peakAttempts = nullptr) {
    (void)link;
    uint32_t t0 = hal.now;
    int peak = 0;
    for (int i = 0; i < 4000; i++) {
        if (acc.sweepPhase() != SweepPhase::PHASE3)
            break;
        if ((uint32_t)(hal.now - t0) > capMs)
            break;
        // Sample inside the camp: the counter is zeroed again on
        // the way out to the P1 walk, so reading it afterwards
        // always shows 0.
        if (acc.resweepPrefAttemptsForTest() > peak)
            peak = acc.resweepPrefAttemptsForTest();
        hal.pumpClock(25);
    }
    if (peakAttempts)
        *peakAttempts = peak;
    return (uint32_t)(hal.now - t0);
}

void test_camp_budget_is_seconds_and_independent_of_t3() {
    std::cout << "\n=== Pin 1: camp budget is seconds and does not scale "
                 "with t3 ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg = makeCfg();
    MockHal hal;
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor acc(link);

    uint32_t budget = acc.resweepPrefBudgetMs();
    std::cout << "  budget=" << budget
              << " ms (syncAckTimeoutMs=" << cfg.syncAckTimeoutMs << ")"
              << std::endl;
    assert(budget >= 3000u &&
           "a sub-second camp is what abandoned a peer that needed "
           "seconds to come back");
    assert(budget <= 5000u && "the camp must stay bounded");
    std::cout << "  PASS" << std::endl;
}

void test_camp_outlasts_the_old_attempt_window() {
    std::cout << "\n=== Pin 2: camp keeps re-PINGing well past the old "
                 "~750 ms attempt window ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg = makeCfg();
    MockHal hal;
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor acc(link);
    acc.forceState(State::OK);

    // Lock at the fast baud, then take a preserving reset with a
    // silent peer — the field shape.
    hal.lock();
    acc.setPreferredBaudForTest(0);
    acc.setWasEverOkForTest(true);
    hal.unlock();
    acc.resetLink(true, /*preserve=*/true, ResetReason::HealthWatchdog);

    assert(acc.sweepPhase() == SweepPhase::PHASE3 &&
           "a preserving reset with a valid preferredBaud_ must camp "
           "at P3, not walk P1");
    int campBaudIdx = link.getCurrentSpdIndex();
    assert(campBaudIdx == 0 && "the camp must sit at the proven baud");

    hal.clearTx();
    int pings = 0;
    uint32_t dur = campDurationMs(link, acc, hal, 10000, &pings);

    std::cout << "  camped " << dur << " ms, re-PINGs=" << pings
              << ", phase now="
              << (acc.sweepPhase() == SweepPhase::PHASE3 ? "PHASE3" : "left")
              << std::endl;
    assert(dur > 1500u &&
           "the camp must outlast the old 2-attempt ~750 ms window — "
           "expiring that fast is what dropped the pair to the "
           "slowest baud while the peer was still coming back");
    assert(pings > 2 &&
           "the camp must keep re-PINGing at the preserved baud across "
           "many attempts, not stop after the old fixed two");
    std::cout << "  PASS" << std::endl;
}

void test_camp_is_still_bounded() {
    std::cout << "\n=== Pin 3: a peer that never returns still reaches the "
                 "P1 walk ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg = makeCfg();
    MockHal hal;
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor acc(link);
    acc.forceState(State::OK);

    hal.lock();
    acc.setPreferredBaudForTest(0);
    acc.setWasEverOkForTest(true);
    hal.unlock();
    acc.resetLink(true, /*preserve=*/true, ResetReason::HealthWatchdog);
    assert(acc.sweepPhase() == SweepPhase::PHASE3);

    uint32_t dur = campDurationMs(link, acc, hal, 30000);
    std::cout << "  left PHASE3 after " << dur << " ms, phase="
              << (acc.sweepPhase() == SweepPhase::PHASE1 ? "PHASE1" : "other")
              << " spdI=" << link.getCurrentSpdIndex() << std::endl;
    assert(acc.sweepPhase() != SweepPhase::PHASE3 &&
           "an unbounded camp would strand the link at a baud the peer "
           "is not answering on");
    assert(dur < 12000u && "the camp must expire on its own budget");
    std::cout << "  PASS" << std::endl;
}

} // namespace

int main() {
    std::cout << "=== P3 preferred-baud camp budget ===" << std::endl;
    test_camp_budget_is_seconds_and_independent_of_t3();
    test_camp_outlasts_the_old_attempt_window();
    test_camp_is_still_bounded();
    std::cout << "\nAll P3CampBudget pins passed." << std::endl;
    return 0;
}
