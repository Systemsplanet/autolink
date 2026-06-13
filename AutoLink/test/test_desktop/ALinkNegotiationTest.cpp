// ALinkNegotiationTest.cpp — host-only tests for the ALink auto-baud
// negotiation: SWP/LCK/OK state machine, best-baud scoring, top-down
// sweep + fast-ack.
#ifndef ARDUINO

#include <iostream>
#include <cassert>
#include <vector>
#include "MockHal.h"

using namespace autolink;

void test_negotiation_state_machine() {
    std::cout << "\n=== Test: Auto-Baud Negotiation State Machine ===" << std::endl;
    MockHal mHal, sHal;
    AutoLinkConfig cfg;
    cfg.allowedBauds = {9600, 115200}; cfg.pingSamplesPerBaud = 1;
    // Exercise the SWP -> LCK -> OK state transitions, not the
    // reliability sweep. One PING per baud keeps the state machine
    // test focused and fast. Disable fast-ack so the pong uses the
    // legacy REQ_CMD path.
    cfg.fastBaudLock = false;
    ALink ping(mHal, true, cfg);
    ALink pong(sHal, false, cfg);

    // ping.begin() -> MockHal::sendBreak() -> onBreak() [exactly once].
    // pong.begin()  -> arms SWP passively, no timer.
    ping.begin();
    pong.begin();

    assert(ping.getState() == State::SWP);
    assert(pong.getState() == State::SWP);
    assert(ping.getCurrentSpdIndex() == 0);
    assert(pong.getCurrentSpdIndex() == 0);

    // Tick 1: ping sends PING@9600, pong scores index 0.
    ping.onTimer();
    pipe_data(mHal, sHal);
    assert(ping.getCurrentSpdIndex() == 1);
    assert(pong.getCurrentSpdIndex() == 1);

    // Tick 2: ping sends PING@115200, pong scores index 1, ping -> LCK.
    ping.onTimer();
    pipe_data(mHal, sHal);
    assert(ping.getCurrentSpdIndex() == 0);  // reset to 0 on LCK entry
    assert(ping.getState() == State::LCK);

    // Tick 3: ping sends REQ_CMD; pong -> OK, replies best index.
    ping.onTimer();
    pipe_data(mHal, sHal);
    assert(pong.getState() == State::OK);

    // Ping receives pong's baud-index reply -> OK.
    pipe_data(sHal, mHal);
    assert(ping.getState() == State::OK);

    std::cout << "PASS" << std::endl;
}

void test_best_baud_selection() {
    std::cout << "\n=== Test: Best-Baud Picks Highest Working Index ===" << std::endl;
    // 4 bauds. Feed the pong 3 PINGs (it scores indices 0,1,2) then a REQ.
    // It must reply with index 2 (fastest baud that scored), not 3.
    MockHal sHal;
    AutoLinkConfig cfg; cfg.allowedBauds = {9600, 19200, 38400, 57600}; cfg.pingSamplesPerBaud = 1;
    cfg.fastBaudLock = false;
    ALink pong(sHal, false, cfg);
    pong.begin();
    assert(pong.getState() == State::SWP);

    auto frame = [&](uint8_t cmd, uint8_t* out) {
        out[0] = 0xAA; out[1] = 0x55; out[2] = cmd;
        uint8_t crc = 0;
        static const auto c8 = [](uint8_t crc, uint8_t d) {
            crc ^= d;
            for (int i = 0; i < 8; i++) crc = (crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1;
            return crc;
        };
        crc = c8(crc, out[0]); crc = c8(crc, out[1]); crc = c8(crc, out[2]);
        out[3] = crc;
    };

    uint8_t pf[4]; frame(PING_CMD, pf);
    pong.onRx(pf, 4);
    pong.onRx(pf, 4);
    pong.onRx(pf, 4);
    assert(pong.getCurrentSpdIndex() == 3);

    uint8_t rf[4]; frame(REQ_CMD, rf);
    pong.onRx(rf, 4);
    assert(pong.getState() == State::OK);

    // Reply frame is {0xAA,0x55,best,crc}; best is 2 (the highest baud that
    // scored with threshold 1 hit).
    assert(sHal.txBuf.size() == 4);
    assert(sHal.txBuf[2] == 2);
    std::cout << "PASS" << std::endl;
}

void test_top_down_fast_ack_locks_top() {
    // The user-requested behavior: the ping tests the fastest baud first.
    // If it passes, lock there. With top-down sweep + fast-ack, the pong
    // sends the best-ack after 4 PINGs at 115200, and the ping locks in
    // 4 ticks total.
    std::cout << "\n=== Test: Top-Down Sweep + Fast-Ack Locks Top Baud ===" << std::endl;

    AutoLinkConfig cfg;
    cfg.allowedBauds = {115200, 57600, 38400, 19200, 9600};
    cfg.pingSamplesPerBaud = 4;
    cfg.minAcceptRate = 0.5f;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    ALink ping(mHal, true, cfg);
    ALink pong(sHal, false, cfg);
    ping.begin(); pong.begin();

    for (int s = 0; s < 4; s++) {
        ping.onTimer();
        pipe_data(mHal, sHal);
    }
    int fastAckPayload = sHal.txBuf[2];
    pipe_data(sHal, mHal);

    assert(ping.getState() == State::OK);
    assert(pong.getState()   == State::OK);
    assert(fastAckPayload == 0);  // index 0 = 115200
    std::cout << "PASS" << std::endl;
}

int main() {
    std::cout << "=== Running ALinkNegotiation Tests ===" << std::endl;
    test_negotiation_state_machine();
    test_best_baud_selection();
    test_top_down_fast_ack_locks_top();
    std::cout << "\n=== ALinkNegotiation Tests Completed Successfully ===" << std::endl;
    return 0;
}

#endif // ARDUINO
