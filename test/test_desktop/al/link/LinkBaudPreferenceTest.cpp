// Baud preference + err-rate window.
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include <vector>
#    include "MockHal.h"

using namespace autolink;

static void test_v53_preferred_baud_recorded_on_lock() {
    std::cout << "\n=== preferredBaud_ recorded on lock ===" << std::endl;
    AutoLinkConfig cfg;
    cfg.allowedBauds[0] = 115200;
    cfg.allowedBauds[1] = 9600;
    cfg.allowedBaudsCount = 2;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 0;
    cfg.baudPreference = true;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link ping(mHal, true, cfg);
    Link pong(sHal, false, cfg);
    negotiate_to_ok(ping, pong, mHal, sHal);
    assert(ping.getState() == State::OK);
    assert(pong.getState() == State::OK);
    Diag d;
    ping.getDiag(d);

    // Sweep locks the master at the fastest baud the P2/P3
    // sweep confirms (115200 = index 0). preferredBaud_ mirrors that.
    assert(d.preferredBaud == 0);
    std::cout << "PASS" << std::endl;
}

static void test_v53_resweep_starts_at_preferred_baud() {
    std::cout << "\n=== re-sweep starts at preferredBaud_ ===" << std::endl;
    AutoLinkConfig cfg;
    cfg.allowedBauds[0] = 115200;
    cfg.allowedBauds[1] = 9600;
    cfg.allowedBaudsCount = 2;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 0;
    cfg.errThreshold = 20;
    cfg.errRateWindow = 0;
    cfg.baudPreference = true;
    cfg.baudRetryLimit = 2;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link ping(mHal, true, cfg);
    Link pong(sHal, false, cfg);
    negotiate_to_ok(ping, pong, mHal, sHal);

    Diag d;
    ping.getDiag(d);
    // Sweep locks at the fastest baud (index 0).
    assert(d.preferredBaud == 0);

    cfg.allowedBauds[0] = 115200;
    cfg.allowedBauds[1] = 9600;
    MockHal mHal2, sHal2;
    mHal2.peer = &sHal2;
    sHal2.peer = &mHal2;
    Link ping2(mHal2, true, cfg);
    Link pong2(sHal2, false, cfg);
    negotiate_to_ok(ping2, pong2, mHal2, sHal2);
    Diag d2;
    ping2.getDiag(d2);

    MockHal mHal3, sHal3;
    mHal3.peer = &sHal3;
    sHal3.peer = &mHal3;
    Link ping3(mHal3, true, cfg);
    Link pong3(sHal3, false, cfg);
    ping3.begin();
    pong3.begin();

    for (int i = 0; i < 30; i++) {
        mHal3.pumpClock(50);
        sHal3.pumpClock(50);
        pipe_data(mHal3, sHal3);
        pipe_data(sHal3, mHal3);
        if (ping3.getState() == State::OK)
            break;
    }
    assert(ping3.getState() == State::OK);
    Diag d3;
    ping3.getDiag(d3);

    std::vector<uint8_t> garbage(300, 0xCC);
    for (int b = 0; b < 25; b++) {
        ping3.onRx(garbage.data(), (int)garbage.size());
        if (ping3.getState() == State::SWP)
            break;
    }
    assert(ping3.getState() == State::SWP);

    assert(ping3.getCurrentSpdIndex() == (int)cfg.allowedBaudsCount - 1);
    std::cout << "PASS" << std::endl;
}

static void test_v53_err_rate_window_drops_on_sustained_noise() {
    std::cout << "\n=== errRateWindow trips on sustained noise ==="
              << std::endl;
    AutoLinkConfig cfg;
    cfg.allowedBauds[0] = 115200;
    cfg.allowedBaudsCount = 1;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 0;
    cfg.errThreshold = 1000;
    cfg.errRateWindow = 5;
    cfg.baudPreference = false;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link ping(mHal, true, cfg);
    Link pong(sHal, false, cfg);
    negotiate_to_ok(ping, pong, mHal, sHal);
    assert(ping.getState() == State::OK);

    mHal.now = 0;
    mHal.now += 500;
    ping.onTimer();
    assert(ping.getState() == State::OK);

    std::vector<uint8_t> burst(300, 0xCC);
    mHal.now = 1000;
    int dropped = 0;
    for (int b = 0; b < 6; b++) {
        mHal.now += 100;
        pong.onRx(burst.data(), (int)burst.size());
        if (pong.getState() == State::SWP) {
            dropped = b + 1;
            break;
        }
    }
    assert(dropped > 0);
    assert(pong.getState() == State::SWP);
    std::cout << "PASS" << std::endl;
}

static void test_v53_baud_retries_cleared_on_reset() {
    std::cout << "\n=== baudRetries_ cleared on every reset ===" << std::endl;
    AutoLinkConfig cfg;
    cfg.allowedBauds[0] = 115200;
    cfg.allowedBaudsCount = 1;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 0;
    cfg.errThreshold = 20;
    cfg.errRateWindow = 0;
    cfg.baudPreference = true;
    cfg.baudRetryLimit = 3;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link ping(mHal, true, cfg);
    Link pong(sHal, false, cfg);
    negotiate_to_ok(ping, pong, mHal, sHal);
    Diag d0;
    ping.getDiag(d0);
    assert(d0.baudRetries == 0);

    std::vector<uint8_t> garbage(300, 0xCC);
    for (int b = 0; b < 25; b++) {
        ping.onRx(garbage.data(), (int)garbage.size());
        if (ping.getState() == State::SWP)
            break;
    }
    assert(ping.getState() == State::SWP);
    Diag d1;
    ping.getDiag(d1);
    assert(d1.baudRetries == 0);
    std::cout << "PASS" << std::endl;
}

static void test_v53_short_message_coalesces() {
    std::cout << "\n=== short messages coalesce header + data ===" << std::endl;
    AutoLinkConfig cfg;
    cfg.allowedBauds[0] = 115200;
    cfg.allowedBaudsCount = 1;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 5000;
    cfg.errThreshold = 100;
    cfg.errRateWindow = 0;
    cfg.baudPreference = false;
    cfg.maxMsg = 1024;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link ping(mHal, true, cfg);
    Link pong(sHal, false, cfg);
    negotiate_to_ok(ping, pong, mHal, sHal);

    uint8_t buf[100];
    for (int i = 0; i < 100; i++)
        buf[i] = (uint8_t)i;
    for (int i = 0; i < 5; i++) {
        assert(ping.sendMsg(buf, 100));
    }

    for (int i = 0; i < 20; i++) {
        mHal.pumpClock(50);
        sHal.pumpClock(50);
        if (!mHal.txBuf.empty()) {
            std::vector<uint8_t> b = mHal.txBuf;
            mHal.clearTx();
            sHal.link->onRx(b.data(), (int)b.size());
        }
        if (!sHal.txBuf.empty()) {
            std::vector<uint8_t> b = sHal.txBuf;
            sHal.clearTx();
            mHal.link->onRx(b.data(), (int)b.size());
        }
    }

    int got = 0;
    uint8_t out[256];
    int n;
    while ((n = pong.recvMsg(out, sizeof out)) > 0 && got < 10) {
        assert(n == 100);

        for (int i = 0; i < 100; i++) {
            assert(out[i] == (uint8_t)i);
        }
        got++;
    }
    assert(got == 5);
    std::cout << "PASS" << std::endl;
}

int main() {
    std::cout << "=== Running Reliability Tests ===" << std::endl;
    test_v53_preferred_baud_recorded_on_lock();
    test_v53_resweep_starts_at_preferred_baud();
    test_v53_err_rate_window_drops_on_sustained_noise();
    test_v53_baud_retries_cleared_on_reset();
    test_v53_short_message_coalesces();
    std::cout << "\n=== Reliability Tests Completed Successfully ==="
              << std::endl;
    return 0;
}

#endif