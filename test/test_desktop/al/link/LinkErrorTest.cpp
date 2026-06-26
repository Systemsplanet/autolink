// Frame-error path: bad CRC, oversize.
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include <vector>
#    include "MockHal.h"
#    include "NullArqCache.h"

using namespace autolink;

void test_error_threshold() {
    NullArqCache cache;
    std::cout << "\n=== Test: Custom Error Thresholding ===" << std::endl;
    MockHal mHal;
    AutoLinkConfig cfg;
    cfg.allowedBauds[0] = 9600;
    cfg.allowedBauds[1] = 115200;
    cfg.allowedBaudsCount = 2;
    cfg.errThreshold = 2;
    cfg.pingSamplesPerBaud = 1;
    Link ping(mHal, cache, true, cfg);

    assert(ping.getState() == State::OK);
    ping.err();
    assert(ping.getState() == State::OK);
    assert(ping.getErrCount() == 1);

    ping.clearErr();
    assert(ping.getErrCount() == 0);

    ping.err();
    ping.err();
    assert(ping.getState() == State::OK);
    assert(ping.getErrCount() == 2);

    ping.err();
    assert(ping.getState() == State::SWP);
    std::cout << "PASS" << std::endl;
}

void test_error_counter() {
    AutoLinkConfig cfg;

    std::cout << "\n=== Test: Disconnect Counter = One Per Link Drop ==="
              << std::endl;

    {
        NullArqCache cache;
        MockHal mHal, sHal;
        cfg.errThreshold = 1000;
        Link a(mHal, cache, true, cfg);
        Link b(sHal, cache, false, cfg);
        uint8_t rx[32];
        Stats bs;

        b.getStats(bs);
        assert(bs.discCount == 0);

        for (int k = 0; k < 10; k++) {
            mHal.txBuf.clear();
            uint8_t m[] = { (uint8_t)k, 0xAA, 0xBB };
            assert(a.sendMsg(m, 3));
            mHal.txBuf[mHal.txBuf.size() / 2] ^= 0x80;
            pipe_data(mHal, sHal);
            b.recvMsg(rx, sizeof(rx));
        }
        b.getStats(bs);
        assert(bs.discCount == 0);
    }
    (void)0;

    {
        NullArqCache cache;
        MockHal mHal, sHal;
        cfg.errThreshold = 2;
        Link a(mHal, cache, true, cfg);
        Link b(sHal, cache, false, cfg);
        Stats bs;

        b.getStats(bs);
        assert(bs.discCount == 0);

        for (int i = 0; i < 6; i++)
            b.err();
        assert(b.getState() == State::SWP);
        b.getStats(bs);
        assert(bs.discCount == 1);

        for (int i = 0; i < 100; i++)
            b.err();
        b.getStats(bs);
        assert(bs.discCount == 1);

        b.resetStats();
        b.getStats(bs);
        assert(bs.discCount == 1);

        b.resetErrors();
        b.getStats(bs);
        assert(bs.discCount == 0);
    }

    {
        NullArqCache cache;
        MockHal mHal, sHal;
        Link a(mHal, cache, true, cfg);
        Stats as;
        a.getStats(as);
        (void)as.tx;
        (void)as.rx;
    }

    std::cout << "PASS" << std::endl;
}

void test_error_counter_during_swp() {
    NullArqCache cache;
    std::cout << "\n=== Test: One Count Per Cable Bounce ===" << std::endl;
    MockHal mHal, sHal;
    AutoLinkConfig cfg;
    cfg.allowedBauds[0] = 9600;
    cfg.allowedBauds[1] = 115200;
    cfg.allowedBaudsCount = 2;
    cfg.pingSamplesPerBaud = 1;
    cfg.errThreshold = 2;
    Link ping(mHal, cache, true, cfg);
    Link pong(sHal, cache, false, cfg);
    negotiate_to_ok(ping, pong, mHal, sHal);

    Stats s0;
    ping.getStats(s0);
    assert(s0.discCount == 0);

    for (int i = 0; i < 6; i++)
        ping.err();
    assert(ping.getState() == State::SWP);
    ping.getStats(s0);
    assert(s0.discCount == 1);

    for (int i = 0; i < 100; i++)
        ping.err();
    ping.getStats(s0);
    assert(s0.discCount == 1);

    for (int i = 0; i < 3; i++) {
        mHal.pumpClock(50);
        ping.onTimer();
        if (!mHal.txBuf.empty())
            pipe_data(mHal, sHal);
    }
    ping.getStats(s0);
    assert(s0.discCount == 1);

    std::cout << "PASS" << std::endl;
}

void test_error_counter_link_failures() {
    std::cout << "\n=== Test: Error Counter Ticks on Link Failures ==="
              << std::endl;

    {
        NullArqCache cache;
        MockHal mHal, sHal;
        AutoLinkConfig cfg;
        cfg.allowedBauds[0] = 9600;
        cfg.allowedBauds[1] = 115200;
        cfg.allowedBaudsCount = 2;
        cfg.pingSamplesPerBaud = 1;
        Link ping(mHal, cache, true, cfg);
        Link pong(sHal, cache, false, cfg);
        ping.begin();
        pong.begin();
        Stats s0;
        ping.getStats(s0);
        assert(s0.discCount == 0);
    }

    {
        NullArqCache cache;
        MockHal mHal, sHal;
        AutoLinkConfig cfg;
        cfg.allowedBauds[0] = 9600;
        cfg.allowedBauds[1] = 115200;
        cfg.allowedBaudsCount = 2;
        cfg.pingSamplesPerBaud = 1;
        cfg.idleTimeoutMs = 100;
        Link ping(mHal, cache, true, cfg);
        Link pong(sHal, cache, false, cfg);
        negotiate_to_ok(ping, pong, mHal, sHal);

        mHal.now = cfg.idleTimeoutMs + 200;
        mHal.pumpClock(200);
        ping.onTimer();
        assert(ping.getState() == State::SWP);

        Stats s1;
        ping.getStats(s1);
        assert(s1.discCount == 1);
    }

    {
        NullArqCache cache;
        MockHal mHal, sHal;
        AutoLinkConfig cfg;
        cfg.allowedBauds[0] = 9600;
        cfg.allowedBauds[1] = 115200;
        cfg.allowedBaudsCount = 2;
        cfg.pingSamplesPerBaud = 1;
        Link ping(mHal, cache, true, cfg);
        Link pong(sHal, cache, false, cfg);
        negotiate_to_ok(ping, pong, mHal, sHal);

        ping.onBreak();
        assert(ping.getState() == State::SWP);

        Stats s;
        ping.getStats(s);
        assert(s.discCount == 1);
    }

    {
        NullArqCache cache;
        MockHal mHal, sHal;
        AutoLinkConfig cfg;
        cfg.allowedBauds[0] = 9600;
        cfg.allowedBauds[1] = 115200;
        cfg.allowedBaudsCount = 2;
        cfg.pingSamplesPerBaud = 1;
        cfg.errThreshold = 2;
        Link ping(mHal, cache, true, cfg);
        Link pong(sHal, cache, false, cfg);
        negotiate_to_ok(ping, pong, mHal, sHal);

        for (int i = 0; i < 6; i++)
            ping.err();
        assert(ping.getState() == State::SWP);
        Stats s;
        ping.getStats(s);
        assert(s.discCount == 1);

        for (int i = 0; i < 100; i++) {
            mHal.pumpClock(50);
            mHal.pumpClock(50);
            ping.onTimer();
        }
        Stats s2;
        ping.getStats(s2);
        assert(s2.discCount == 1);
    }

    {
        NullArqCache cache;
        MockHal mHal, sHal;
        AutoLinkConfig cfg;
        cfg.allowedBauds[0] = 9600;
        cfg.allowedBauds[1] = 115200;
        cfg.allowedBaudsCount = 2;
        cfg.pingSamplesPerBaud = 1;
        cfg.idleTimeoutMs = 100;
        Link ping(mHal, cache, true, cfg);
        Link pong(sHal, cache, false, cfg);
        negotiate_to_ok(ping, pong, mHal, sHal);

        mHal.now = cfg.idleTimeoutMs + 200;
        mHal.pumpClock(200);
        ping.onTimer();
        assert(ping.getState() == State::SWP);

        for (int i = 0; i < 20; i++)
            ping.err();
        for (int i = 0; i < 5; i++)
            ping.err();

        mHal.pumpClock(50);
        ping.onTimer();
        pipe_data(mHal, sHal);
        mHal.pumpClock(50);
        ping.onTimer();
        pipe_data(mHal, sHal);
        mHal.pumpClock(50);
        ping.onTimer();
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);

        Stats s;
        ping.getStats(s);
        assert(s.discCount == 1);
    }

    {
        NullArqCache cache;
        MockHal mHal, sHal;
        AutoLinkConfig cfg;
        cfg.allowedBauds[0] = 9600;
        cfg.allowedBauds[1] = 115200;
        cfg.allowedBaudsCount = 2;
        cfg.pingSamplesPerBaud = 1;
        Link ping(mHal, cache, true, cfg);
        Link pong(sHal, cache, false, cfg);
        negotiate_to_ok(ping, pong, mHal, sHal);

        ping.onBreak();
        assert(ping.getState() == State::SWP);
        for (int i = 0; i < 5; i++)
            ping.onBreak();
        Stats s;
        ping.getStats(s);
        assert(s.discCount == 1);

        for (int i = 0; i < 200; i++) {
            mHal.pumpClock(50);
            mHal.pumpClock(50);
            ping.onTimer();
        }
        ping.getStats(s);
        assert(s.discCount == 1);
    }

    std::cout << "PASS" << std::endl;
}

void test_scattered_errors_dont_drop() {
    NullArqCache cache;
    AutoLinkConfig cfg;
    std::cout << "\n=== Test: Scattered Errors Don't Drop a Working Link ==="
              << std::endl;
    cfg.streamBufferSize = 8192;
    MockHal mHal, sHal;
    Link a(mHal, cache, true, cfg);
    Link b(sHal, cache, false, cfg);

    uint8_t badFrame[] = { 0x00, 0x02, 0xFF, 0x00 };
    uint8_t msg[] = { 0x11, 0x22, 0x33, 0x44 };

    for (int k = 0; k < 20; k++) {
        b.onRx(badFrame, sizeof(badFrame));
        assert(b.getState() == State::OK);
        assert(a.sendMsg(msg, sizeof(msg)));
        pipe_data(mHal, sHal);
        assert(b.getState() == State::OK);
        uint8_t rx[16];
        assert(b.recvMsg(rx, sizeof(rx)) == (int)sizeof(msg));
        assert(b.getErrCount() == 0);
    }

    for (int k = 0; k <= (int)cfg.errThreshold; k++)
        b.onRx(badFrame, sizeof(badFrame));
    assert(b.getState() == State::SWP);
    std::cout << "PASS" << std::endl;
}

void test_parser_yields_after_drop() {
    NullArqCache cache;
    AutoLinkConfig cfg;
    std::cout << "\n=== Test: Parser Yields to Command Parser After Drop ==="
              << std::endl;

    cfg.allowedBauds[0] = 115200;
    cfg.allowedBauds[1] = 9600;
    cfg.allowedBaudsCount = 2;
    cfg.pingSamplesPerBaud = 1;
    MockHal mHal, sHal;
    sHal.peer = &mHal;
    Link pingNode(mHal, cache, true, cfg);
    Link pong(sHal, cache, false, cfg);

    pingNode.begin();
    pong.begin();

    uint8_t bad[] = { 0x00, 0x02, 0xFF, 0x00, 0x02, 0xFF, 0x00 };
    uint8_t ping[5] = { 0xAA, 0x55, 0, PING_CMD, 0 };

    uint8_t crc = 0;
    for (int i = 0; i < 4; i++) {
        crc ^= ping[i];
        for (int k = 0; k < 8; k++)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07)
                               : (uint8_t)(crc << 1);
    }
    ping[4] = crc;

    std::vector<uint8_t> event(bad, bad + sizeof(bad));
    event.insert(event.end(), ping, ping + 5);
    pong.onRx(event.data(), (int)event.size());

    assert(pong.getState() == State::SWP);

    // Pong P1 PING promotes to P2: the bad-prefix
    // bytes are flushed by the parser, the PING is
    // then processed normally, and the promotion
    // leaves spdI at 0 (the new P2 baud). The
    // pre-fix shape stayed at spdI=1 because the
    // P1 PING handler returned SendPongAck, which
    // kept the slave parked at the P1 baud.
    assert(pong.getCurrentSpdIndex() == 0);
    std::cout << "PASS" << std::endl;
}

int main() {
    std::cout << "=== Running ALinkError Tests ===" << std::endl;
    test_error_threshold();
    test_error_counter();
    test_error_counter_during_swp();
    test_error_counter_link_failures();
    test_scattered_errors_dont_drop();
    test_parser_yields_after_drop();
    std::cout << "\n=== ALinkError Tests Completed Successfully ==="
              << std::endl;
    return 0;
}

#endif