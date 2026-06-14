// ALinkWatchdogTest.cpp — host-only tests for ALink post-OK watchdogs
// and recovery: idle watchdog, keepalive (atom, raw-mode, recent-TX),
// LCK timeout, asymmetric peer-death recovery.
#ifndef ARDUINO

#include <iostream>
#include <cassert>
#include <vector>
#include "MockHal.h"

using namespace autolink;

void test_idle_watchdog() {
    std::cout << "\n=== Test: Idle Watchdog Drops a Silent Link ===" << std::endl;
    AutoLinkConfig cfg;
    cfg.allowedBauds = {9600, 115200}; cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 3000;
    MockHal mHal, sHal;
    mHal.peer = &sHal; sHal.peer = &mHal;
    ALink ping(mHal, true, cfg);
    ALink pong(sHal, false, cfg);
    negotiate_to_ok(ping, pong, mHal, sHal);

    // Entering OK must arm the watchdog tick.
    assert(mHal.timerActive);
    assert(sHal.timerActive);
    assert(mHal.lastTimerMs == 1000);   // idleTimeoutMs / 3

    // Quiet tick: no drop, timer re-armed.
    mHal.now = 500;
    ping.onTimer();
    assert(ping.getState() == State::OK);

    // Silence past the limit: ping drops, peer is broken to SWP too.
    mHal.now = 4000;
    int breaks = mHal.sendBreakCalls;
    ping.onTimer();
    assert(ping.getState() == State::SWP);
    assert(mHal.sendBreakCalls == breaks + 1);
    assert(pong.getState() == State::SWP);
    std::cout << "PASS" << std::endl;
}

void test_keepalive() {
    std::cout << "\n=== Test: Keepalive Stops a Quiet Link From Bouncing ===" << std::endl;
    AutoLinkConfig cfg;
    cfg.allowedBauds = {9600, 115200}; cfg.pingSamplesPerBaud = 1;
    cfg.fastBaudLock = false;
    cfg.reliableMode = true;
    cfg.idleTimeoutMs = 3000;
    MockHal mHal, sHal;
    mHal.peer = &sHal; sHal.peer = &mHal;
    ALink ping(mHal, true, cfg);
    ALink pong(sHal, false, cfg);
    negotiate_to_ok(ping, pong, mHal, sHal);
    mHal.clearTx(); sHal.clearTx();

    mHal.now = 1000;
    ping.onTimer();
    // v4.0.0: keepalive is [0x00, COBS(cobsSeq | CRC), 0x00] = 5 bytes
    // (COBS of a 2-byte input [cobsSeq, CRC] with a leading 0x00 is
    // [0x01, 0x02, cobsSeq, CRC] -- but the cobsSeq=0 case is special
    // because the input starts with 0x00; we get [0x01, 0x02, CRC] where
    // the second code byte is at index 2).
    assert(mHal.txBuf.size() == 5);
    assert(mHal.txBuf[0] == 0x00);
    assert(mHal.txBuf[4] == 0x00);

    sHal.now = 1000;
    pipe_data(mHal, sHal);
    assert(pong.getState() == State::OK);
    assert(pong.available() == 0);
    assert(pong.getErrCount() == 0);

    pong.onTimer();
    mHal.now = 2900;
    pipe_data(sHal, mHal);
    ping.onTimer();
    assert(ping.getState() == State::OK);
    std::cout << "PASS" << std::endl;
}

void test_keepalive_disabled_in_raw_mode() {
    std::cout << "\n=== Test: Keepalive Suppressed in Raw Mode ===" << std::endl;
    AutoLinkConfig cfg;
    cfg.allowedBauds = {9600, 115200}; cfg.pingSamplesPerBaud = 1;
    cfg.reliableMode = false;
    cfg.idleTimeoutMs = 3000;
    MockHal mHal, sHal;
    mHal.peer = &sHal; sHal.peer = &mHal;
    ALink ping(mHal, true, cfg);
    ALink pong(sHal, false, cfg);
    negotiate_to_ok(ping, pong, mHal, sHal);
    mHal.clearTx(); sHal.clearTx();

    mHal.now = 1000;
    ping.onTimer();
    assert(mHal.txBuf.empty());
    std::cout << "PASS" << std::endl;
}

void test_keepalive_quiet_after_recent_tx() {
    std::cout << "\n=== Test: Keepalive Skipped After Recent TX ===" << std::endl;
    AutoLinkConfig cfg;
    cfg.allowedBauds = {9600, 115200}; cfg.pingSamplesPerBaud = 1;
    cfg.fastBaudLock = false;
    cfg.reliableMode = true;
    cfg.idleTimeoutMs = 3000;
    MockHal mHal, sHal;
    mHal.peer = &sHal; sHal.peer = &mHal;
    ALink ping(mHal, true, cfg);
    ALink pong(sHal, false, cfg);
    negotiate_to_ok(ping, pong, mHal, sHal);

    mHal.now = 1000;
    uint8_t b = 0xAB;
    ping.write(&b, 1);
    mHal.clearTx();

    ping.onTimer();
    assert(mHal.txBuf.empty());
    std::cout << "PASS" << std::endl;
}

void test_lck_timeout() {
    std::cout << "\n=== Test: LCK Timeout Restarts the Sweep ===" << std::endl;
    AutoLinkConfig cfg;
    cfg.allowedBauds = {9600, 115200}; cfg.pingSamplesPerBaud = 1;
    MockHal mHal;   // no peer: REQs go nowhere
    ALink ping(mHal, true, cfg);
    ping.begin();
    ping.onTimer();              // PING@9600
    ping.onTimer();              // PING@115200 -> LCK
    assert(ping.getState() == State::LCK);

    for (int i = 0; i < 4; i++) ping.onTimer();
    assert(ping.getState() == State::LCK);
    ping.onTimer();
    assert(ping.getState() == State::SWP);
    assert(ping.getCurrentSpdIndex() == 0);
    std::cout << "PASS" << std::endl;
}

// Asymmetric peer-death recovery. v2.5 added the idle-channel watchdog so
// the ping drops to SWP and re-sweeps on its own when the pong goes silent.
// Here we exercise the same code path via err() trips (the real device
// path when the parser sees a flood of garbage after a peer's UART desyncs).
void test_asymmetric_peer_death_recovery() {
    std::cout << "\n=== Test: Asymmetric Peer-Death Recovery ===" << std::endl;
    AutoLinkConfig cfg;
    cfg.reliableMode = true;
    cfg.allowedBauds = {9600, 115200}; cfg.pingSamplesPerBaud = 1;
    cfg.fastBaudLock = false;
    cfg.idleTimeoutMs = 0;

    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    ALink ping(mHal, true, cfg);
    ALink pong(sHal, false, cfg);
    negotiate_to_ok(ping, pong, mHal, sHal);

    int sendBreakCallsBeforePong = sHal.sendBreakCalls;
    int sendBreakCallsBeforePing = mHal.sendBreakCalls;
    int errsBefore = pong.getErrCount();

    // 300 non-zero bytes overflow the COBS accumulator and trip err_unlocked.
    for (int burst = 0; burst < 20; burst++) {
        std::vector<uint8_t> garbage(300, 0xCC);
        pong.onRx(garbage.data(), (int)garbage.size());
        if (pong.getErrCount() < errsBefore) break;
    }

    assert(pong.getErrCount() == 0);   // dropped: errs reset to 0
    assert(pong.getState() == State::SWP);
    assert(sHal.sendBreakCalls == sendBreakCallsBeforePong + 1);
    assert(ping.getState() == State::SWP);
    assert(mHal.sendBreakCalls == sendBreakCallsBeforePing);

    // The ping is now sweeping on its own.
    ping.onTimer();
    assert(!mHal.txBuf.empty());
    // v4.0.0: control frame is {0xAA, 0x55, cobsSeq, PING_CMD, CRC8} = 5 bytes.
    assert(mHal.txBuf.size() == 5);
    assert(mHal.txBuf[3] == PING_CMD);

    pipe_data(mHal, sHal);
    assert(pong.getCurrentSpdIndex() == 1);
    assert(pong.getErrCount() == 0);

    std::cout << "PASS" << std::endl;
}

int main() {
    std::cout << "=== Running ALinkWatchdog Tests ===" << std::endl;
    test_idle_watchdog();
    test_keepalive();
    test_keepalive_disabled_in_raw_mode();
    test_keepalive_quiet_after_recent_tx();
    test_lck_timeout();
    test_asymmetric_peer_death_recovery();
    std::cout << "\n=== ALinkWatchdog Tests Completed Successfully ===" << std::endl;
    return 0;
}

#endif // ARDUINO
