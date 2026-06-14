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
    std::cout << "\n=== Test: Best-Baud Picks First Qualifying Index (Fastest-First) ===" << std::endl;
    // v4.0.0: pickBest() iterates from the FASTEST baud (lowest index) to
    // the slowest, returning the first one that meets the reliability
    // threshold. With 4 bauds and 1 PING each, the threshold is 1 hit
    // (pingSamplesPerBaud=1, minHits = max(1, 0) = 1), so the fastest
    // baud (index 0) qualifies before the higher-indexed ones.
    //
    // This is the v3.2.10 behavior: lock at the fastest baud that's
    // physically reachable, not the slowest one that happened to
    // accumulate hits first.
    MockHal sHal;
    AutoLinkConfig cfg; cfg.allowedBauds = {9600, 19200, 38400, 57600}; cfg.pingSamplesPerBaud = 1;
    cfg.fastBaudLock = false;
    ALink pong(sHal, false, cfg);
    pong.begin();
    assert(pong.getState() == State::SWP);

    auto frame = [&](uint8_t cmd, uint8_t cobsSeq, uint8_t* out) {
        // v4.0.0: control frame is {0xAA, 0x55, cobsSeq, payload, CRC8(first 4)}
        out[0] = 0xAA; out[1] = 0x55; out[2] = cobsSeq; out[3] = cmd;
        uint8_t crc = 0;
        static const auto c8 = [](uint8_t crc, uint8_t d) {
            crc ^= d;
            for (int i = 0; i < 8; i++) crc = (crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1;
            return crc;
        };
        crc = c8(crc, out[0]); crc = c8(crc, out[1]); crc = c8(crc, out[2]); crc = c8(crc, out[3]);
        out[4] = crc;
    };

    uint8_t pf[5]; frame(PING_CMD, /*cobsSeq=*/0, pf);
    pong.onRx(pf, 5);
    pong.onRx(pf, 5);
    pong.onRx(pf, 5);
    assert(pong.getCurrentSpdIndex() == 3);

    uint8_t rf[5]; frame(REQ_CMD, /*cobsSeq=*/0, rf);
    pong.onRx(rf, 5);
    assert(pong.getState() == State::OK);

    // Reply frame is {0xAA, 0x55, cobsSeq, best, CRC8}. v4.0.0 fastest-
    // first pickBest returns 0 (the first qualifying baud).
    assert(sHal.txBuf.size() == 5);
    assert(sHal.txBuf[3] == 0);
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
    int fastAckPayload = sHal.txBuf[3];  // v4.0.0: [0xAA, 0x55, cobsSeq, payload, CRC8]
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
