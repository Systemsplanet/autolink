// ALinkMessageTest.cpp — host-only tests for the ALink message API:
// sendMsg / recvMsg, boundaries across back-to-back frames, size sweep,
// CRC reject.
#ifndef ARDUINO

#include <iostream>
#include <cassert>
#include <vector>
#include <cstdlib>
#include "MockHal.h"

using namespace autolink;

void test_message_roundtrip() {
    std::cout << "\n=== Test: Message API Round-Trip (random sizes) ===" << std::endl;
    MockHal mHal, sHal;
    AutoLinkConfig cfg;
    cfg.reliableMode = true;
    cfg.streamBufferSize = 70000;
    cfg.maxMsg = 65535;
    ALink a(mHal, true, cfg);
    ALink b(sHal, false, cfg);

    srand(1234);
    std::vector<int> sizes = {1, 2, 3, 7, 250, 251, 500, 1000, 4096, 9001, 65535};
    for (int sz : sizes) {
        std::vector<uint8_t> tx(sz), rx(sz + 16);
        for (int i = 0; i < sz; i++) tx[i] = (uint8_t)(rand() & 0xFF);

        assert(a.sendMsg(tx.data(), sz));
        pipe_data(mHal, sHal);

        int got = b.recvMsg(rx.data(), rx.size());
        assert(got == sz);
        for (int i = 0; i < sz; i++) assert(rx[i] == tx[i]);
        assert(b.recvMsg(rx.data(), rx.size()) == 0);
    }
    std::cout << "PASS" << std::endl;
}

void test_message_boundaries_back_to_back() {
    std::cout << "\n=== Test: Back-to-Back Messages Keep Boundaries ===" << std::endl;
    MockHal mHal, sHal;
    AutoLinkConfig cfg; cfg.reliableMode = true; cfg.streamBufferSize = 8192;
    ALink a(mHal, true, cfg);
    ALink b(sHal, false, cfg);

    uint8_t m1[] = {1, 2, 3};
    uint8_t m2[] = {9, 8, 7, 6, 5};
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

// Integration sweep across the sizes the README promises to support. Covers
// three distinct classes of stress in one pass:
//
//   * Boundary framing: 1..10 B exercise single-frame COBS with very short
//     payloads, where the inner cobsDecode loop is fed a 1- or 2-byte run.
//   * Cross-chunk reassembly: 1000, 2000 B force 4 and 8 MAX_CHUNK=250 frames
//     in flight together, exercising the message reassembly state machine.
//   * Large-payload stress: 10000 B is 40 frames, big enough that a missing
//     memcpy or off-by-one in the chunker would corrupt the tail.
//
// Every iteration uses a distinct fill byte so a cross-message leak in the
// reassembly buffer would surface as a payload mismatch on the next recv
// (the single-message tests can't catch that). At each size we also send a
// back-to-back different-sized message to verify the receiver keeps the
// message boundary after the largest payloads, and we send a small-large-
// small sequence at the top size to exercise the parser across a multi-
// message burst at the same buffer occupancy.
void test_message_size_sweep() {
    std::cout << "\n=== Test: Message API Size Sweep (0..10000) ===" << std::endl;
    MockHal mHal, sHal;
    AutoLinkConfig cfg;
    cfg.reliableMode = true;
    cfg.streamBufferSize = 131072;
    cfg.maxMsg = 65535;
    ALink a(mHal, true, cfg);
    ALink b(sHal, false, cfg);

    // Zero-length: documented as rejected. Verify before we begin the sweep
    // so a regression in the length guard can't masquerade as a send success.
    {
        uint8_t scratch[1] = {0};
        bool sent = a.sendMsg(scratch, 0);
        assert(sent == false);
        assert(mHal.txBuf.empty());
        std::cout << "  [0]    rejected as expected" << std::endl;
    }

    mHal.txBuf.clear();
    uint64_t expectedTx = 0, expectedRx = 0;

    const std::vector<int> sizes = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 100, 1000, 2000, 10000};
    const std::vector<int> tailSizes = {3, 250, 7};

    for (size_t idx = 0; idx < sizes.size(); idx++) {
        const int sz = sizes[idx];
        const uint8_t fill = (uint8_t)(0xA0 ^ (uint8_t)idx);
        std::vector<uint8_t> tx(sz), rx(sz + 32);
        for (int i = 0; i < sz; i++) tx[i] = (uint8_t)(fill + i);

        assert(a.sendMsg(tx.data(), sz));
        int tailSz = tailSizes[idx % tailSizes.size()];
        uint8_t tailFill = (uint8_t)(fill ^ 0x5A);
        std::vector<uint8_t> tailTx(tailSz), tailRx(tailSz + 32);
        for (int i = 0; i < tailSz; i++) tailTx[i] = (uint8_t)(tailFill + i);
        assert(a.sendMsg(tailTx.data(), tailSz));

        pipe_data(mHal, sHal);

        int got = b.recvMsg(rx.data(), (int)rx.size());
        if (got != sz) {
            std::cerr << "  size=" << sz << " expected " << sz << " got " << got << std::endl;
            assert(false);
        }
        for (int i = 0; i < sz; i++) {
            if (rx[i] != tx[i]) {
                std::cerr << "  size=" << sz << " payload mismatch at i=" << i
                          << " (expected 0x" << std::hex << (int)tx[i]
                          << " got 0x" << (int)rx[i] << std::dec << ")" << std::endl;
                assert(false);
            }
        }

        int gotTail = b.recvMsg(tailRx.data(), (int)tailRx.size());
        if (gotTail != tailSz) {
            std::cerr << "  size=" << sz << " tail size " << tailSz
                      << " expected got=" << gotTail << std::endl;
            assert(false);
        }
        for (int i = 0; i < tailSz; i++) assert(tailRx[i] == tailTx[i]);

        uint8_t probe[16];
        assert(b.recvMsg(probe, sizeof(probe)) == 0);

        expectedTx += (uint64_t)(sz + tailSz) + (uint64_t)MSG_HDR * 2;
        expectedRx += (uint64_t)(sz + tailSz) + (uint64_t)MSG_HDR * 2;

        std::cout << "  [" << sz << " B / tail " << tailSz << " B] ok"
                  << "  (txBuf wire bytes so far: " << mHal.txBuf.size() << ")"
                  << std::endl;
    }

    // Multi-message burst at the top size.
    {
        const uint8_t fA = 0x11, fB = 0x22, fC = 0x33;
        std::vector<uint8_t> mA(3), mB(10000), mC(7);
        std::vector<uint8_t> rA(3 + 16), rB(10000 + 32), rC(7 + 16);
        for (int i = 0; i < 3; i++)    mA[i] = (uint8_t)(fA + i);
        for (int i = 0; i < 10000; i++) mB[i] = (uint8_t)(fB + i);
        for (int i = 0; i < 7; i++)    mC[i] = (uint8_t)(fC + i);

        assert(a.sendMsg(mA.data(), 3));
        assert(a.sendMsg(mB.data(), 10000));
        assert(a.sendMsg(mC.data(), 7));
        pipe_data(mHal, sHal);

        assert(b.recvMsg(rA.data(), (int)rA.size()) == 3);
        for (int i = 0; i < 3; i++) assert(rA[i] == mA[i]);
        assert(b.recvMsg(rB.data(), (int)rB.size()) == 10000);
        for (int i = 0; i < 10000; i++) {
            if (rB[i] != mB[i]) {
                std::cerr << "  burst: B mismatch at i=" << i << std::endl;
                assert(false);
            }
        }
        assert(b.recvMsg(rC.data(), (int)rC.size()) == 7);
        for (int i = 0; i < 7; i++) assert(rC[i] == mC[i]);
        assert(b.recvMsg(rA.data(), (int)rA.size()) == 0);
        std::cout << "  [burst: 3 + 10000 + 7] ok" << std::endl;
    }

    expectedTx += (uint64_t)(3 + 10000 + 7) + (uint64_t)MSG_HDR * 3;
    expectedRx += (uint64_t)(3 + 10000 + 7) + (uint64_t)MSG_HDR * 3;
    uint64_t atx, arx, btx, brx, aerr, berr;
    a.getStats(atx, arx, aerr);
    b.getStats(btx, brx, berr);
    assert(atx == expectedTx);
    assert(brx == expectedRx);
    assert(atx == brx);
    assert(aerr == 0);
    assert(berr == 0);
    std::cout << "  [stats] sender tx=" << atx << " receiver rx=" << brx
              << " (expected " << expectedTx << ")" << std::endl;

    std::cout << "PASS" << std::endl;
}

void test_message_crc_reject() {
    std::cout << "\n=== Test: Corrupt Message Rejected (CRC16) ===" << std::endl;
    MockHal mHal, sHal;
    AutoLinkConfig cfg; cfg.reliableMode = true; cfg.streamBufferSize = 8192;
    ALink a(mHal, true, cfg);
    ALink b(sHal, false, cfg);

    uint8_t msg[] = {0x10, 0x20, 0x30, 0x40};
    assert(a.sendMsg(msg, 4));
    assert(!mHal.txBuf.empty());
    mHal.txBuf[mHal.txBuf.size() / 2] ^= 0x01;
    pipe_data(mHal, sHal);

    uint8_t rx[32];
    int r = b.recvMsg(rx, sizeof(rx));
    // Per-frame CRC8 dropped the frame (0) or message CRC16 caught it (-1).
    assert(r <= 0);
    assert(b.getErrCount() > 0);
    std::cout << "PASS" << std::endl;
}

int main() {
    std::cout << "=== Running ALinkMessage Tests ===" << std::endl;
    test_message_roundtrip();
    test_message_boundaries_back_to_back();
    test_message_size_sweep();
    test_message_crc_reject();
    std::cout << "\n=== ALinkMessage Tests Completed Successfully ===" << std::endl;
    return 0;
}

#endif // ARDUINO
