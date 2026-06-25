// 3-phase sweep contract: master/pong never leave P1
// until connected.


#include <iostream>
#include <iomanip>
#include <cassert>
#include <vector>
#include "MockHal.h"

using namespace autolink;

static void test_master_never_leaves_p1()
{
    std::cout << "\n=== master never leaves P1 (50 "
                 "PINGs no ack) ==="
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

static void test_pong_never_leaves_p1()
{
    std::cout << "\n=== pong never leaves P1 (50 "
                 "listens no PING) ==="
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

static void
test_master_locks_on_first_pong_ack_at_slowest()
{
    std::cout << "\n=== master locks on first "
                 "PONG_ACK at slowest baud ==="
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


    for (int i = 0; i < 10; i++) {
        mHal.pumpClock(50);
        sHal.pumpClock(50);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
        if (ping.getState() == State::OK &&
            pong.getState() == State::OK)
            break;
    }
    assert(ping.getState() == State::OK);
    assert(pong.getState() == State::OK);
    assert(ping.getCurrentSpdIndex() == 1);
    std::cout << "PASS" << std::endl;
}

static void test_break_in_p1_resets_to_slowest()
{
    std::cout << "\n=== BREAK in P1 restarts at "
                 "slowest baud ==="
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

    for (int i = 0; i < 20; i++) {
        mHal.pumpClock(50);
        sHal.pumpClock(50);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
        if (ping.getState() == State::OK &&
            pong.getState() == State::OK)
            break;
    }
    assert(ping.getState() == State::OK);
    assert(pong.getState() == State::OK);

    ping.onBreak();
    pong.onBreak();
    assert(ping.getState() == State::SWP);
    assert(pong.getState() == State::SWP);


    assert(ping.getCurrentSpdIndex() == 1);
    assert(pong.getCurrentSpdIndex() == 1);
    Diag d;
    ping.getDiag(d);
    assert(d.preferredBaud == 0xFF);
    std::cout << "PASS" << std::endl;
}

int main()
{
    std::cout
        << "=== Never-Leave-P1 Contract Tests ==="
        << std::endl;
    test_master_never_leaves_p1();
    test_pong_never_leaves_p1();
    test_master_locks_on_first_pong_ack_at_slowest();
    test_break_in_p1_resets_to_slowest();
    std::cout
        << "\n=== All 4 never-leave-P1 tests PASS ==="
        << std::endl;
    return 0;
}
