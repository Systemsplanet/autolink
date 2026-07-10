// Regression: the OK-state timer is one-shot
// (xTimerCreate pdFALSE), so every OK exit must
// re-arm or the ARQ sweep and health watchdogs die
// silently. Two prior early returns skipped the
// re-arm: linkPaused_ (a dashboard Pause during OK
// killed the tick; Resume never restarted it) and
// idleTimeoutMs <= 0 (disabling the idle watchdog
// also disabled retransmission entirely). MockHal
// used to re-fire a stale one-shot arm, masking
// both on the host.
//
// Pins:
//   1. Paused OK link: the tick still re-arms, and
//      after resume the OK tick keeps running.
//      Toggle-off (return false without startTimer
//      when paused) -> red.
//   2. idleTimeoutMs = 0: the OK tick stays alive
//      (re-arms every pass). Toggle-off (restore
//      the idleTimeoutMs <= 0 early return) -> red.
#include <iostream>
#include <cassert>
#include "MockHal.h"
#include "TestCfg.h"
#include "LinkTestAccessor.h"
#include "al/link/arq/ArqCache.h"
#include "al/AutoLinkConfig.h"

using namespace autolink;

static void mkCfg(AutoLinkConfig &cfg) {
    testBaseCfg(cfg);
    cfg.syncAckTimeoutMs = 500;
}

static void negotiateToOk(Link &ping, Link &pong, MockHal &mHal,
                          MockHal &sHal) {
    lockPair(ping, pong, mHal, sHal);
    assert(ping.getState() == State::OK);
    assert(pong.getState() == State::OK);
}

static void test_paused_ok_tick_keeps_rearming() {
    std::cout << "\n=== Pin 1: paused OK tick re-arms; resume keeps ticking ==="
              << std::endl;
    AutoLinkConfig cfg;
    mkCfg(cfg);
    cfg.idleTimeoutMs = 10000;
    ArqCache pingArq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
    ArqCache pongArq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
    MockHal mHal, sHal;
    Link ping(mHal, pingArq, true, cfg);
    Link pong(sHal, pongArq, false, cfg);
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    ping.begin();
    pong.begin();
    negotiateToOk(ping, pong, mHal, sHal);

    ping.setLinkPaused(true);
    int startsBefore = mHal.timerStartCalls;
    // Several tick periods while paused: each fire
    // must re-arm (one startTimer per fire).
    mHal.runFor(5000);
    assert(mHal.timerStartCalls > startsBefore &&
           "paused OK tick must re-arm the one-shot timer");
    assert(mHal.timerActive && "timer must still be armed while paused");

    ping.setLinkPaused(false);
    int firesBefore = mHal.timerFiredCalls;
    mHal.runFor(5000);
    assert(mHal.timerFiredCalls > firesBefore &&
           "OK tick must keep running after resume");
    assert(ping.getState() == State::OK);
    std::cout << "  PASS" << std::endl;
}

static void test_idle_disabled_keeps_ok_tick_alive() {
    std::cout << "\n=== Pin 2: idleTimeoutMs=0 keeps the OK tick alive ==="
              << std::endl;
    AutoLinkConfig cfg;
    mkCfg(cfg);
    cfg.idleTimeoutMs = 0;
    ArqCache pingArq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
    ArqCache pongArq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
    MockHal mHal, sHal;
    Link ping(mHal, pingArq, true, cfg);
    Link pong(sHal, pongArq, false, cfg);
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    ping.begin();
    pong.begin();
    negotiateToOk(ping, pong, mHal, sHal);

    int startsBefore = mHal.timerStartCalls;
    int firesBefore = mHal.timerFiredCalls;
    mHal.runFor(5000);
    assert(mHal.timerFiredCalls > firesBefore + 3 &&
           "OK tick must keep firing with the idle watchdog disabled");
    assert(mHal.timerStartCalls > startsBefore + 3 &&
           "each one-shot fire must re-arm");
    assert(ping.getState() == State::OK &&
           "idleTimeoutMs=0 must not drop a healthy link");
    std::cout << "  PASS" << std::endl;
}

int main() {
    test_paused_ok_tick_keeps_rearming();
    test_idle_disabled_keeps_ok_tick_alive();
    std::cout << "\nLinkTimerRearmTest: all pins passed" << std::endl;
    return 0;
}
