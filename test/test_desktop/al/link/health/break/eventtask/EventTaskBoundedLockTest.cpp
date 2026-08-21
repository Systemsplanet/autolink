// onBreak() and onBreakStorm() both run on the UART event task,
// which is also the RX drainer. sendMsg holds the same link lock
// across a blocking hw.tx() write, so the old unconditional
// hw.lock() there could park the event task — and with it the RX
// path both hooks depend on to ever get scheduled again — behind a
// slow sender. IHal::tryLock(timeoutMs) gives event-task callers a
// bounded wait instead: on success, behaves exactly like lock(); on
// timeout, the caller drops the notification and returns rather
// than block.
//
// Pin 1: both hooks call hw.tryLock(EVENT_TASK_LOCK_TIMEOUT_MS), not
// hw.lock() directly, and behave identically to the old
// unconditional-lock behavior when the lock is uncontended.
// Pin 2: when tryLock times out, both hooks drop the notification —
// no state mutated, no crash, no unmatched unlock().
// Toggle off (revert to hw.lock()) -> red: MockHal's tryLockCalls
// stays 0 (lock() was called instead) and Pin 2's forced-timeout
// case can't be exercised at all (lock() cannot time out).
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
    std::cout << "=== onBreak()/onBreakStorm() use a bounded tryLock ===\n";
    AutoLinkConfig cfg;
    testBaseCfg(cfg);
    cfg.allowedBauds[0] = 115200;
    cfg.allowedBaudsCount = 1;
    cfg.idleTimeoutMs = 0;

    // Pin 1: uncontended tryLock behaves like the old lock().
    {
        NullArqCache cache;
        MockHal mHal;
        Link link(mHal, cache, /*isMaster=*/true, cfg);
        link.begin();
        LinkTestAccessor t(link);
        t.forceStateNoLock(State::OK);
        t.setSpdI(0);
        mHal.spd = 115200;

        mHal.tryLockCalls = 0;
        link.onBreak();
        assert(mHal.tryLockCalls == 1 &&
               "onBreak() must call tryLock(), not lock() directly");
        assert(mHal.lastTryLockTimeoutMs ==
                   LinkBreakConsts::EVENT_TASK_LOCK_TIMEOUT_MS &&
               "onBreak() must pass the shared event-task lock timeout");
        assert(link.getState() == State::OK);
        assert(t.breakSuspectMsForTest() != 0 &&
               "a successful tryLock must still arm suspicion as before");

        mHal.tryLockCalls = 0;
        link.onBreakStorm();
        assert(mHal.tryLockCalls == 1 &&
               "onBreakStorm() must call tryLock(), not lock() directly");
        assert(mHal.lastTryLockTimeoutMs ==
               LinkBreakConsts::EVENT_TASK_LOCK_TIMEOUT_MS);
        assert(t.breakStormPendingForTest() == true &&
               "a successful tryLock must still set breakStormPending_ "
               "as before");
        std::cout << "  Pin 1 PASS (both hooks use tryLock, uncontended "
                     "behavior unchanged)\n";
    }

    // Pin 2: a timed-out tryLock drops the notification safely.
    {
        NullArqCache cache;
        MockHal mHal;
        Link link(mHal, cache, /*isMaster=*/true, cfg);
        link.begin();
        LinkTestAccessor t(link);
        t.forceStateNoLock(State::OK);
        t.setSpdI(0);
        mHal.spd = 115200;
        mHal.forceTryLockFail = true;

        link.onBreak();
        assert(t.breakSuspectMsForTest() == 0 &&
               "a timed-out tryLock must drop the BREAK notification, "
               "not mutate suspicion state");
        assert(link.getState() == State::OK);

        link.onBreakStorm();
        assert(t.breakStormPendingForTest() == false &&
               "a timed-out tryLock must drop the storm notification too");

        std::cout << "  Pin 2 PASS (timed-out tryLock drops the "
                     "notification, no state mutated)\n";
    }

    std::cout << "=== EventTaskBoundedLock: PASS ===\n";
    return 0;
}

#endif
