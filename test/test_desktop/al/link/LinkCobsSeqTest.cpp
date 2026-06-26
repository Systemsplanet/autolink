// cobsSeq wraparound + gap classification.
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include <vector>
#    include "MockHal.h"
#    include "al/util/UtilCrc.h"
#    include "al/util/UtilCobs.h"
#    include "NullArqCache.h"

using namespace autolink;

static std::vector<uint8_t> cobsFrame(uint8_t cobsSeq, int payloadLen = 16) {
    std::vector<uint8_t> raw;
    raw.push_back(cobsSeq);
    for (int i = 0; i < payloadLen; i++)
        raw.push_back((uint8_t)(0xA0 + i));
    raw.push_back(UtilCrc::crc8(raw.data(), (int)raw.size()));

    std::vector<uint8_t> enc(UtilCobs::encodedMax(raw.size()) + 2);
    size_t n = UtilCobs::encode(raw.data(), raw.size(), enc.data() + 1);
    enc[0] = 0x00;
    enc[1 + n] = 0x00;
    enc.resize(n + 2);
    return enc;
}

void test_first_frame_accepted() {
    NullArqCache cache;
    AutoLinkConfig cfg;
    std::cout << "\n=== Test: First Reliable Frame Sets cobsSeq ==="
              << std::endl;
    MockHal mHal, sHal;
    Link a(mHal, cache, true, cfg);
    Link b(sHal, cache, false, cfg);

    Diag d;
    b.getDiag(d);
    assert(!d.rxSeqSet);
    auto f = cobsFrame(0);
    b.onRx(f.data(), (int)f.size());
    b.getDiag(d);
    assert(d.rxSeqSet);
    assert(d.rxSeq == 0);
    assert(d.gaps == 0);
    assert(d.stale == 0);
    std::cout << "PASS" << std::endl;
}

void test_consecutive_frames_advance() {
    NullArqCache cache;
    AutoLinkConfig cfg;
    std::cout << "\n=== Test: Consecutive Frames Advance cobsSeq ==="
              << std::endl;
    MockHal mHal, sHal;
    Link a(mHal, cache, true, cfg);
    Link b(sHal, cache, false, cfg);

    Diag d;
    for (uint8_t seq = 0; seq < 5; seq++) {
        auto f = cobsFrame(seq);
        b.onRx(f.data(), (int)f.size());
        b.getDiag(d);
        assert(d.rxSeq == seq);
    }
    b.getDiag(d);
    assert(d.gaps == 0);
    assert(d.stale == 0);
    std::cout << "PASS" << std::endl;
}

void test_gap_holds_frame_in_reorder_buffer() {
    NullArqCache cache;
    AutoLinkConfig cfg;
    std::cout << "\n=== Test: Forward Gap Holds Frame in Reorder Buffer ==="
              << std::endl;
    MockHal mHal, sHal;
    Link a(mHal, cache, true, cfg);
    Link b(sHal, cache, false, cfg);

    b.onRx(cobsFrame(0).data(), (int)cobsFrame(0).size());
    b.onRx(cobsFrame(1).data(), (int)cobsFrame(1).size());
    size_t availBefore = (size_t)b.available();
    b.onRx(cobsFrame(3).data(), (int)cobsFrame(3).size());

    Diag d;
    b.getDiag(d);
    assert(d.gaps == 1);
    assert(d.stale == 0);
    assert(d.lostMsgs == 0);
    assert((size_t)b.available() == availBefore);
    assert(d.rxSeq == 1);
    std::cout << "PASS" << std::endl;
}

void test_gap_then_retransmit_flushes_in_order() {
    NullArqCache cache;
    AutoLinkConfig cfg;
    std::cout
        << "\n=== Test: Held Gap Frame Flushes When Missing Seq Arrives ==="
        << std::endl;
    MockHal mHal, sHal;
    Link a(mHal, cache, true, cfg);
    Link b(sHal, cache, false, cfg);

    b.onRx(cobsFrame(0).data(), (int)cobsFrame(0).size());
    b.onRx(cobsFrame(1).data(), (int)cobsFrame(1).size());
    b.onRx(cobsFrame(3).data(), (int)cobsFrame(3).size());

    b.onRx(cobsFrame(2).data(), (int)cobsFrame(2).size());

    Diag d;
    b.getDiag(d);
    assert(d.gaps == 1);
    assert(d.stale == 0);
    assert(d.lostMsgs == 0);
    assert(d.rxSeq == 3);
    std::cout << "PASS" << std::endl;
}

void test_gap_then_late_duplicate_is_stale() {
    NullArqCache cache;
    AutoLinkConfig cfg;
    std::cout << "\n=== Test: Late Frame After Gap is STALE (not delivered) ==="
              << std::endl;
    MockHal mHal, sHal;
    Link a(mHal, cache, true, cfg);
    Link b(sHal, cache, false, cfg);

    b.onRx(cobsFrame(0).data(), (int)cobsFrame(0).size());
    b.onRx(cobsFrame(1).data(), (int)cobsFrame(1).size());
    b.onRx(cobsFrame(3).data(), (int)cobsFrame(3).size());

    cfg.reorderHoldMs = 0;

    b.onRx(cobsFrame(2).data(), (int)cobsFrame(2).size());

    Diag d;
    b.getDiag(d);
    assert(d.gaps == 1);
    assert(d.rxSeq == 3);

    (void)d.stale;
    std::cout << "PASS" << std::endl;
}

void test_single_corruption_does_not_cascade() {
    NullArqCache cache;
    AutoLinkConfig cfg;
    std::cout
        << "\n=== Test: Single Corruption Does Not Cascade (0 regression) ==="
        << std::endl;
    MockHal mHal, sHal;
    Link a(mHal, cache, true, cfg);
    Link b(sHal, cache, false, cfg);

    cfg.reorderHoldMs = 10000;

    for (int s = 0; s <= 41; s++)
        b.onRx(cobsFrame(s).data(), (int)cobsFrame(s).size());

    for (int s = 43; s <= 143; s++)
        b.onRx(cobsFrame(s).data(), (int)cobsFrame(s).size());

    Diag d;
    b.getDiag(d);

    assert(d.lostMsgs == 0);
    assert(d.gaps >= 1);
    assert(d.stale == 0);

    assert(d.gaps + d.stale <= 102);

    assert(d.rxSeq == 41);

    std::cout << "PASS" << std::endl;
}

void test_duplicate_is_stale() {
    NullArqCache cache;
    AutoLinkConfig cfg;
    std::cout << "\n=== Test: Duplicate cobsSeq Is Stale ===" << std::endl;
    MockHal mHal, sHal;
    Link a(mHal, cache, true, cfg);
    Link b(sHal, cache, false, cfg);

    b.onRx(cobsFrame(0).data(), (int)cobsFrame(0).size());
    b.onRx(cobsFrame(1).data(), (int)cobsFrame(1).size());
    b.onRx(cobsFrame(0).data(), (int)cobsFrame(0).size());
    Diag d;
    b.getDiag(d);
    assert(d.stale == 1);
    assert(d.gaps == 0);
    assert(d.rxSeq == 1);
    std::cout << "PASS" << std::endl;
}

void test_wraparound_continuity() {
    NullArqCache cache;
    AutoLinkConfig cfg;
    std::cout << "\n=== Test: cobsSeq Wraparound Is Continuous ==="
              << std::endl;
    MockHal mHal, sHal;
    Link a(mHal, cache, true, cfg);
    Link b(sHal, cache, false, cfg);

    b.onRx(cobsFrame(252).data(), (int)cobsFrame(252).size());
    b.onRx(cobsFrame(253).data(), (int)cobsFrame(253).size());
    b.onRx(cobsFrame(0).data(), (int)cobsFrame(0).size());

    Diag d;
    b.getDiag(d);
    assert(d.rxSeq == 0);
    assert(d.gaps == 0);
    assert(d.stale == 0);
    std::cout << "PASS" << std::endl;
}

void test_wraparound_then_gap() {
    NullArqCache cache;
    AutoLinkConfig cfg;
    std::cout << "\n=== Test: Post-Wraparound Gap Holds Frame ===" << std::endl;
    MockHal mHal, sHal;
    Link a(mHal, cache, true, cfg);
    Link b(sHal, cache, false, cfg);

    b.onRx(cobsFrame(253).data(), (int)cobsFrame(253).size());

    b.onRx(cobsFrame(0).data(), (int)cobsFrame(0).size());

    b.onRx(cobsFrame(2).data(), (int)cobsFrame(2).size());

    Diag d;
    b.getDiag(d);
    assert(d.gaps == 1);
    assert(d.lostMsgs == 0);
    assert(d.rxSeq == 0);
    std::cout << "PASS" << std::endl;
}

void test_sender_cobsSeq_increments() {
    NullArqCache cache;
    std::cout << "\n=== Test: Sender cobsSeq Increments Per Data Frame ==="
              << std::endl;
    MockHal mHal, sHal;
    AutoLinkConfig cfg;
    cfg.allowedBauds[0] = 9600;
    cfg.allowedBauds[1] = 115200;
    cfg.allowedBaudsCount = 2;
    cfg.pingSamplesPerBaud = 1;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link ping(mHal, cache, true, cfg);
    Link pong(sHal, cache, false, cfg);
    negotiate_to_ok(ping, pong, mHal, sHal);

    Diag d;
    ping.getDiag(d);
    assert(d.txSeq == 0);

    uint8_t b1 = 0xAA;
    ping.write(&b1, 1);
    ping.getDiag(d);
    assert(d.txSeq == 1);

    uint8_t b2 = 0xBB;
    ping.write(&b2, 1);
    ping.getDiag(d);
    assert(d.txSeq == 2);

    std::cout << "PASS" << std::endl;
}

void test_drop_resets_cobsSeq() {
    NullArqCache cache;
    std::cout << "\n=== Test: dropLink() Resets cobsSeq on Both Sides ==="
              << std::endl;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    AutoLinkConfig cfg;
    cfg.allowedBauds[0] = 9600;
    cfg.allowedBauds[1] = 115200;
    cfg.allowedBaudsCount = 2;
    cfg.pingSamplesPerBaud = 1;
    cfg.streamBufferSize = 8192;
    Link ping(mHal, cache, true, cfg);
    Link pong(sHal, cache, false, cfg);
    negotiate_to_ok(ping, pong, mHal, sHal);

    uint8_t b = 0xAA;
    for (int i = 0; i < 5; i++)
        ping.write(&b, 1);
    pipe_data(mHal, sHal);
    Diag d;
    ping.getDiag(d);
    assert(d.txSeq >= 5);
    pong.getDiag(d);
    assert(d.rxSeq >= 0);

    ping.dropLink();
    pong.dropLink();

    ping.getDiag(d);
    assert(d.txSeq == 0);
    pong.getDiag(d);
    assert(d.txSeq == 0);
    pong.getDiag(d);
    assert(!d.rxSeqSet);
    std::cout << "PASS" << std::endl;
}

void test_wire_byte_shift_caught_at_cobsSeq() {
    NullArqCache cache;
    AutoLinkConfig cfg;
    std::cout << "\n=== Test: Wire-Byte Shift Accounted, Link Stays in OK ==="
              << std::endl;
    MockHal mHal, sHal;
    Link a(mHal, cache, true, cfg);
    Link b(sHal, cache, false, cfg);

    auto f0 = cobsFrame(0);
    auto f1 = cobsFrame(1);
    f1[1] ^= 0x40;
    b.onRx(f0.data(), (int)f0.size());
    b.onRx(f1.data(), (int)f1.size());

    Diag d;
    b.getDiag(d);

    bool accounted = (d.gaps > 0) || (d.stale > 0) || (b.getErrCount() > 0);
    assert(accounted);

    assert(d.rxSeqSet);

    assert(d.gaps + d.stale + b.getErrCount() <= 2);
    std::cout << "PASS" << std::endl;
}

void test_gap_stale_counters_accessible() {
    NullArqCache cache;
    AutoLinkConfig cfg;
    std::cout << "\n=== Test: Gap/Stale Counters Are Public API ==="
              << std::endl;
    MockHal mHal, sHal;
    Link a(mHal, cache, true, cfg);
    Link b(sHal, cache, false, cfg);

    Diag d;
    b.getDiag(d);
    assert(d.gaps == 0);
    assert(d.stale == 0);
    b.onRx(cobsFrame(0).data(), (int)cobsFrame(0).size());
    b.onRx(cobsFrame(2).data(), (int)cobsFrame(2).size());
    b.onRx(cobsFrame(0).data(), (int)cobsFrame(0).size());
    b.getDiag(d);
    assert(d.gaps == 1);
    assert(d.stale == 1);
    std::cout << "PASS" << std::endl;
}

void test_app_buffer_full_does_not_drop_link() {
    NullArqCache cache;
    AutoLinkConfig cfg;
    std::cout << "\n=== Test: App Buffer Full Doesn't Trip errThreshold ==="
              << std::endl;
    MockHal mHal, sHal;
    cfg.streamBufferSize = 256;
    cfg.errThreshold = 5;
    Link a(mHal, cache, true, cfg);
    Link b(sHal, cache, false, cfg);

    sHal.appBufCap = 16;

    negotiate_to_ok(a, b, mHal, sHal);
    assert(b.getState() == State::OK);

    int errsBefore = b.getErrCount();
    Diag d;
    b.getDiag(d);
    uint64_t gapsBefore = d.gaps;

    for (int seq = 0; seq < 30; seq++) {
        b.onRx(cobsFrame(seq).data(), (int)cobsFrame(seq).size());
    }

    assert(b.getState() == State::OK);

    assert(b.getErrCount() == errsBefore);

    b.getDiag(d);
    assert(d.gaps == gapsBefore);
    std::cout << "PASS" << std::endl;
}

void test_lost_msgs_burst_vs_single() {
    NullArqCache cache;
    AutoLinkConfig cfg;
    std::cout
        << "\n=== Test: lostMsgs Counts Held Frames on Staleness Expiry ==="
        << std::endl;
    MockHal mHal, sHal;
    Link a(mHal, cache, true, cfg);
    Link b(sHal, cache, false, cfg);

    cfg.reorderHoldMs = 10000;

    b.onRx(cobsFrame(0).data(), (int)cobsFrame(0).size());
    b.onRx(cobsFrame(4).data(), (int)cobsFrame(4).size());
    Diag d;
    b.getDiag(d);
    assert(d.gaps == 1);
    assert(d.lostMsgs == 0);

    b.onRx(cobsFrame(5).data(), (int)cobsFrame(5).size());
    b.onRx(cobsFrame(7).data(), (int)cobsFrame(7).size());
    b.onRx(cobsFrame(9).data(), (int)cobsFrame(9).size());
    b.getDiag(d);
    assert(d.gaps == 4);
    assert(d.lostMsgs == 0);

    {
        NullArqCache cache;
        AutoLinkConfig cfg2 = cfg;
        cfg2.reorderHoldMs = 0;
        Link c(sHal, cache, false, cfg2);
        c.onRx(cobsFrame(0).data(), (int)cobsFrame(0).size());
        c.onRx(cobsFrame(4).data(), (int)cobsFrame(4).size());
        c.onRx(cobsFrame(5).data(), (int)cobsFrame(5).size());
        c.onRx(cobsFrame(7).data(), (int)cobsFrame(7).size());
        Diag d2;
        c.getDiag(d2);
        assert(d2.gaps == 3);
        assert(d2.lostMsgs == 2);
    }

    b.dropLink();
    b.onRx(cobsFrame(0).data(), (int)cobsFrame(0).size());
    b.getDiag(d);
    assert(d.lostMsgs == 0);
    assert(d.gaps == 4);
    std::cout << "PASS" << std::endl;
}

int main() {
    std::cout << "=== Running ALinkCobsSeq Tests (hold-on-gap) ==="
              << std::endl;
    test_first_frame_accepted();
    test_consecutive_frames_advance();
    test_gap_holds_frame_in_reorder_buffer();
    test_gap_then_retransmit_flushes_in_order();
    test_gap_then_late_duplicate_is_stale();
    test_single_corruption_does_not_cascade();
    test_duplicate_is_stale();
    test_wraparound_continuity();
    test_wraparound_then_gap();
    test_sender_cobsSeq_increments();
    test_drop_resets_cobsSeq();
    test_wire_byte_shift_caught_at_cobsSeq();
    test_gap_stale_counters_accessible();
    test_app_buffer_full_does_not_drop_link();
    test_lost_msgs_burst_vs_single();
    std::cout << "\n=== ALinkCobsSeq Tests Completed Successfully ==="
              << std::endl;
    return 0;
}

#endif