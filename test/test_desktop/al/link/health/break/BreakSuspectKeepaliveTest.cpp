// Pinned regression: the BREAK confirm deadline is a single
// one-shot RTOS timer (xTimerCreate(..., pdFALSE, ...) on the
// real HAL — MockHal's pumpClock models the same one-shot-fire
// semantics). Before this fix, onBreak() always armed that timer
// for the FULL confirm window and onTimerOk_unlocked was never
// invoked again until it fired — so an idle link's only route to
// clearing suspicion (a keepalive PING/PONG round trip) never got
// a chance to run: the link slept straight through to the
// deadline and confirmed a reset even against a fully healthy,
// merely-idle peer. The fix arms an early wake-up when a
// keepalive is due before the deadline, and the intermediate tick
// sends it and re-arms to the true remaining deadline.
//
//   Pin 1 (single-node, wire-shape): an overdue keepalive at
//   BREAK-arm time makes onBreak() arm an early (not full-window)
//   wake-up, and the next tick emits the 5-byte PING CTRL frame
//   while still under suspicion (not yet confirmed/reset).
//   Pin 2 (two-node, end-to-end): an idle link survives a BREAK
//   landing mid-idle — two keepalive round trips inside the
//   confirm window fully clear the suspicion before the deadline,
//   discCount stays unchanged, and the link never drops to SWP.
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include "MockHal.h"
#    include "TestCfg.h"
#    include "NullArqCache.h"
#    include "accessors/LinkTestAccessor.h"
#    include "al/link/timers/LinkBreak.h"

using namespace autolink;

// Pin 1: overdue keepalive at arm time -> early wake-up, keepalive
// sent while still suspect.
static void test_overdue_keepalive_gets_early_wakeup() {
    std::cout << "\n=== Pin 1: BREAK arm with an overdue keepalive wakes up "
                 "early and sends it while still suspect ===\n";
    AutoLinkConfig cfg;
    testBaseCfg(cfg);
    cfg.allowedBauds[0] = 115200;
    cfg.allowedBaudsCount = 1;
    cfg.idleTimeoutMs = 10000; // half = 5000 ms

    MockHal mHal;
    NullArqCache cache;
    Link link(mHal, cache, /*isMaster=*/true, cfg);
    link.begin();
    LinkTestAccessor t(link);

    t.forceState(State::OK);
    t.setSpdI(0);
    mHal.spd = 115200;

    // Advance the clock well past the keepalive threshold, then
    // stamp lastTxMs at the origin so the keepalive is maximally
    // overdue the moment the BREAK arms.
    mHal.now = 20000;
    t.setLastTx(0);
    mHal.clearTx();

    link.onBreak();
    assert(link.getState() == State::OK &&
           "Pin 1 pre: first BREAK arms suspicion, does not reset");
    assert(t.breakSuspectMsForTest() != 0 &&
           "Pin 1 pre: breakSuspectMs_ must be armed");

    uint32_t confirmMs = breakConfirmMs_unlocked(link);
    std::cout << "  armed wake-up=" << mHal.lastTimerMs
              << " ms (full confirm window=" << confirmMs << " ms)\n";
    assert((uint32_t)mHal.lastTimerMs < confirmMs &&
           "Pin 1: an overdue keepalive must arm an EARLY wake-up, not "
           "sleep through the full confirm window — otherwise "
           "onTimerOk_unlocked never runs again until the deadline and "
           "the keepalive that would have cleared suspicion never fires");

    // Pump exactly to the early wake-up. Deliberately NOT calling
    // onTimer() directly — pumpClock's internal one-shot check is
    // what proves the real (non-test-forced) timer path fires.
    mHal.pumpClock((uint32_t)mHal.lastTimerMs);

    assert(mHal.txBuf.size() == (size_t)CTRL_FRAME_SIZE &&
           "Pin 1: the early wake-up must emit exactly one 5-byte CTRL "
           "frame — the overdue keepalive — while still under suspicion");
    assert(mHal.txBuf[0] == 0xAA && mHal.txBuf[1] == 0x55 &&
           "Pin 1: the emitted frame must be a CTRL frame");
    assert(mHal.txBuf[CTRL_FRAME_PAYLOAD_IDX] == PING_CMD &&
           "Pin 1: the emitted frame must be PING_CMD — a keepalive, not "
           "an ACK or any other payload");
    assert(link.getState() == State::OK &&
           "Pin 1: sending the keepalive must not itself resolve the "
           "suspicion — the deadline hasn't been reached and no reply "
           "has arrived yet");
    assert(t.breakSuspectMsForTest() != 0 &&
           "Pin 1: breakSuspectMs_ must still be armed — one outbound "
           "keepalive is not proof of anything by itself");
    std::cout << "  Pin 1 PASS (early wake-up armed, keepalive PING sent "
                 "while still suspect)\n";
}

// Pin 2: idle link survives a BREAK landing mid-idle via two
// keepalive round trips completing inside the confirm window.
static void bringToOk(Link &a, Link &b, MockHal &mHal, MockHal &sHal) {
    a.begin();
    b.begin();
    for (int i = 0; i < 200; i++) {
        mHal.pumpClock(50);
        sHal.pumpClock(50);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
        if (a.getState() == State::OK && b.getState() == State::OK)
            return;
    }
    assert(false && "failed to bring two single-baud nodes to OK");
}

static void agePastSettle(MockHal &mHal, MockHal &sHal) {
    for (uint32_t ts = 0; ts < 120; ts += 20) {
        mHal.pumpClock(20);
        sHal.pumpClock(20);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
    }
}

static void test_idle_link_survives_break_via_early_keepalive() {
    std::cout << "\n=== Pin 2: idle link survives a BREAK mid-idle — two "
                 "keepalive round trips clear it before the confirm "
                 "deadline ===\n";
    NullArqCache cacheA, cacheB;
    AutoLinkConfig cfg;
    cfg.allowedBauds[0] = 115200;
    cfg.allowedBaudsCount = 1;
    cfg.pingSamplesPerBaud = 1;
    // half = 50 ms; two round trips (t=50, t=100) fit inside the
    // 150 ms confirm window at 115200.
    cfg.idleTimeoutMs = 100;
    cfg.syncAckTimeoutMs = 1000;
    cfg.maxRetx = 50;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link a(mHal, cacheA, /*isMaster=*/true, cfg);
    Link b(sHal, cacheB, /*isMaster=*/false, cfg);
    bringToOk(a, b, mHal, sHal);
    agePastSettle(mHal, sHal);

    LinkTestAccessor accA(a), accB(b);
    accA.setLastTx(mHal.now);
    accB.setLastTx(sHal.now);
    assert(accB.arqPendingCountForTest() == 0 &&
           "precondition: B has no pending data (idle)");

    Stats sB0;
    b.getStats(sB0);
    uint32_t discB0 = sB0.discCount;

    sHal.deliver_break_to_self();
    assert(b.getState() == State::OK &&
           "Pin 2 pre: the BREAK defers to the confirm window, not an "
           "immediate reset");
    assert(accB.breakSuspectMsForTest() != 0 &&
           "Pin 2 pre: breakSuspectMs_ must be armed");

    for (uint32_t t = 0; t < 250; t += 5) {
        mHal.pumpClock(5);
        sHal.pumpClock(5);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
    }

    Stats sB1;
    b.getStats(sB1);
    std::cout << "  stateB=" << (int)b.getState()
              << " breakSuspectMs=" << accB.breakSuspectMsForTest()
              << " breaksSuppressed=" << accB.breaksSuppressedForTest()
              << " discB=" << sB1.discCount << " (was " << discB0 << ")\n";
    assert(b.getState() == State::OK &&
           "Pin 2: an idle-but-healthy link must survive a BREAK landing "
           "mid-idle — without the early-wakeup fix this always confirms "
           "into a reset because the one-shot confirm timer never gives "
           "the keepalive a chance to run");
    assert(accB.breakSuspectMsForTest() == 0 &&
           "Pin 2: two keepalive round trips inside the confirm window "
           "must fully clear the suspicion (two-frame-clear)");
    assert(accB.breaksSuppressedForTest() == 1 &&
           "Pin 2: the BREAK must be counted as suppressed, not silently "
           "ignored nor escalated to a reset");
    assert(sB1.discCount == discB0 &&
           "Pin 2: discCount on B must not increment — the link never "
           "reset");
    std::cout << "  Pin 2 PASS (idle link survived the BREAK, suspicion "
                 "cleared via keepalive round trips, discCount unchanged)\n";
}

int main() {
    std::cout << "=== BREAK-Suspect Early-Keepalive Tests ===" << std::endl;
    test_overdue_keepalive_gets_early_wakeup();
    test_idle_link_survives_break_via_early_keepalive();
    std::cout << "\n=== All 2 BREAK-suspect early-keepalive pins PASS ===\n";
    return 0;
}

#endif
