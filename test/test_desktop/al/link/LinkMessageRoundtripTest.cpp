// Auto-generated split of the original LinkMessageTest.cpp.
// Each TU in this split covers a single concern
// (roundtrip / corrupt / resync / edge) and includes
// the shared harness via LinkMessageTestCommon.h.
// Run via `make run_test_alink_message_roundtrip` etc.

#include "LinkMessageTestCommon.h"

using namespace autolink;

void test_message_roundtrip() {
    NullArqCache cache;
    std::cout << "\n=== Test: Message API Round-Trip (random sizes) ==="
              << std::endl;
    MockHal mHal, sHal;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 70000;
    cfg.maxMsg = 65535;
    Link a(mHal, cache, true, cfg);
    Link b(sHal, cache, false, cfg);

    srand(1234);

    std::vector<int> sizes = { 1, 2, 3, 7, 250, 251, 500, 1000, 4096, 32000 };
    for (int sz : sizes) {
        std::vector<uint8_t> tx(sz), rx(sz + 16);
        for (int i = 0; i < sz; i++)
            tx[i] = (uint8_t)(rand() & 0xFF);

        assert(a.sendMsg(tx.data(), sz));
        pipe_data(mHal, sHal);

        int got = b.recvMsg(rx.data(), rx.size());
        assert(got == sz);
        for (int i = 0; i < sz; i++)
            assert(rx[i] == tx[i]);
        assert(b.recvMsg(rx.data(), rx.size()) == 0);
    }
    std::cout << "PASS" << std::endl;
}

void test_message_boundaries_back_to_back() {
    NullArqCache cache;
    AutoLinkConfig cfg;
    std::cout << "\n=== Test: Back-to-Back Messages Keep Boundaries ==="
              << std::endl;
    MockHal mHal, sHal;
    Link a(mHal, cache, true, cfg);
    Link b(sHal, cache, false, cfg);

    uint8_t m1[] = { 1, 2, 3 };
    uint8_t m2[] = { 9, 8, 7, 6, 5 };
    assert(a.sendMsg(m1, 3));
    assert(a.sendMsg(m2, 5));
    pipe_data(mHal, sHal);

    uint8_t rx[32];
    assert(b.recvMsg(rx, sizeof(rx)) == 3);
    assert(rx[0] == 1 && rx[2] == 3);
    assert(b.recvMsg(rx, sizeof(rx)) == 5);
    assert(rx[0] == 9 && rx[4] == 5);
    assert(b.recvMsg(rx, sizeof(rx)) == 0);
    std::cout << "PASS" << std::endl;
}

void test_message_size_sweep() {
    NullArqCache cache;
    std::cout << "\n=== Test: Message API Size Sweep (0..10000) ==="
              << std::endl;
    MockHal mHal, sHal;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 131072;
    cfg.maxMsg = 65535;
    Link a(mHal, cache, true, cfg);
    Link b(sHal, cache, false, cfg);

    {
        uint8_t scratch[1] = { 0 };
        bool sent = a.sendMsg(scratch, 0);
        assert(sent == true);
        assert(mHal.txBuf.empty());
        std::cout << " [0] no-op (true, no bytes)" << std::endl;
    }

    mHal.txBuf.clear();
    uint64_t expectedTx = 0, expectedRx = 0;

    const std::vector<int> sizes = { 1, 2, 3,  4,   5,    6,    7,
                                     8, 9, 10, 100, 1000, 2000, 10000 };
    const std::vector<int> tailSizes = { 3, 250, 7 };

    for (size_t idx = 0; idx < sizes.size(); idx++) {
        const int sz = sizes[idx];
        const uint8_t fill = (uint8_t)(0xA0 ^ (uint8_t)idx);
        std::vector<uint8_t> tx(sz), rx(sz + 32);
        for (int i = 0; i < sz; i++)
            tx[i] = (uint8_t)(fill + i);

        assert(a.sendMsg(tx.data(), sz));
        int tailSz = tailSizes[idx % tailSizes.size()];
        uint8_t tailFill = (uint8_t)(fill ^ 0x5A);
        std::vector<uint8_t> tailTx(tailSz), tailRx(tailSz + 32);
        for (int i = 0; i < tailSz; i++)
            tailTx[i] = (uint8_t)(tailFill + i);
        assert(a.sendMsg(tailTx.data(), tailSz));

        pipe_data(mHal, sHal);

        int got = b.recvMsg(rx.data(), (int)rx.size());
        if (got != sz) {
            std::cerr << " size=" << sz << " expected " << sz << " got " << got
                      << std::endl;
            assert(false);
        }
        for (int i = 0; i < sz; i++) {
            if (rx[i] != tx[i]) {
                std::cerr << " size=" << sz << " payload mismatch at i=" << i
                          << " (expected 0x" << std::hex << (int)tx[i]
                          << " got 0x" << (int)rx[i] << std::dec << ")"
                          << std::endl;
                assert(false);
            }
        }

        int gotTail = b.recvMsg(tailRx.data(), (int)tailRx.size());
        if (gotTail != tailSz) {
            std::cerr << " size=" << sz << " tail size " << tailSz
                      << " expected got=" << gotTail << std::endl;
            assert(false);
        }
        for (int i = 0; i < tailSz; i++)
            assert(tailRx[i] == tailTx[i]);

        uint8_t probe[16];
        assert(b.recvMsg(probe, sizeof(probe)) == 0);

        expectedTx += (uint64_t)(sz + tailSz);
        expectedRx += (uint64_t)(sz + tailSz) + (uint64_t)MSG_HDR * 2;

        std::cout << " [" << sz << " B / tail " << tailSz << " B] ok"
                  << " (txBuf wire bytes so far: " << mHal.txBuf.size() << ")"
                  << std::endl;
    }

    {
        const uint8_t fA = 0x11, fB = 0x22, fC = 0x33;
        std::vector<uint8_t> mA(3), mB(10000), mC(7);
        std::vector<uint8_t> rA(3 + 16), rB(10000 + 32), rC(7 + 16);
        for (int i = 0; i < 3; i++)
            mA[i] = (uint8_t)(fA + i);
        for (int i = 0; i < 10000; i++)
            mB[i] = (uint8_t)(fB + i);
        for (int i = 0; i < 7; i++)
            mC[i] = (uint8_t)(fC + i);

        assert(a.sendMsg(mA.data(), 3));
        assert(a.sendMsg(mB.data(), 10000));
        assert(a.sendMsg(mC.data(), 7));
        pipe_data(mHal, sHal);

        assert(b.recvMsg(rA.data(), (int)rA.size()) == 3);
        for (int i = 0; i < 3; i++)
            assert(rA[i] == mA[i]);
        assert(b.recvMsg(rB.data(), (int)rB.size()) == 10000);
        for (int i = 0; i < 10000; i++) {
            if (rB[i] != mB[i]) {
                std::cerr << " burst: B mismatch at i=" << i << std::endl;
                assert(false);
            }
        }
        assert(b.recvMsg(rC.data(), (int)rC.size()) == 7);
        for (int i = 0; i < 7; i++)
            assert(rC[i] == mC[i]);
        assert(b.recvMsg(rA.data(), (int)rA.size()) == 0);
        std::cout << " [burst: 3 + 10000 + 7] ok" << std::endl;
    }

    expectedTx += (uint64_t)(3 + 10000 + 7);
    expectedRx += (uint64_t)(3 + 10000 + 7) + (uint64_t)MSG_HDR * 3;
    Stats as, bs;
    a.getStats(as);
    b.getStats(bs);

    assert(as.tx == expectedTx);
    assert(bs.rx == expectedRx);
    assert(as.discCount == 0);
    assert(bs.discCount == 0);
    std::cout << " [stats] sender tx=" << as.tx
              << " (payload) receiver rx=" << bs.rx << " (payload+MSG_HDR)"
              << std::endl;

    std::cout << "PASS" << std::endl;
}

void test_message_small_size_boundary() {
    NullArqCache cache;
    AutoLinkConfig cfg;
    std::cout << "\n=== Test: Small-Size Boundary 1..6 ===" << std::endl;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link a(mHal, cache, true, cfg);
    Link b(sHal, cache, false, cfg);
    while (a.getState() != State::OK || b.getState() != State::OK) {
        mHal.pumpClock(50);
        sHal.pumpClock(50);
        pipe_data(mHal, sHal);
    }

    for (int sz = 1; sz <= 6; sz++) {
        uint8_t tx[6];
        for (int i = 0; i < sz; i++)
            tx[i] = (uint8_t)(0x10 + i + sz);
        bool ok = a.sendMsg(tx, sz);
        if (!ok) {
            std::cerr << "sendMsg(" << sz << ") returned false\n";
            assert(false);
        }
        pipe_data(mHal, sHal);
        uint8_t rx[16];
        int got = b.recvMsg(rx, sizeof(rx));
        if (got != sz) {
            std::cerr << "size=" << sz << " got=" << got << "\n";
            assert(false);
        }
        for (int i = 0; i < sz; i++)
            assert(rx[i] == tx[i]);

        uint8_t probe[1];
        assert(b.recvMsg(probe, sizeof(probe)) == 0);
        std::cout << " [sz=" << sz << "] ok\n";
    }
    std::cout << "PASS" << std::endl;
}

void test_message_explicit_size_sweep() {
    NullArqCache cache;
    std::cout << "\n=== Test: Explicit Size Sweep 1..300, 1000..maxMsg ==="
              << std::endl;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 131072;
    cfg.maxMsg = 65535;

    const std::vector<int> sizes = { 1,    2,    3,    4,    5,    50,
                                     100,  150,  200,  250,  300,  1000,
                                     2000, 3000, 4000, 5000, 7500, 9000 };
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link a(mHal, cache, true, cfg);
    Link b(sHal, cache, false, cfg);

    while (a.getState() != State::OK || b.getState() != State::OK) {
        mHal.pumpClock(50);
        sHal.pumpClock(50);
        pipe_data(mHal, sHal);
    }
    b.flushRx();

    for (int sz : sizes) {
        std::vector<uint8_t> tx(sz);
        for (int i = 0; i < sz; i++)
            tx[i] = (uint8_t)((i * 7 + sz) & 0xFF);
        bool ok = a.sendMsg(tx.data(), sz);
        if (!ok) {
            std::cerr << "sendMsg(" << sz << ") returned false\n";
            assert(false);
        }
        pipe_data(mHal, sHal);
        std::vector<uint8_t> rx(sz + 32);
        int got = b.recvMsg(rx.data(), (int)rx.size());
        if (got != sz) {
            std::cerr << "size=" << sz << " got=" << got << "\n";
            assert(false);
        }
        for (int i = 0; i < sz; i++) {
            if (rx[i] != tx[i]) {
                std::cerr << "size=" << sz << " payload mismatch at i=" << i
                          << " expected 0x" << std::hex << (int)tx[i]
                          << " got 0x" << (int)rx[i] << std::dec << "\n";
                assert(false);
            }
        }
        uint8_t probe[16];
        assert(b.recvMsg(probe, sizeof(probe)) == 0);
        std::cout << " [sz=" << sz << "] ok\n";
    }
    std::cout << "PASS" << std::endl;
}

void test_message_chunk_boundary_carries_then_rejects() {
    std::cout << "\n=== Test: 240-chunk carries, 241st rejected (ARQ cap) ==="
              << std::endl;

    test_internal::TestCache cacheA, cacheB;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 70000;
    cfg.maxMsg = 65535;
    Link a(mHal, cacheA, true, cfg);
    Link b(sHal, cacheB, false, cfg);
    while (a.getState() != State::OK || b.getState() != State::OK) {
        mHal.pumpClock(50);
        sHal.pumpClock(50);
        pipe_data(mHal, sHal);
    }

    std::vector<uint8_t> big(60000);
    for (size_t i = 0; i < big.size(); i++)
        big[i] = (uint8_t)(i & 0xFF);
    uint8_t base1 = 0;
    assert(a.sendMsg(big.data(), (int)big.size(), &base1) == true);
    pipe_data(mHal, sHal);
    std::vector<uint8_t> rx(big.size());
    assert(b.recvMsg(rx.data(), (int)rx.size()) == (int)big.size());
    for (size_t i = 0; i < big.size(); i++) {
        assert(rx[i] == big[i]);
    }

    std::vector<uint8_t> oneMore(60250);
    for (size_t i = 0; i < oneMore.size(); i++)
        oneMore[i] = (uint8_t)(i & 0xFF);
    uint8_t base2 = 0;
    assert(a.sendMsg(oneMore.data(), (int)oneMore.size(), &base2) == false);

    std::cout << "PASS (240 chunks carry, 241st rejected by ARQ cap gate)"
              << std::endl;
}

int main() {
    std::cout << "=== Running LinkMessageRoundtripTest Tests ===" << std::endl;

    Log::log().setLevel(Log::DEBUG);
    test_message_roundtrip();
    test_message_boundaries_back_to_back();
    test_message_size_sweep();
    test_message_small_size_boundary();
    test_message_explicit_size_sweep();
    test_message_chunk_boundary_carries_then_rejects();

    std::cout
        << "\n=== LinkMessageRoundtripTest Tests Completed Successfully ==="
        << std::endl;
    return 0;
}
