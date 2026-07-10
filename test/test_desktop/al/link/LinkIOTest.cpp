// Link IO: send/recv round-trip, stream.
#ifndef ARDUINO

#    include <iostream>
#    include <iomanip>
#    include <chrono>
#    include <cassert>
#    include <cstring>
#    include <vector>
#    include "MockHal.h"
#    include "NullArqCache.h"

using namespace autolink;

void test_basic_io() {
    NullArqCache cache;
    std::cout << "\n=== Test: Basic Write/Read/Peek/Flush/Available ==="
              << std::endl;
    MockHal mHal, sHal;
    AutoLinkConfig cfg;
    Link ping(mHal, cache, true, cfg);
    Link pong(sHal, cache, false, cfg);

    uint8_t data[] = { 0x11, 0x22 };
    ping.write(data, 2);
    ping.flush();

    pong.onRx(mHal.txBuf.data(), mHal.txBuf.size());

    assert(pong.available() == 2);
    assert(pong.peek() == 0x11);
    assert(pong.available() == 2);

    uint8_t rb_arr[10];
    assert(pong.read(rb_arr, 10) == 2);
    assert(rb_arr[0] == 0x11);
    assert(rb_arr[1] == 0x22);

    assert(pong.available() == 0);
    std::cout << "PASS" << std::endl;
}

void test_reliable_mode() {
    NullArqCache cache;
    AutoLinkConfig cfg;
    std::cout << "\n=== Test: Reliable Mode (COBS) ===" << std::endl;
    MockHal mHal, sHal;
    Link ping(mHal, cache, true, cfg);
    Link pong(sHal, cache, false, cfg);

    uint8_t data[] = { 0xAA, 0xBB };
    ping.write(data, 2);
    assert(!mHal.txBuf.empty());

    pong.onRx(mHal.txBuf.data(), mHal.txBuf.size());
    uint8_t rb_arr[10];
    assert(pong.read(rb_arr, 10) == 2);
    assert(rb_arr[0] == 0xAA);
    assert(rb_arr[1] == 0xBB);

    uint8_t bad_crc_frame[] = { 0x00, 0x04, 0x01, 0x02, 0xFF, 0x00 };
    pong.onRx(bad_crc_frame, sizeof(bad_crc_frame));
    assert(pong.getErrCount() > 0);

    std::cout << "PASS" << std::endl;
}

void test_throughput_and_sizes() {
    NullArqCache cache;
    std::cout << "\n=== Test: Payloads & Throughput (Reliable Mode) ==="
              << std::endl;
    MockHal mHal, sHal;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 32000;
    Link ping(mHal, cache, true, cfg);
    Link pong(sHal, cache, false, cfg);

    std::vector<int> sizes = { 0,   1,   2,    4,    8,    16,   32,   64,
                               128, 512, 1024, 2048, 4096, 8000, 16000 };

    std::cout << std::left << std::setw(15) << "Payload Size" << std::setw(20)
              << "Time Taken (s)" << std::setw(20) << "Bytes/Sec" << std::endl;
    std::cout << std::string(55, '-') << std::endl;

    for (int sz : sizes) {
        std::vector<uint8_t> txData(sz > 0 ? sz : 1);
        std::vector<uint8_t> rxData(sz > 0 ? sz : 1);

        for (int i = 0; i < sz; i++)
            txData[i] = i & 0xFF;

        auto start = std::chrono::high_resolution_clock::now();

        if (sz > 0)
            ping.write(txData.data(), sz);

        pipe_data(mHal, sHal);

        int bytesRead = 0;
        if (sz > 0) {
            int chunk;
            while ((chunk = pong.read(rxData.data() + bytesRead,
                                      sz - bytesRead)) > 0) {
                bytesRead += chunk;
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> diff = end - start;
        double bps = sz > 0 ? (sz / diff.count()) : 0.0;

        assert(bytesRead == sz);
        if (sz > 0) {
            for (int i = 0; i < sz; i++) {
                if (rxData[i] != txData[i]) {
                    std::cerr << "Data mismatch at index " << i << " for size "
                              << sz << std::endl;
                    assert(false);
                }
            }
        }

        std::cout << std::left << std::setw(15) << sz << std::setw(20)
                  << std::fixed << std::setprecision(6) << diff.count()
                  << std::setw(20) << std::fixed << std::setprecision(2) << bps
                  << std::endl;
    }

    std::cout << "\nPASS" << std::endl;
}

void test_stats() {
    NullArqCache cache;
    AutoLinkConfig cfg;
    std::cout << "\n=== Test: Throughput Counters ===" << std::endl;
    MockHal mHal, sHal;
    Link a(mHal, cache, true, cfg);
    Link b(sHal, cache, false, cfg);

    uint8_t msg[100];
    for (int i = 0; i < 100; i++)
        msg[i] = i;
    assert(a.sendMsg(msg, 100));
    pipe_data(mHal, sHal);
    uint8_t rx[128];
    assert(b.recvMsg(rx, sizeof(rx)) == 100);

    Stats as, bs;
    a.getStats(as);
    b.getStats(bs);

    assert(as.tx == 100);
    assert(bs.rx == 100 + MSG_HDR);
    assert(as.discCount == 0);
    assert(bs.discCount == 0);

    a.resetStats();
    a.getStats(as);
    assert(as.tx == 0 && as.rx == 0);
    assert(as.discCount == 0);
    std::cout << "PASS" << std::endl;
}

void test_readme_usage() {
    NullArqCache cache;
    std::cout << "\n=== Test: Real-world README Usage Simulation ==="
              << std::endl;

    AutoLinkConfig cfg;
    cfg.streamBufferSize = 2048;

    cfg.allowedBauds[0] = 115200;
    cfg.allowedBauds[1] = 9600;
    cfg.allowedBaudsCount = 2;
    cfg.pingSamplesPerBaud = 1;

    MockHal txHal, rxHal;
    Link txNode(txHal, cache, true, cfg);
    Link link(rxHal, cache, false, cfg);

    txNode.begin();
    link.begin();

    // Sync rxHal to the master's starting P1 baud so
    // the initial PING/PONG round reaches the slave
    // before the negotiation enters P2. Without this,
    // MockHal's baud-mismatch filter drops the first
    // PING and the slave never sees a transition.
    rxHal.setSpd(cfg.allowedBauds[cfg.allowedBaudsCount - 1]);

    // Pump the master timer + pipe data until both sides converge.
    // The P1->P2->P3 sweep takes more iterations than the
    // old lock-on-first-PONG path; let the negotiation play out.
    for (int i = 0; i < 200; i++) {
        txHal.pumpClock(50);
        txNode.onTimer();
        pipe_data(txHal, rxHal);
        pipe_data(rxHal, txHal);
        if (txNode.getState() == State::OK && link.getState() != State::OK)
            rxHal.setSpd(cfg.allowedBauds[txNode.getCurrentSpdIndex()]);
        if (txNode.getState() == State::OK && link.getState() == State::OK)
            break;
    }

    assert(txNode.getState() == State::OK);
    assert(link.getState() == State::OK);
    // Snap rxHal to the master's locked baud.
    // With the new P1->P2 routing, both sides reach
    // OK in the same iteration so the per-iteration
    // setSpd branch above never fires — set it
    // here, after the loop.
    rxHal.setSpd(cfg.allowedBauds[txNode.getCurrentSpdIndex()]);

    uint8_t payload[] = { 0xAB, 0xCD, 0xEF };
    txNode.write(payload, 3);
    // Pump enough ARQ round-trips for the master
    // to receive the slave's MSG_ACK. A single
    // pipe_data only moves the wire frame across;
    // the ARQ layer needs the master's onTimer to
    // fire and retransmit until the slave ACKs.
    int bytes_processed = 0;
    for (int i = 0; i < 30 && bytes_processed < (int)sizeof(payload); i++) {
        txHal.pumpClock(20);
        txNode.onTimer();
        pipe_data(txHal, rxHal);
        pipe_data(rxHal, txHal);
        while (link.available() && bytes_processed < (int)sizeof(payload)) {
            int b = link.read();
            std::cout << "Got: " << std::hex << std::uppercase << std::setw(2)
                      << std::setfill('0') << b << std::dec << std::endl;
            assert(b == payload[bytes_processed]);
            bytes_processed++;
        }
    }

    assert(bytes_processed == 3);
    std::cout << "PASS" << std::endl;
}

void test_io_coverage() {
    AutoLinkConfig cfg;
    std::cout << "\n=== Test: Link Public API Coverage ===" << std::endl;

    {
        NullArqCache cache;
        MockHal mHal, sHal;
        mHal.peer = &sHal;
        sHal.peer = &mHal;
        Link a(mHal, cache, true, cfg);
        Link b(sHal, cache, false, cfg);
        negotiate_to_ok(a, b, mHal, sHal);
        b.flushRx();

        assert(b.available() == 0);

        assert(b.peek() == -1);

        assert(b.read() == -1);

        uint8_t m1[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
        assert(a.sendMsg(m1, 4));
        pipe_data(mHal, sHal);
        assert(b.available() >= 6 + 4);
        assert(b.available() >= 6 + 4);

        assert(b.peek() == 0x04);

        assert(b.available() >= 6 + 4);

        int first = b.read();
        assert(first == 0x04);

        uint8_t rest[16];
        int got = b.read(rest, sizeof(rest));
        assert(got >= 6 + 4 - 1);
    }

    {
        NullArqCache cache;
        MockHal mHal, sHal;
        mHal.peer = &sHal;
        sHal.peer = &mHal;
        Link a(mHal, cache, true, cfg);
        Link b(sHal, cache, false, cfg);
        negotiate_to_ok(a, b, mHal, sHal);
        b.flushRx();
        uint8_t m1[20] = {};
        for (int i = 0; i < 20; i++)
            m1[i] = (uint8_t)(i + 1);
        assert(a.sendMsg(m1, 20));
        pipe_data(mHal, sHal);

        uint8_t tiny[3];
        int got = b.read(tiny, 3);
        assert(got == 3);
        assert(b.available() > 0);

        uint8_t rest[64];
        int got2 = b.read(rest, sizeof(rest));
        assert(got2 > 0);
    }

    {
        NullArqCache cache;
        MockHal mHal, sHal;
        mHal.peer = &sHal;
        sHal.peer = &mHal;
        Link a(mHal, cache, true, cfg);
        Link b(sHal, cache, false, cfg);
        negotiate_to_ok(a, b, mHal, sHal);

        assert(a.write((const uint8_t *)"x", -1) == 0);
        assert(a.write((const uint8_t *)"x", 0) == 0);

        a.dropLink();
        assert(a.getState() == State::SWP);
        assert(a.write((const uint8_t *)"x", 5) == 0);
    }

    {
        NullArqCache cache;
        MockHal mHal, sHal;
        mHal.peer = &sHal;
        sHal.peer = &mHal;
        cfg.maxMsg = 32;
        Link a(mHal, cache, true, cfg);
        Link b(sHal, cache, false, cfg);
        negotiate_to_ok(a, b, mHal, sHal);
        assert(a.sendMsg((const uint8_t *)"", 0) == true);

        uint8_t big[64] = {};
        assert(a.sendMsg(big, 64) == false);

        a.dropLink();
        assert(a.getState() == State::SWP);
        assert(a.sendMsg((const uint8_t *)"x", 1) == false);
    }

    {
        NullArqCache cache;
        MockHal mHal, sHal;
        mHal.peer = &sHal;
        sHal.peer = &mHal;
        Link a(mHal, cache, true, cfg);
        Link b(sHal, cache, false, cfg);
        negotiate_to_ok(a, b, mHal, sHal);

        int breaksBefore = mHal.sendBreakCalls;
        a.dropLink();

        assert(mHal.sendBreakCalls == breaksBefore + 1);
    }

    {
        NullArqCache cache;
        MockHal mHal, sHal;
        mHal.peer = &sHal;
        sHal.peer = &mHal;
        Link a(mHal, cache, true, cfg);
        Link b(sHal, cache, false, cfg);
        negotiate_to_ok(a, b, mHal, sHal);
        b.flushRx();
        uint8_t m1[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        assert(a.sendMsg(m1, 8));
        pipe_data(mHal, sHal);
        assert(b.available() >= 14);
        b.flushRx();
        assert(b.available() == 0);
    }

    {
        NullArqCache cache;
        MockHal mHal, sHal;
        mHal.peer = &sHal;
        sHal.peer = &mHal;
        Link a(mHal, cache, true, cfg);
        Link b(sHal, cache, false, cfg);
        negotiate_to_ok(a, b, mHal, sHal);

        size_t txBefore = mHal.txBuf.size();
        int wrote = a.write((const uint8_t *)"hello", 5);
        assert(wrote == 5);
        assert(mHal.txBuf.size() > txBefore + 5);

        pipe_data(mHal, sHal);
        uint8_t rx[8] = {};
        int got = b.read(rx, sizeof(rx));
        assert(got == 5);
        assert(memcmp(rx, "hello", 5) == 0);
    }

    {
        NullArqCache cache;
        MockHal mHal, sHal;
        mHal.peer = &sHal;
        sHal.peer = &mHal;
        Link a(mHal, cache, true, cfg);
        Link b(sHal, cache, false, cfg);
        negotiate_to_ok(a, b, mHal, sHal);
        b.flushRx();

        Diag d0;
        a.getDiag(d0);
        assert(d0.gaps == 0);
        assert(d0.stale == 0);
        assert(d0.lostMsgs == 0);

        a.resetDiag();
        assert(a.getState() == State::OK);
        Diag d1;
        a.getDiag(d1);
        assert(d1.gaps == 0);
        assert(d1.stale == 0);
        assert(d1.lostMsgs == 0);
    }

    {
        NullArqCache cache;
        MockHal mHal, sHal;
        mHal.peer = &sHal;
        sHal.peer = &mHal;
        Link a(mHal, cache, true, cfg);
        Link b(sHal, cache, false, cfg);
        negotiate_to_ok(a, b, mHal, sHal);
        b.flushRx();

        uint8_t m1[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        assert(a.sendMsg(m1, 8));
        pipe_data(mHal, sHal);
        uint8_t rx[16];
        b.recvMsg(rx, sizeof(rx));

        Stats sa;
        a.getStats(sa);
        assert(sa.tx >= 8);
        assert(sa.rx == 0);
        Stats sb;
        b.getStats(sb);
        assert(sb.rx >= 8);

        a.resetStats();
        a.getStats(sa);
        assert(sa.tx == 0);
        assert(sa.rx == 0);
        a.resetErrors();
        a.getStats(sa);
        assert(sa.discCount == 0);
    }

    {
        NullArqCache cache;
        MockHal mHal, sHal;
        mHal.peer = &sHal;
        sHal.peer = &mHal;
        Link a(mHal, cache, true, cfg);
        const AutoLinkConfig &c = a.getConfig();

        assert(c.streamBufferSize == 2048);
    }

    {
        NullArqCache cache;
        MockHal mHal, sHal;
        mHal.peer = &sHal;
        sHal.peer = &mHal;
        Link a(mHal, cache, true, cfg);
        assert(a.getState() == State::OK);
        assert(a.getErrCount() == 0);
        assert(a.getCurrentSpdIndex() >= 0);
    }

    {
        NullArqCache cache;
        MockHal mHal, sHal;
        mHal.peer = &sHal;
        sHal.peer = &mHal;
        Link a(mHal, cache, true, cfg);
        Link b(sHal, cache, false, cfg);
        negotiate_to_ok(a, b, mHal, sHal);
        b.flushRx();

        mHal.txFailN = 1;
        uint8_t m1[4] = { 1, 2, 3, 4 };

        assert(a.sendMsg(m1, 4) == true);
    }

    {
        NullArqCache cache;
        MockHal mHal, sHal;
        mHal.peer = &sHal;
        sHal.peer = &mHal;
        Link a(mHal, cache, true, cfg);

        assert(a.sendMsg((const uint8_t *)"x", -1) == false);
    }

    {
        NullArqCache cache;
        MockHal mHal, sHal;
        mHal.peer = &sHal;
        sHal.peer = &mHal;
        Link a(mHal, cache, true, cfg);
        mHal.txFailN = 6;
        uint8_t m1[4] = { 1, 2, 3, 4 };
        bool r = a.sendMsg(m1, 4);
        assert(r == true);
    }

    {
        NullArqCache cache;
        MockHal mHal, sHal;
        mHal.peer = &sHal;
        sHal.peer = &mHal;
        Link a(mHal, cache, true, cfg);
        Link b(sHal, cache, false, cfg);
        negotiate_to_ok(a, b, mHal, sHal);

        mHal.txFailN = 2;
        int errsBefore = a.getErrCount();
        int wrote = a.write((const uint8_t *)"hello", 5);
        assert(wrote == 5);

        (void)errsBefore;
    }

    {
        NullArqCache cache;
        MockHal mHal, sHal;
        mHal.peer = &sHal;
        sHal.peer = &mHal;
        cfg.maxMsg = 4096;
        Link a(mHal, cache, true, cfg);
        Link b(sHal, cache, false, cfg);
        negotiate_to_ok(a, b, mHal, sHal);
        b.flushRx();

        std::vector<uint8_t> big(1024, 0xAA);

        a.dropLink();

        int wrote = a.write(big.data(), (int)big.size());
        assert(wrote == 0);
    }

    {
        NullArqCache cache;
        MockHal mHal, sHal;
        mHal.peer = &sHal;
        sHal.peer = &mHal;
        Link a(mHal, cache, true, cfg);

        uint32_t b = a.getCurrentBaud();
        assert(b > 0);
    }

    {
        NullArqCache cache;
        MockHal mHal, sHal;
        AutoLinkConfig cfg;
        cfg.streamBufferSize = 64;
        cfg.maxMsg = 1024;
        Link a(mHal, cache, true, cfg);

        assert(a.getState() == State::OK);
    }

    std::cout << "PASS (full public-API surface covered)" << std::endl;
}

void test_txSeq_wraps_254_to_0_without_dropping_0xFF() {
    NullArqCache cache;
    std::cout
        << "\n=== Test: cobsSeq wraps 254→0 (no 0xFF collision) (the fix) ==="
        << std::endl;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 32000;
    Link ping(mHal, cache, true, cfg);
    Link pong(sHal, cache, false, cfg);
    while (ping.getState() != State::OK || pong.getState() != State::OK) {
        mHal.pumpClock(50);
        sHal.pumpClock(50);
        pipe_data(mHal, sHal);
    }

    const int N = 260;
    uint8_t payload = 0xAB;
    int sent = 0;
    while (sent < N) {
        int w = ping.write(&payload, 1);
        if (w <= 0) {
            mHal.pumpClock(10);
            sHal.pumpClock(10);
            pipe_data(mHal, sHal);
            continue;
        }
        sent += w;
        mHal.pumpClock(10);
        sHal.pumpClock(10);
        pipe_data(mHal, sHal);
    }

    for (int k = 0; k < 300; k++) {
        mHal.pumpClock(10);
        sHal.pumpClock(10);
        pipe_data(mHal, sHal);
    }

    uint8_t rx[N];
    int got = 0;
    int chunk;
    while ((chunk = pong.read(rx + got, N - got)) > 0) {
        got += chunk;
    }
    assert(got == N);
    for (int i = 0; i < got; i++) {
        assert(rx[i] == 0xAB);
    }

    Diag d;
    pong.getDiag(d);
    assert(d.gaps == 0);
    assert(d.stale == 0);
    std::cout
        << "PASS (260 frames round-trip across seq 254→0 wrap, zero loss, zero gaps)"
        << std::endl;
}

int main() {
    std::cout << "=== Running ALinkIO Tests ===" << std::endl;
    test_basic_io();
    test_reliable_mode();
    test_throughput_and_sizes();
    test_stats();
    test_readme_usage();
    test_io_coverage();
    test_txSeq_wraps_254_to_0_without_dropping_0xFF();
    std::cout << "\n=== ALinkIO Tests Completed Successfully ===" << std::endl;
    return 0;
}

#endif