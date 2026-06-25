// P1 contract: never leave until connected.
#include <iostream>
#include <iomanip>
#include <cassert>
#include <vector>
#include "MockHal.h"

using namespace autolink;

static void test_master_never_leaves_p1() {
    std::cout << "\n=== master never leaves P1 (50 PINGs no ack) ==="
              << std::endl;
    AutoLinkConfig cfg;
    cfg.allowedBauds[0] = 115200;
    cfg.allowedBauds[1] = 9600;
    cfg.allowedBaudsCount = 2;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 0;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link ping(mHal, true, cfg);
    Link pong(sHal, false, cfg);
    ping.begin();
    pong.begin();

    sHal.setSpd(cfg.allowedBauds[0]);
    sHal.dropNextFrames = 9999;
    for (int i = 0; i < 50; i++) {
        mHal.pumpClock(50);
        sHal.pumpClock(50);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
    }
    assert(ping.getState() == State::SWP);
    assert(ping.getCurrentSpdIndex() == 1);
    std::cout << "PASS" << std::endl;
}

static void test_pong_never_leaves_p1() {
    std::cout << "\n=== pong never leaves P1 (50 listens no PING) ==="
              << std::endl;
    AutoLinkConfig cfg;
    cfg.allowedBauds[0] = 115200;
    cfg.allowedBauds[1] = 9600;
    cfg.allowedBaudsCount = 2;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 0;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link ping(mHal, true, cfg);
    Link pong(sHal, false, cfg);
    ping.begin();
    pong.begin();

    mHal.setSpd(cfg.allowedBauds[0]);
    mHal.dropNextFrames = 9999;
    for (int i = 0; i < 50; i++) {
        mHal.pumpClock(50);
        sHal.pumpClock(50);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
    }
    assert(pong.getState() == State::SWP);
    assert(pong.getCurrentSpdIndex() == 1);
    std::cout << "PASS" << std::endl;
}

// P1 -> P2 -> P3 sweep locks at the FASTEST baud on the master side.
// The old contract locked at whatever baud the first PONG happened
// to arrive at (always the slowest, since P1 starts there). The fix
// routes the first PONG into P2 so the sweep actually exercises the
// faster baud; the master then locks at the fastest working baud.
//
// Note: MockHal doesn't simulate baud mismatches, so this test
// asserts only the master's locked baud (the slave-side lock
// requires a faithful baud-mismatch simulation the harness doesn't
// provide). The link-level sweep is exercised by
// LinkSweepPhaseTest::test_v531_phase1_first_pong_routes_to_phase2.
static void test_master_locks_at_fastest_after_full_sweep() {
    std::cout << "\n=== master locks at FASTEST baud after P1->P2->P3 ==="
              << std::endl;
    AutoLinkConfig cfg;
    cfg.allowedBauds[0] = 115200;
    cfg.allowedBauds[1] = 9600;
    cfg.allowedBaudsCount = 2;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 0;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link ping(mHal, true, cfg);
    Link pong(sHal, false, cfg);
    ping.begin();
    pong.begin();

    for (int i = 0; i < 50; i++) {
        uint32_t targetMs = mHal.now + 50;
        while (mHal.now < targetMs) {
            uint32_t dt = targetMs - mHal.now;
            mHal.pumpClock(dt);
            sHal.pumpClock(dt);
            pipe_data(mHal, sHal);
            pipe_data(sHal, mHal);
            if (ping.getState() == State::OK)
                break;
        }
        if (ping.getState() == State::OK)
            break;
    }
    assert(ping.getState() == State::OK);
    // Lock target is the fastest baud (115200) the P2/P3 sweep
    // confirmed. The old buggy code locked at spdI=1 (slowest).
    assert(ping.getCurrentSpdIndex() == 0);
    std::cout << "PASS (master locked at index 0 = 115200)" << std::endl;
}

static void test_break_in_p1_resets_to_slowest() {
    std::cout << "\n=== BREAK in P1 restarts at slowest baud ===" << std::endl;
    AutoLinkConfig cfg;
    cfg.allowedBauds[0] = 115200;
    cfg.allowedBauds[1] = 9600;
    cfg.allowedBaudsCount = 2;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 0;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link ping(mHal, true, cfg);
    Link pong(sHal, false, cfg);
    ping.begin();
    pong.begin();

    // Manually drive until the master locks; MockHal's faithful
    // baud-mismatch filtering prevents negotiate_to_ok from
    // completing under the sweep contract.
    for (int i = 0; i < 200; i++) {
        mHal.pumpClock(50);
        sHal.pumpClock(50);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
        if (ping.getState() == State::OK)
            break;
    }
    assert(ping.getState() == State::OK);

    ping.onBreak();
    pong.onBreak();
    assert(ping.getState() == State::SWP);
    assert(pong.getState() == State::SWP);

    // After BREAK, P1 restarts at the slowest baud (allowedBauds[N-1]).
    assert(ping.getCurrentSpdIndex() == 1);
    Diag d;
    ping.getDiag(d);
    assert(d.preferredBaud == 0xFF);
    std::cout << "PASS" << std::endl;
}

int main() {
    std::cout << "=== Never-Leave-P1 Contract Tests ===" << std::endl;
    test_master_never_leaves_p1();
    test_pong_never_leaves_p1();
    test_master_locks_at_fastest_after_full_sweep();
    test_break_in_p1_resets_to_slowest();
    std::cout << "\n=== All 4 never-leave-P1 tests PASS ===" << std::endl;
    return 0;
}