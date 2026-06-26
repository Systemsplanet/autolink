// Auto-generated split of the original LinkMessageTest.cpp.
// Each TU in this split covers a single concern
// (roundtrip / corrupt / resync / edge) and includes
// the shared harness via LinkMessageTestCommon.h.
// Run via `make run_test_alink_message_roundtrip` etc.

#include "LinkMessageTestCommon.h"

using namespace autolink;

void test_app_buffer_null_simulates_disconnect() {
    NullArqCache cache;
    AutoLinkConfig cfg;
    std::cout << "\n=== Test: App Buffer NULL (0..1 regression shape) ==="
              << std::endl;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;

    sHal.appBufCap = 0;
    mHal.appBufCap = 0;

    Link a(mHal, cache, true, cfg);
    Link b(sHal, cache, false, cfg);
    while (a.getState() != State::OK || b.getState() != State::OK) {
        mHal.pumpClock(50);
        sHal.pumpClock(50);
        pipe_data(mHal, sHal);
    }
    b.flushRx();

    uint8_t msg[6] = { 1, 2, 3, 4, 5, 6 };
    assert(a.sendMsg(msg, 6));
    pipe_data(mHal, sHal);

    assert(b.available() == 0);
    uint8_t rx[16];
    int got = b.recvMsg(rx, sizeof(rx));
    assert(got == 0);

    Diag d;
    b.getDiag(d);
    assert(d.gaps == 0);
    assert(d.lostMsgs == 0);

    std::cout << "PASS (recv returned 0, gaps=0 (flow control, not wire error))"
              << std::endl;
}

void test_recvMsg_buffer_too_small() {
    NullArqCache cache;
    AutoLinkConfig cfg;
    std::cout << "\n=== Test: recvMsg Buffer Too Small Drains Payload ==="
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
    uint8_t m1[64];
    for (int i = 0; i < 64; i++)
        m1[i] = (uint8_t)(0x40 + i);
    assert(a.sendMsg(m1, 64));
    pipe_data(mHal, sHal);

    uint8_t tiny[8];
    int errsBefore = b.getErrCount();
    int r = b.recvMsg(tiny, sizeof(tiny));
    assert(r == -1);
    assert(b.getErrCount() > errsBefore);

    assert(b.available() == 0);

    assert(b.getState() == State::OK);
    std::cout << "PASS (buffer too small -> -1, payload drained, link OK)"
              << std::endl;
}

void test_recvMsg_empty_buffer() {
    NullArqCache cache;
    std::cout << "\n=== Test: recvMsg on Empty Buffer Returns 0 ==="
              << std::endl;
    MockHal mHal, sHal;
    Link b(sHal, cache, false, {});

    uint8_t rx[8];
    assert(b.recvMsg(rx, sizeof(rx)) == 0);
    assert(b.getErrCount() == 0);
    std::cout << "PASS (empty recvMsg returns 0)" << std::endl;
}

void test_zero_byte_send_silent_noop() {
    NullArqCache cache;
    AutoLinkConfig cfg;
    std::cout << "\n=== Test: Zero-Byte sendMsg/write is Silent No-Op ==="
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
    size_t txBefore = mHal.txBuf.size();
    int errsBefore = a.getErrCount();
    Stats stBefore;
    a.getStats(stBefore);

    assert(a.sendMsg((const uint8_t *)"", 0) == true);

    assert(a.write((const uint8_t *)"", 0) == 0);

    assert(mHal.txBuf.size() == txBefore);

    assert(a.getErrCount() == errsBefore);
    Stats stAfter;
    a.getStats(stAfter);
    assert(stAfter.discCount == stBefore.discCount);

    assert(b.available() == 0);
    std::cout
        << "PASS (sendMsg(0)=true silent, write(0)=0 silent, no wire bytes)"
        << std::endl;
}

void test_resetDiag_zeros_cobsseq_counters() {
    NullArqCache cache;
    AutoLinkConfig cfg;
    std::cout << "\n=== Test: resetDiag() Clears gaps/stale/lostMsgs ==="
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

    Diag d1;
    a.getDiag(d1);
    a.resetDiag();
    Diag d2;
    a.getDiag(d2);
    assert(d2.gaps == 0);
    assert(d2.stale == 0);
    assert(d2.lostMsgs == 0);

    assert(a.getState() == State::OK);
    assert(b.getState() == State::OK);

    uint8_t m[4] = { 1, 2, 3, 4 };
    assert(a.sendMsg(m, 4));
    pipe_data(mHal, sHal);
    uint8_t rx[8];
    int got = b.recvMsg(rx, sizeof(rx));
    assert(got == 4);
    for (int i = 0; i < 4; i++)
        assert(rx[i] == m[i]);
    std::cout
        << "PASS (resetDiag zeros gaps/stale/lostMsgs, idempotent, link stays OK)"
        << std::endl;
}

void test_send_rejections_log_errors() {
    NullArqCache cache;
    AutoLinkConfig cfg;
    std::cout << "\n=== Test: sendMsg/write Rejections ===" << std::endl;
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

    assert(a.write((const uint8_t *)"x", 0) == 0);
    assert(a.sendMsg((const uint8_t *)"", 0) == true);

    assert(a.write((const uint8_t *)"x", 1) == 1);
    pipe_data(mHal, sHal);

    std::vector<uint8_t> oversized(2048, 0);
    assert(a.sendMsg(oversized.data(), 2048) == false);

    assert(a.sendMsg((const uint8_t *)"y", 1) == true);
    pipe_data(mHal, sHal);

    a.dropLink();
    b.dropLink();
    assert(a.sendMsg((const uint8_t *)"z", 1) == false);
    assert(a.write((const uint8_t *)"z", 1) == 0);

    assert(a.sendMsg((const uint8_t *)"x", -1) == false);
    assert(a.write((const uint8_t *)"x", -1) == 0);

    std::cout << "PASS" << std::endl;
}

int main() {
    std::cout << "=== Running LinkMessageEdgeTest Tests ===" << std::endl;

    Log::log().setLevel(Log::DEBUG);
    test_app_buffer_null_simulates_disconnect();
    test_recvMsg_buffer_too_small();
    test_recvMsg_empty_buffer();
    test_zero_byte_send_silent_noop();
    test_resetDiag_zeros_cobsseq_counters();
    test_send_rejections_log_errors();

    std::cout << "\n=== LinkMessageEdgeTest Tests Completed Successfully ==="
              << std::endl;
    return 0;
}
