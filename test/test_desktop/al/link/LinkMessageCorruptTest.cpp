// Auto-generated split of the original LinkMessageTest.cpp.
// Each TU in this split covers a single concern
// (roundtrip / corrupt / resync / edge) and includes
// the shared harness via LinkMessageTestCommon.h.
// Run via `make run_test_alink_message_roundtrip` etc.

#include "LinkMessageTestCommon.h"

using namespace autolink;

void test_message_crc_reject() {
    NullArqCache cache;
    AutoLinkConfig cfg;
    std::cout << "\n=== Test: Corrupt Message Rejected (CRC16) ==="
              << std::endl;
    MockHal mHal, sHal;
    Link a(mHal, cache, true, cfg);
    Link b(sHal, cache, false, cfg);

    uint8_t msg[] = { 0x10, 0x20, 0x30, 0x40 };
    assert(a.sendMsg(msg, 4));
    assert(!mHal.txBuf.empty());
    mHal.txBuf[mHal.txBuf.size() / 2] ^= 0x01;
    pipe_data(mHal, sHal);

    uint8_t rx[32];
    int r = b.recvMsg(rx, sizeof(rx));

    assert(r <= 0);
    assert(b.getErrCount() > 0);
    std::cout << "PASS" << std::endl;
}

void test_corrupt_msg_header_does_not_clear_buffer() {
    NullArqCache cache;
    AutoLinkConfig cfg;
    std::cout
        << "\n=== Test: Corrupt MSG_HDR Drops Single Frame, Not Whole Buffer ==="
        << std::endl;
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

    uint8_t m1[10];
    for (int i = 0; i < 10; i++)
        m1[i] = (uint8_t)(0xA0 + i);
    uint8_t m2[20];
    for (int i = 0; i < 20; i++)
        m2[i] = (uint8_t)(0xB0 + i);
    assert(a.sendMsg(m1, 10));
    assert(a.sendMsg(m2, 20));
    pipe_data(mHal, sHal);
    int availBefore = b.available();
    assert(availBefore > 0);

    std::vector<uint8_t> scratch(availBefore);
    assert(b.read(scratch.data(), availBefore) == availBefore);
    assert(b.available() == 0);
    for (int i = 0; i < 6; i++)
        sHal.appBuf.push(0);
    for (int i = 0; i < availBefore; i++)
        sHal.appBuf.push(scratch[i]);
    assert(b.available() == availBefore + 6);

    uint8_t rx[32];
    int err = b.recvMsg(rx, sizeof(rx));

    assert(err == -1);

    int got = b.recvMsg(rx, sizeof(rx));
    assert(got == 10);
    for (int i = 0; i < 10; i++)
        assert(rx[i] == m1[i]);

    got = b.recvMsg(rx, sizeof(rx));
    assert(got == 20);
    for (int i = 0; i < 20; i++)
        assert(rx[i] == m2[i]);

    std::cout << "PASS (corrupt header dropped, m1 + m2 preserved)"
              << std::endl;
}

void test_corrupt_msg_header_no_resync_clears_buffer() {
    NullArqCache cache;
    AutoLinkConfig cfg;
    std::cout << "\n=== Test: Corrupt MSG_HDR With No Resync Clears Buffer ==="
              << std::endl;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    cfg.maxMsg = 64;
    Link a(mHal, cache, true, cfg);
    Link b(sHal, cache, false, cfg);
    while (a.getState() != State::OK || b.getState() != State::OK) {
        mHal.pumpClock(50);
        sHal.pumpClock(50);
        pipe_data(mHal, sHal);
    }
    b.flushRx();

    for (int i = 0; i < 200; i++)
        sHal.appBuf.push(0xFF);
    uint8_t rx[32];
    int err = b.recvMsg(rx, sizeof(rx));
    assert(err == -1);

    assert(b.available() == 0);

    assert(b.getState() == State::OK);

    uint8_t ok[8] = { 0xDE, 0xAD, 0xBE, 0xEF, 1, 2, 3, 4 };
    assert(a.sendMsg(ok, 8));
    pipe_data(mHal, sHal);
    int got = b.recvMsg(rx, sizeof(rx));
    assert(got == 8);
    for (int i = 0; i < 8; i++)
        assert(rx[i] == ok[i]);
    std::cout << "PASS (no-resync path cleared 200 bytes, next msg OK)"
              << std::endl;
}

void test_corrupt_payload_byte_crc_reject() {
    NullArqCache cache;
    AutoLinkConfig cfg;
    std::cout << "\n=== Test: Corrupt Payload Byte Rejected by CRC16 ==="
              << std::endl;
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

    uint8_t m1[16];
    for (int i = 0; i < 16; i++)
        m1[i] = (uint8_t)(i * 0x11);
    assert(a.sendMsg(m1, 16));
    pipe_data(mHal, sHal);
    uint8_t rx[32];
    int ok = b.recvMsg(rx, sizeof(rx));
    assert(ok == 16);
    for (int i = 0; i < 16; i++)
        assert(rx[i] == m1[i]);

    while (b.read(rx, sizeof(rx)) > 0) {
    }

    uint8_t m2[16];
    for (int i = 0; i < 16; i++)
        m2[i] = (uint8_t)(0x80 + i);
    assert(a.sendMsg(m2, 16));
    pipe_data(mHal, sHal);
    int avail = b.available();
    assert(avail >= 22);

    std::vector<uint8_t> snap(avail);
    int n = b.read(snap.data(), avail);
    assert(n == avail);
    assert(snap.size() > 10);
    snap[10] ^= 0x01;
    for (size_t i = 0; i < snap.size(); i++)
        sHal.appBuf.push(snap[i]);

    int errsBefore = b.getErrCount();
    int r = b.recvMsg(rx, sizeof(rx));

    assert(r <= 0);
    assert(b.getErrCount() > errsBefore);

    assert(b.getState() == State::OK);
    std::cout << "PASS (payload bit-flip caught by CRC, no leakage)"
              << std::endl;
}

int main() {
    std::cout << "=== Running LinkMessageCorruptTest Tests ===" << std::endl;

    Log::log().setLevel(Log::DEBUG);
    test_message_crc_reject();
    test_corrupt_msg_header_does_not_clear_buffer();
    test_corrupt_msg_header_no_resync_clears_buffer();
    test_corrupt_payload_byte_crc_reject();

    std::cout << "\n=== LinkMessageCorruptTest Tests Completed Successfully ==="
              << std::endl;
    return 0;
}
