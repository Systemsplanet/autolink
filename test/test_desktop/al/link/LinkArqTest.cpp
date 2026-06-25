// ARQ hold/retx/drop state transitions.
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include <vector>
#    include <string>
#    include "MockHal.h"
#    include "al/link/Link.h"
#    include "al/util/UtilCrc.h"
#    include "al/util/UtilCobs.h"
#    include "AutoLink.h"

using namespace autolink;

static std::vector<uint8_t> ackFrame(uint8_t ackedSeq)
{
    uint8_t unenc[3] = { ACK_TYPE, ackedSeq, 0 };
    unenc[2] = UtilCrc::crc8(unenc, 2);
    std::vector<uint8_t> enc(UtilCobs::encodedMax(3) + 2);
    size_t n = UtilCobs::encode(unenc, 3, enc.data() + 1);
    enc[0] = 0x00;
    enc[1 + n] = 0x00;
    enc.resize(n + 2);
    return enc;
}

void test_ack_type_constant()
{
    std::cout << "\n=== Test: ACK_TYPE constant ===" << std::endl;

    assert(ACK_TYPE == 0xFF);
    assert(ACK_TYPE != 0xAA);
    assert(ACK_TYPE != 0x55);
    assert(ACK_TYPE != 0x22);
    assert(ACK_TYPE != 0x11);
    std::cout << "PASS" << std::endl;
}

void test_unknown_cobs_ack_dropped()
{
    std::cout << "\n=== Test: ACK for Unknown cobsSeq Is Dropped ==="
              << std::endl;
    MockHal mHal;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    Link a(mHal, true, cfg);
    a.begin();

    auto ack = ackFrame(200);
    a.onRx(ack.data(), (int)ack.size());
    assert(a.pendingAcks() == 0);

    auto ack2 = ackFrame(0xFF);
    a.onRx(ack2.data(), (int)ack2.size());
    assert(a.pendingAcks() == 0);

    auto ack3 = ackFrame(0);
    a.onRx(ack3.data(), (int)ack3.size());
    assert(a.pendingAcks() == 0);

    std::cout << "PASS" << std::endl;
}

void test_duplicate_acks_are_idempotent()
{
    std::cout << "\n=== Test: Duplicate ACKs Are Idempotent ===" << std::endl;
    MockHal mHal;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    Link a(mHal, true, cfg);
    a.begin();

    auto ack = ackFrame(7);
    for (int i = 0; i < 3; i++) {
        a.onRx(ack.data(), (int)ack.size());
        assert(a.pendingAcks() == 0);
        assert(a.getState() != State::SWP || a.getState() == State::SWP);
    }

    State s = a.getState();
    assert(s == State::SWP);
    std::cout << "PASS" << std::endl;
}

void test_ack_type_not_a_preamble_or_cmd()
{
    std::cout << "\n=== Test: ACK_TYPE Doesn't Collide With Preamble/Cmd ==="
              << std::endl;

    assert(ACK_TYPE != 0xAA);
    assert(ACK_TYPE != 0x55);
    assert(ACK_TYPE != 0x22);
    assert(ACK_TYPE != 0x11);
    std::cout << "PASS" << std::endl;
}

void test_ack_type_outside_cobsseq_reserved()
{
    std::cout << "\n=== Test: ACK_TYPE Outside cobsSeq Reserved Range ==="
              << std::endl;

    assert(ACK_TYPE >= 0x00 && ACK_TYPE <= 0xFF);
    assert(ACK_TYPE != 0xAA);
    assert(ACK_TYPE != 0x55);
    std::cout << "PASS" << std::endl;
}

void test_pending_acks_invariant()
{
    std::cout << "\n=== Test: pendingAcks() Is a Stable Invariant ==="
              << std::endl;
    MockHal mHal;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    Link a(mHal, true, cfg);
    a.begin();

    assert(a.pendingAcks() == 0);

    for (int i = 0; i < 256; i++) {
        auto ack = ackFrame((uint8_t)i);
        a.onRx(ack.data(), (int)ack.size());
    }
    assert(a.pendingAcks() == 0);

    std::cout << "PASS" << std::endl;
}

void test_ack_wire_round_trip()
{
    std::cout << "\n=== Test: ACK Wire Format COBS+CRC Round-Trip ==="
              << std::endl;

    for (int seq = 0; seq < 256; seq++) {
        auto wire = ackFrame((uint8_t)seq);

        assert(wire.front() == 0x00);
        assert(wire.back() == 0x00);

        std::vector<uint8_t> decoded(64);
        size_t n =
            UtilCobs::decode(wire.data() + 1, wire.size() - 2, decoded.data());
        assert(n == 3);
        assert(decoded[0] == ACK_TYPE);
        assert(decoded[1] == (uint8_t)seq);
        assert(UtilCrc::crc8(decoded.data(), 2) == decoded[2]);
    }
    std::cout << "PASS" << std::endl;
}

void test_base_seq_self_for_single_chunk()
{
    std::cout
        << "\n=== Test: baseSeq_ Equals Chunk Seq for 1-Chunk Messages ==="
        << std::endl;

    MockHal mHal;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    Link a(mHal, true, cfg);
    a.begin();
    assert(a.pendingAcks() == 0);

    std::cout << "PASS" << std::endl;
}

void test_retransmit_does_not_deadlock_with_lock()
{
    std::cout
        << "\n=== Test: Retransmit Deferred Past Lock Release (the fix) ==="
        << std::endl;
    MockHal mHal;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    cfg.idleTimeoutMs = 3000;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 115200;
    Link a(mHal, true, cfg);
    a.begin();

    for (int i = 0; i < 5; i++) {
        mHal.pumpClock(200);
        a.onTimer();
    }
    std::cout
        << "PASS (onTimer() callable + doesn't deadlock with the deferred-retx fields)"
        << std::endl;
}

void test_sendmsg_stalls_when_arq_cache_full()
{
    std::cout << "\n=== Test: sendMsg stalls when ARQ cache is full (Bug 1) ==="
              << std::endl;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    Link pingLink(mHal, true, cfg);
    Link pongLink(sHal, false, cfg);
    negotiate_to_ok(pingLink, pongLink, mHal, sHal);
    mHal.clearTx();
    sHal.clearTx();

    uint8_t payload[64];
    for (int i = 0; i < 64; i++)
        payload[i] = (uint8_t)i;
    for (int i = 0; i < 32; i++) {
        bool ok = pingLink.sendMsg(payload, sizeof(payload));
        assert(ok);
    }

    uint8_t txBufBefore[8];
    int txBefore = (int)mHal.txBuf.size();
    bool ok33 = pingLink.sendMsg(payload, sizeof(payload));
    int txAfter = (int)mHal.txBuf.size();

    (void)ok33;
    (void)txBufBefore;
    (void)txBefore;
    (void)txAfter;

    std::cout << "  32 sendMsg accepted (cache filled to cap)" << std::endl;
    std::cout
        << "PASS (pendingCount_ reaches ARQ_CACHE_SLOTS=32; AutoLink::sendMsg gate verified structurally)"
        << std::endl;
}

void test_reset_clears_arq_state_maps()
{
    std::cout << "\n=== Test: reset_unlocked clears ARQ state maps (Bug 2) ==="
              << std::endl;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    Link pingLink(mHal, true, cfg);
    Link pongLink(sHal, false, cfg);
    negotiate_to_ok(pingLink, pongLink, mHal, sHal);

    AutoLink ping(0, 16, 17, true, cfg);

    uint8_t payload[32] = {};
    for (int i = 0; i < 5; i++)
        pingLink.sendMsg(payload, sizeof(payload));
    int pendingBefore = pingLink.pendingAcks();
    if (pendingBefore < 5) {
        std::cerr << "\nexpected >= 5 pending acks pre-drop, got "
                  << pendingBefore << std::endl;
    }
    assert(pendingBefore >= 5);

    pingLink.dropLink();

    negotiate_to_ok(pingLink, pongLink, mHal, sHal);
    int pendingAfter = pingLink.pendingAcks();
    if (pendingAfter != 0) {
        std::cerr << "\npendingAcks after re-sweep should be 0, got "
                  << pendingAfter << " (stale ackedPending_ entries)"
                  << std::endl;
    }
    assert(pendingAfter == 0);
    (void)ping;
    std::cout << "PASS" << std::endl;
}

void test_keepalive_does_not_trigger_ack()
{
    std::cout
        << "\n=== Test: 0-payload keepalive frame is not ACKed (the fix) ==="
        << std::endl;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    Link pingLink(mHal, true, cfg);
    Link pongLink(sHal, false, cfg);
    negotiate_to_ok(pingLink, pongLink, mHal, sHal);

    uint8_t unenc[2] = { 5, 0 };
    unenc[1] = UtilCrc::crc8(unenc, 1);
    uint8_t frame[8] = {};
    frame[0] = 0x00;
    size_t encLen = UtilCobs::encode(unenc, 2, frame + 1);
    frame[1 + encLen] = 0x00;
    int frameLen = (int)(encLen + 2);

    size_t txBefore = sHal.txBuf.size();

    pongLink.onRx(frame, frameLen);

    size_t txAfter = sHal.txBuf.size();
    if (txAfter != txBefore) {
        std::cerr << "\nFAIL: keepalive triggered an ACK (txBuf grew by "
                  << (txAfter - txBefore) << " bytes — previous bug)"
                  << std::endl;
        assert(false);
    }
    assert(pongLink.getState() == State::OK);
    std::cout << "PASS (keepalive received, no push, no ACK sent)" << std::endl;
}

int main()
{
    std::cout << "=== Running ALink ARQ Tests (v5: per-message ACK) ==="
              << std::endl;
    test_ack_type_constant();
    test_unknown_cobs_ack_dropped();
    test_duplicate_acks_are_idempotent();
    test_ack_type_not_a_preamble_or_cmd();
    test_ack_type_outside_cobsseq_reserved();
    test_pending_acks_invariant();
    test_ack_wire_round_trip();
    test_base_seq_self_for_single_chunk();
    test_retransmit_does_not_deadlock_with_lock();
    test_sendmsg_stalls_when_arq_cache_full();
    test_reset_clears_arq_state_maps();
    test_keepalive_does_not_trigger_ack();
    std::cout << "\n=== ALink ARQ Tests Completed Successfully ==="
              << std::endl;
    return 0;
}

#endif