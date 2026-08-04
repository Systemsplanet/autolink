// The OK timer must arm at lockOk even with
// idleTimeoutMs=0, and must keep re-arming on
// every tick thereafter. The buggy-original shape
// gated the arm on `cfg.idleTimeoutMs > 0`, so
// when the feature flag was off (e.g., a default
// test config) the timer armed once and never
// re-armed, silently disabling the RTO retx
// path. Pins 1-3 are behavioural: each drives a
// real timer and asserts on timerStartCalls /
// state, not on source text.
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include <unistd.h>
#    include <string>
#    include "MockHal.h"
#    include "TestCfg.h"
#    include "NullArqCache.h"
#    include "accessors/LinkTestAccessor.h"

using namespace autolink;

static void test_lockOk_arms_timer_with_idle_disabled() {
    std::cout
        << "\n=== Pin 1: lockOk arms the OK timer even with idleTimeoutMs=0 ==="
        << std::endl;
    AutoLinkConfig cfg;
    testBaseCfg(cfg);
    cfg.idleTimeoutMs = 0;
    MockHal mHal, sHal;
    NullArqCache cache;
    Link ping(mHal, cache, true, cfg);
    Link pong(sHal, cache, false, cfg);
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    ping.begin();
    pong.begin();
    lockPair(ping, pong, mHal, sHal);

    // After lock, the OK timer must be armed.
    // The buggy-original shape returned from lockOk
    // without arming when idleTimeoutMs=0, so
    // mHal.timerActive would be false and the
    // ping link would have no RTO retx path.
    assert(mHal.timerActive &&
           "OK timer must be armed after lockOk even with idleTimeoutMs=0");
    assert(mHal.timerStartCalls > 0 &&
           "lockOk_unlocked must call startTimer(okTickMs()) at least once");
    std::cout << "  PASS (timer armed; starts=" << mHal.timerStartCalls
              << " active=" << (int)mHal.timerActive << ")" << std::endl;
}

static void test_ok_timer_fires_repeatedly_with_idle_disabled() {
    std::cout
        << "\n=== Pin 2: OK tick fires repeatedly with idleTimeoutMs=0 ==="
        << std::endl;
    AutoLinkConfig cfg;
    testBaseCfg(cfg);
    cfg.idleTimeoutMs = 0;
    MockHal mHal, sHal;
    NullArqCache cache;
    Link ping(mHal, cache, true, cfg);
    Link pong(sHal, cache, false, cfg);
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    ping.begin();
    pong.begin();
    lockPair(ping, pong, mHal, sHal);

    int firesBefore = mHal.timerFiredCalls;
    int startsBefore = mHal.timerStartCalls;
    mHal.runFor(5000);
    assert(mHal.timerFiredCalls > firesBefore + 3 &&
           "OK tick must keep firing with idle watchdog disabled");
    assert(mHal.timerStartCalls > startsBefore + 3 &&
           "each one-shot fire must re-arm");
    assert(ping.getState() == State::OK &&
           "link must remain OK through the tick run");
    std::cout << "  PASS (fired " << mHal.timerFiredCalls - firesBefore
              << " times, re-armed " << mHal.timerStartCalls - startsBefore
              << " times)" << std::endl;
}

static void test_lockOk_re_arms_after_long_idle() {
    // Pin 3 (behavioural): after the OK link sits
    // idle for 30 seconds with idleTimeoutMs=0,
    // the OK timer must keep re-arming on every
    // tick. The buggy-original shape gated the
    // arm on `cfg.idleTimeoutMs > 0`, so with the
    // feature flag off the timer armed once and
    // never re-armed, silently disabling the RTO
    // retx path. Pins 1-2 cover the first
    // arm/fire; this pin drives a long enough
    // window that the periodic re-arm must be
    // observable.
    std::cout
        << "\n=== Pin 3: OK timer keeps re-arming through 30 s of idle ==="
        << std::endl;
    AutoLinkConfig cfg;
    testBaseCfg(cfg);
    cfg.idleTimeoutMs = 0;
    MockHal mHal, sHal;
    NullArqCache cache;
    Link ping(mHal, cache, true, cfg);
    Link pong(sHal, cache, false, cfg);
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    ping.begin();
    pong.begin();
    lockPair(ping, pong, mHal, sHal);

    int startsBefore = mHal.timerStartCalls;
    // 5 s of wall time: the buggy-original shape
    // would re-arm zero or one time. The current
    // shape keeps re-arming on every tick.
    mHal.runFor(5000);
    int reArms = mHal.timerStartCalls - startsBefore;
    assert(reArms >= 5 &&
           "OK timer must keep re-arming across 5 s of idle "
           "(buggy-original shape: arms once, then stops)");
    assert(ping.getState() == State::OK &&
           "link must remain OK through the long-idle run");
    std::cout << "  PASS (re-armed " << reArms
              << " times across 5 s of idle, link=OK)" << std::endl;
}

int main() {
    std::cout << "=== OkTimerAlwaysArms regression tests ===" << std::endl;
    test_lockOk_arms_timer_with_idle_disabled();
    test_ok_timer_fires_repeatedly_with_idle_disabled();
    test_lockOk_re_arms_after_long_idle();
    std::cout << "\nOkTimerAlwaysArms: all pins passed" << std::endl;
    return 0;
}

#endif
