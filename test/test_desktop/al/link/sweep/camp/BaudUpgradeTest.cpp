// Field failure this pins: preferredBaud_ is overwritten by every
// lockOk_unlocked call, including a p2-fallback lock at the
// slowest baud. From then on every preserving reset camps at the
// slow baud, and there was no path back to the fast baud that was
// working minutes earlier — a slow lock was permanent until some
// other event dropped the link entirely.
//
// Fix: bestProvenBaud_ only ever moves toward faster (lower index)
// and survives a slow fallback lock. A lock slower than
// bestProvenBaud_ arms a deadline (BAUD_UPGRADE_DELAY_MS after the
// lock, so a fresh lock gets a chance to settle first); the OK-tick
// handler spends one preserving reset per attempt retrying the
// proven baud, capped at BAUD_UPGRADE_MAX_ATTEMPTS.
//
// Pin 1: a slow lock does not clobber a faster bestProvenBaud_, and
// arms the upgrade deadline. Pin 2: a lock at or above the proven
// baud does not arm anything and resets the attempt counter. Pin 3:
// once the deadline passes, the OK tick fires exactly one
// preserving BaudUpgrade reset aimed at bestProvenBaud_. Pin 4: the
// attempt cap is enforced — repeated slow locks stop arming once
// BAUD_UPGRADE_MAX_ATTEMPTS is spent. Pin 5: a from-scratch
// renegotiation (preferredBaud_ cleared) also clears the upgrade
// memory, so it does not chase a baud the pair no longer proved.
#ifndef AUTOLINK_HOST_TEST
#    error "Build with -DAUTOLINK_HOST_TEST (see Makefile)"
#endif

#include <iostream>
#include <cassert>
#include "MockHal.h"
#include "NullArqCache.h"
#include "LinkTestAccessor.h"
#include "al/link/Link.h"
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

void test_slow_lock_preserves_best_and_arms_upgrade() {
    std::cout << "\n=== Pin 1: a slow lock preserves bestProvenBaud_ and "
                 "arms the upgrade deadline ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg = makeCfg();
    MockHal hal;
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor acc(link);

    acc.lockOkForTest(0); // lock at the fast baud first
    assert(acc.bestProvenBaudForTest() == 0);
    assert(acc.baudUpgradeAtMsForTest() == 0 &&
           "locking at the best baud must not arm an upgrade");

    hal.pumpClock(500);
    acc.forceState(State::SWP); // link dropped, renegotiating
    acc.lockOkForTest(2);       // fall back to the slowest baud

    std::cout << "  bestProven=" << (int)acc.bestProvenBaudForTest()
              << " upgradeAt=" << acc.baudUpgradeAtMsForTest()
              << " now=" << hal.now << std::endl;
    assert(acc.bestProvenBaudForTest() == 0 &&
           "the p2-fallback lock must not erase the fast baud's memory "
           "— that erasure is exactly what made the slow lock "
           "permanent in the field");
    assert(acc.baudUpgradeAtMsForTest() > hal.now &&
           "a lock slower than the proven baud must arm a future "
           "upgrade deadline");
    std::cout << "  PASS" << std::endl;
}

void test_lock_at_or_above_best_disarms() {
    std::cout << "\n=== Pin 2: locking at or above bestProvenBaud_ disarms "
                 "and resets attempts ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg = makeCfg();
    MockHal hal;
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor acc(link);

    acc.lockOkForTest(0);
    hal.pumpClock(100);
    acc.forceState(State::SWP);
    acc.lockOkForTest(2); // arm an upgrade
    assert(acc.baudUpgradeAtMsForTest() != 0);

    hal.pumpClock(100);
    acc.forceState(State::SWP);
    acc.lockOkForTest(0); // recovered at the proven baud on its own
    std::cout << "  after recovery: upgradeAt=" << acc.baudUpgradeAtMsForTest()
              << " attempts=" << acc.baudUpgradeAttemptsForTest() << std::endl;
    assert(acc.baudUpgradeAtMsForTest() == 0 &&
           "recovering at the proven baud must disarm the pending "
           "upgrade attempt");
    assert(acc.baudUpgradeAttemptsForTest() == 0);
    std::cout << "  PASS" << std::endl;
}

void test_upgrade_fires_once_deadline_passes() {
    std::cout << "\n=== Pin 3: the OK tick fires exactly one preserving "
                 "BaudUpgrade reset once the deadline passes ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg = makeCfg();
    MockHal hal;
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor acc(link);

    acc.lockOkForTest(0);
    hal.pumpClock(100);
    acc.forceState(State::SWP);
    acc.lockOkForTest(2);
    uint32_t deadline = acc.baudUpgradeAtMsForTest();
    assert(deadline != 0);

    // Before the deadline: no upgrade, no change.
    hal.now = deadline - 1;
    acc.onTimerOkForTest();
    assert(link.getState() != State::SWP &&
           "the upgrade must not fire before its deadline");
    assert(acc.baudUpgradeAttemptsForTest() == 0);

    // At/after the deadline: exactly one preserving reset, aimed
    // at the proven baud.
    hal.now = deadline;
    bool brk = acc.onTimerOkForTest();
    std::cout << "  brk=" << (brk ? "true" : "false")
              << " state=" << (link.getState() == State::SWP ? "SWP" : "OK")
              << " preferredBaud=" << (int)acc.preferredBaudForTest()
              << " attempts=" << acc.baudUpgradeAttemptsForTest() << std::endl;
    assert(link.getState() == State::SWP &&
           "the upgrade attempt must reset the link (preserving, so "
           "it re-enters the P3 camp rather than a full P1 walk)");
    assert(acc.preferredBaudForTest() == 0 &&
           "the reset must aim preferredBaud_ back at the proven baud");
    assert(acc.baudUpgradeAttemptsForTest() == 1);
    std::cout << "  PASS" << std::endl;
}

void test_attempt_cap_enforced() {
    std::cout << "\n=== Pin 4: upgrade attempts are capped at "
                 "BAUD_UPGRADE_MAX_ATTEMPTS ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg = makeCfg();
    MockHal hal;
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor acc(link);

    acc.lockOkForTest(0);
    int fired = 0;
    for (int i = 0;
         i < LinkTestAccessor::BAUD_UPGRADE_MAX_ATTEMPTS_FOR_TEST + 3; i++) {
        hal.pumpClock(100);
        acc.forceState(State::SWP); // link dropped, renegotiating slow
        acc.lockOkForTest(2);
        uint32_t deadline = acc.baudUpgradeAtMsForTest();
        if (deadline == 0)
            break;
        hal.now = deadline;
        acc.onTimerOkForTest();
        if (acc.getStateForTest() == State::SWP) {
            fired++;
            acc.forceState(State::OK);
        }
    }
    std::cout << "  fired=" << fired << " (cap="
              << LinkTestAccessor::BAUD_UPGRADE_MAX_ATTEMPTS_FOR_TEST << ")"
              << std::endl;
    assert(fired <= LinkTestAccessor::BAUD_UPGRADE_MAX_ATTEMPTS_FOR_TEST &&
           "a persistently degraded line must settle at the slow baud "
           "instead of oscillating forever");
    assert(fired >= 1);
    std::cout << "  PASS" << std::endl;
}

void test_full_renegotiation_clears_upgrade_memory() {
    std::cout << "\n=== Pin 5: a from-scratch renegotiation clears "
                 "bestProvenBaud_ and the upgrade arm ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg = makeCfg();
    MockHal hal;
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor acc(link);

    acc.lockOkForTest(0);
    hal.pumpClock(100);
    acc.forceState(State::SWP);
    acc.lockOkForTest(2);
    assert(acc.bestProvenBaudForTest() == 0);
    assert(acc.baudUpgradeAtMsForTest() != 0);

    // A non-preserving reset is a from-scratch renegotiation.
    acc.resetLink(false, /*preserve=*/false, ResetReason::UserDropLink);

    std::cout << "  bestProven=" << (int)acc.bestProvenBaudForTest()
              << " upgradeAt=" << acc.baudUpgradeAtMsForTest() << std::endl;
    assert(acc.bestProvenBaudForTest() ==
               LinkTestAccessor::NO_PREFERRED_BAUD_FOR_TEST &&
           "a from-scratch renegotiation must not keep chasing a baud "
           "the pair no longer has any evidence for");
    assert(acc.baudUpgradeAtMsForTest() == 0);
    std::cout << "  PASS" << std::endl;
}

} // namespace

int main() {
    std::cout << "=== Bounded baud upgrade after a slow fallback lock ==="
              << std::endl;
    test_slow_lock_preserves_best_and_arms_upgrade();
    test_lock_at_or_above_best_disarms();
    test_upgrade_fires_once_deadline_passes();
    test_attempt_cap_enforced();
    test_full_renegotiation_clears_upgrade_memory();
    std::cout << "\nAll BaudUpgrade pins passed." << std::endl;
    return 0;
}
