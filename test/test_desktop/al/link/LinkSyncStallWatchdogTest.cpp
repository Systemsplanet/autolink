// Regression: SYNC mode must self-heal when the peer
// stops ACKing. A lost mid-message ACK desyncs SYNC's
// length-prefixed stream; the peer then waits forever
// for a message tail that never arrives and stops
// ACKing, so every subsequent sendMsg times out with
// pendingCount()==0 and frameErrs==0. Before this fix
// the entire idle/backpressure watchdog block in
// onTimerOk_unlocked was gated behind
// `if (mode != SYNC)`, so SYNC had no drop/resweep
// path at all: the link wedged permanently with disc=0
// (observed on the FireBeetle pair — tx falls to 0 and
// stays there). The fix stamps the tx-reject streak on
// a SYNC send failure and runs the tx-backpressure
// stall watchdog for SYNC too, so a persistently
// un-ACKed sender drops + BREAKs + resweeps.
//
// Pins:
//   1. A live tx-reject streak older than idleTimeoutMs
//      drops a SYNC link (state -> SWP, discCount==1)
//      on the OK-timer tick. Toggle-off (watchdog
//      ASYNC-only again) -> the SYNC link stays OK ->
//      assertion fails (red).
//   2. A single stale reject (not still-live) does NOT
//      drop a quiet SYNC link (still-live gate holds).
#include <iostream>
#include <cassert>
#include <cstdio>
#include "MockHal.h"
#include "LinkTestAccessor.h"
#include "al/link/arq/ArqCache.h"
#include "al/AutoLinkConfig.h"
#include "al/util/Log.h"

using namespace autolink;

static const int kBauds[] = { 115200, 57600, 38400, 19200, 9600 };
static const int kNumBauds = (int)(sizeof(kBauds) / sizeof(kBauds[0]));

static void mkCfg(AutoLinkConfig &cfg) {
    for (int i = 0; i < kNumBauds; i++)
        cfg.allowedBauds[i] = (uint32_t)kBauds[i];
    cfg.allowedBaudsCount = kNumBauds;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 10000;
    cfg.streamBufferSize = 131072;
    cfg.maxMsg = 2048;
    cfg.syncAckTimeoutMs = 500;
    cfg.mode = AutoLinkConfig::Mode::SYNC;
}

static void negotiateToOk(Link &ping, Link &pong, MockHal &mHal,
                          MockHal &sHal) {
    for (int i = 0; i < 100 &&
         (ping.getState() != State::OK || pong.getState() != State::OK);
         i++) {
        mHal.pumpClock(50);
        sHal.pumpClock(50);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
    }
    assert(ping.getState() == State::OK);
    assert(pong.getState() == State::OK);
    sHal.txBuf.clear();
    mHal.txBuf.clear();
    sHal.clearAppBuf();
    mHal.clearAppBuf();
}

static void test_sync_stall_drops_and_resweeps() {
    std::cout << "\n=== Pin 1: SYNC tx-reject stall drops + resweeps ==="
              << std::endl;
    AutoLinkConfig cfg;
    mkCfg(cfg);
    ArqCache pingArq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
    ArqCache pongArq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
    MockHal mHal, sHal;
    Link ping(mHal, pingArq, true, cfg);
    Link pong(sHal, pongArq, false, cfg);
    ping.setMode(AutoLinkConfig::Mode::SYNC);
    pong.setMode(AutoLinkConfig::Mode::SYNC);
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    ping.begin();
    pong.begin();
    negotiateToOk(ping, pong, mHal, sHal);
    assert(ping.getState() == State::OK);

    // Cut the wire so nothing the watchdog does gets
    // a spurious ACK back that clears the streak.
    mHal.peer = nullptr;

    LinkTestAccessor pa(ping);
    Stats before;
    ping.getStats(before);

    // Simulate the first failed SYNC send: stamp the
    // reject streak (txRejFirst = txRejLast = now).
    pa.noteTxReject();

    // Age past idleTimeoutMs while keeping the streak
    // live: re-stamp near the end so txRejLast stays
    // within idleTimeoutMs of "now" when the timer
    // fires. Pump in sub-idleTimeout steps so the
    // OK-timer ticks along the way don't drop early
    // (streak not yet old enough).
    for (int k = 0; k < 12; k++) {
        mHal.pumpClock(1000);
        pa.noteTxReject(); // sender still failing every ~1s
        if (ping.getState() != State::OK)
            break;
    }
    // One more tick after the streak has aged > 10s.
    mHal.pumpClock(1000);

    Stats after;
    ping.getStats(after);
    std::cout << "  state=" << (int)ping.getState() << " disc "
              << before.discCount << " -> " << after.discCount << std::endl;
    assert(ping.getState() != State::OK &&
           "SYNC link must leave OK (drop + resweep) on a sustained "
           "tx-reject stall");
    assert(after.discCount == before.discCount + 1 &&
           "the stall drop must count exactly one disconnect");
    std::cout << "  PASS" << std::endl;
}

static void test_sync_stale_reject_does_not_drop() {
    std::cout << "\n=== Pin 2: single stale SYNC reject does not drop ==="
              << std::endl;
    AutoLinkConfig cfg;
    mkCfg(cfg);
    ArqCache pingArq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
    ArqCache pongArq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
    MockHal mHal, sHal;
    Link ping(mHal, pingArq, true, cfg);
    Link pong(sHal, pongArq, false, cfg);
    ping.setMode(AutoLinkConfig::Mode::SYNC);
    pong.setMode(AutoLinkConfig::Mode::SYNC);
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    ping.begin();
    pong.begin();
    negotiateToOk(ping, pong, mHal, sHal);
    mHal.peer = nullptr;

    LinkTestAccessor pa(ping);
    // One transient reject, then a long quiet stretch
    // with no further failures: txRejLast goes stale
    // (older than idleTimeoutMs), so the still-live
    // gate must keep the link up.
    pa.noteTxReject();
    for (int k = 0; k < 15; k++)
        mHal.pumpClock(1000);

    assert(ping.getState() == State::OK &&
           "a single stale reject must not drop a quiet SYNC link");
    std::cout << "  PASS" << std::endl;
}

int main() {
    Log::log().setLevel(Log::NONE);
    test_sync_stall_drops_and_resweeps();
    test_sync_stale_reject_does_not_drop();
    std::cout << "\nLinkSyncStallWatchdogTest: all pins passed" << std::endl;
    return 0;
}
