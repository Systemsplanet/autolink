// The baud-aware stuck threshold is recomputed per
// tick from the actual pendingCount, not cached at
// lockOk_ where pendingCount is always 0. A cached
// value pins the threshold to the syncAckTimeoutMs
// floor and the per-baud drain formula never runs.
//
// Pin: configure a link, fill the GBN window to 32
// chunks at 9600, advance the OK-timer tick. The
// threshold must exceed 8.3 s (the 32-chunk drain
// time at 9600). Toggle off (revert the per-tick
// recompute and only cache at lockOk_) -> red:
// threshold equals syncAckTimeoutMs (~500 ms).
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include "MockHal.h"
#    include "TestCfg.h"
#    include "NullArqCache.h"
#    include "accessors/LinkTestAccessor.h"
#    include "al/link/LinkWire.h"
#    include "al/link/sweep/LinkDecision.h"

using namespace autolink;

static void bringToOk(Link &a, Link &b, MockHal &mHal, MockHal &sHal) {
    a.begin();
    b.begin();
    for (int i = 0; i < 400; i++) {
        mHal.pumpClock(50);
        sHal.pumpClock(50);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
        if (a.getState() == State::OK && b.getState() == State::OK)
            return;
    }
    assert(false && "failed to bring two nodes to OK");
}

int main() {
    std::cout << "=== baud-aware stuck threshold per tick ===\n";
    AutoLinkConfig cfg;
    testBaseCfg(cfg);
    // 32-chunk 250-byte window at 9600 ≈ 8.3 s drain;
    // the test seeds 32 pending and expects the per-tick
    // recompute to drive the threshold well above the
    // 500 ms floor (otherwise honest-drop trips at 1/8
    // of the drain time).
    cfg.allowedBauds[0] = 9600;
    cfg.allowedBaudsCount = 1;
    cfg.idleTimeoutMs = 0;
    cfg.syncAckTimeoutMs = 500;

    NullArqCache cacheA, cacheB;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link ping(mHal, cacheA, true, cfg);
    Link pong(sHal, cacheB, false, cfg);
    bringToOk(ping, pong, mHal, sHal);
    assert(ping.getState() == State::OK);
    LinkTestAccessor t(ping);

    for (int i = 0; i < 32; i++) {
        t.markAckedPending((uint8_t)i);
    }
    assert(t.arq().pendingCount() == 32);

    t.setSpdI(0);
    mHal.spd = 9600;

    int rt = (int)roundTripMs(9600);
    uint32_t expected =
        (uint32_t)(32 * (MAX_CHUNK + MSG_HDR)) * 10u / 9600u + (uint32_t)rt;
    if (expected < (uint32_t)cfg.syncAckTimeoutMs)
        expected = (uint32_t)cfg.syncAckTimeoutMs;

    for (int i = 0; i < 25; i++) {
        mHal.pumpClock(2);
        sHal.pumpClock(2);
    }
    ping.onTimer();
    uint32_t actual = t.gbnBaseStuckThresholdMsForTest();
    std::cout << "  expected >= " << expected << " ms, actual = " << actual
              << " ms\n";
    assert(actual >= expected &&
           "Threshold must reflect the actual pendingCount at 9600");

    std::cout << "  PASS (threshold recomputed per tick from pending)\n";
    std::cout << "=== BaudAwareStuckThresholdPerTick: PASS ===\n";
    return 0;
}

#endif
