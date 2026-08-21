// cobsSeq wraparound + gap classification.
//
// GBN rewrite: the receiver no longer holds out-of-order
// frames in a reorder buffer. A frame that arrives ahead of
// the expected seq is dropped immediately (gaps++, lostMsgs++)
// and a NAK for the expected seq goes out; delivery resumes
// only when that exact seq arrives. Frames dropped this way
// are gone for good from this receiver-only unit test's point
// of view — a real sender's GBN retransmit (LinkFastRetxTest,
// the itest loopback suites) is what actually recovers them.
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include <vector>
#    include "MockHal.h"
#    include "al/util/codec/UtilCrc.h"
#    include "al/util/codec/UtilCobs.h"
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

// GBN: a frame that arrives ahead of the expected seq is
// dropped on the spot — not buffered. gaps++ and lostMsgs++
// both fire; nothing is pushed to the app buffer; a NAK for
// the expected seq goes out; rxSeq does not move.
void test_gap_drops_frame_immediately() {
    NullArqCache cache;
    AutoLinkConfig cfg;
    std::cout << "\n=== Test: Forward Gap Drops the Out-of-Order Frame ==="
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
    assert(d.lostMsgs == 1 && "the out-of-order frame is dropped, not held");
    assert((size_t)b.available() == availBefore);
    assert(d.rxSeq == 1);
    std::cout << "PASS" << std::endl;
}

// The true missing seq arriving in order delivers it and
// resumes at exactly that seq — the earlier out-of-order
// frame (seq 3) stays lost; it was never held.
void test_missing_seq_delivers_in_order_after_gap() {
    NullArqCache cache;
    AutoLinkConfig cfg;
    std::cout << "\n=== Test: Missing Seq Delivers In-Order; the Gap Frame "
                 "Stays Lost ==="
              << std::endl;
    MockHal mHal, sHal;
    Link a(mHal, cache, true, cfg);
    Link b(sHal, cache, false, cfg);

    b.onRx(cobsFrame(0).data(), (int)cobsFrame(0).size());
    b.onRx(cobsFrame(1).data(), (int)cobsFrame(1).size());
    b.onRx(cobsFrame(3).data(), (int)cobsFrame(3).size()); // dropped, lost

    b.onRx(cobsFrame(2).data(), (int)cobsFrame(2).size()); // the real gap-fill

    Diag d;
    b.getDiag(d);
    assert(d.gaps == 1);
    assert(d.stale == 0);
    assert(d.lostMsgs == 1);
    assert(d.rxSeq == 2 &&
           "delivery resumes at the seq that actually arrived in order; "
           "seq 3 was dropped and needs its own retransmit");
    std::cout << "PASS" << std::endl;
}

// A single true gap (one missing seq that never arrives in
// this receiver-only test) drops every subsequent
// out-of-order frame behind it — GBN's known trade for
// dropping the reorder buffer. Each dropped frame is its own
// gaps+lostMsgs event; rxSeq is pinned at the last in-order
// seq until the actual missing frame lands.
void test_gap_drops_everything_behind_it() {
    NullArqCache cache;
    AutoLinkConfig cfg;
    std::cout << "\n=== Test: A Gap Drops Every Frame Behind It Until Filled "
                 "==="
              << std::endl;
    MockHal mHal, sHal;
    Link a(mHal, cache, true, cfg);
    Link b(sHal, cache, false, cfg);

    for (int s = 0; s <= 5; s++)
        b.onRx(cobsFrame(s).data(), (int)cobsFrame(s).size());

    // seq 6 is missing; 8 and 9 arrive ahead of it — both
    // dropped (expected stays 6 the whole time).
    b.onRx(cobsFrame(8).data(), (int)cobsFrame(8).size());
    b.onRx(cobsFrame(9).data(), (int)cobsFrame(9).size());

    Diag d;
    b.getDiag(d);
    assert(d.gaps == 2);
    assert(d.lostMsgs == 2);
    assert(d.rxSeq == 5 && "rxSeq is pinned until seq 6 itself arrives");

    // The real seq 6 lands: delivered, resumes.
    b.onRx(cobsFrame(6).data(), (int)cobsFrame(6).size());
    b.getDiag(d);
    assert(d.rxSeq == 6);
    assert(d.gaps == 2 && "no new gap for the in-order arrival of 6");

    // seq 7 lands next, in order: delivered too.
    b.onRx(cobsFrame(7).data(), (int)cobsFrame(7).size());
    b.getDiag(d);
    assert(d.rxSeq == 7);
    assert(d.lostMsgs == 2 &&
           "8 and 9 stay lost in this receiver-only test — a live sender's "
           "GBN retransmit is what actually recovers them (see "
           "LinkFastRetxTest / the itest loss-sweep suite)");
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
    std::cout << "\n=== Test: Post-Wraparound Gap Drops the Frame ==="
              << std::endl;
    MockHal mHal, sHal;
    Link a(mHal, cache, true, cfg);
    Link b(sHal, cache, false, cfg);

    b.onRx(cobsFrame(253).data(), (int)cobsFrame(253).size());
    b.onRx(cobsFrame(0).data(), (int)cobsFrame(0).size());
    b.onRx(cobsFrame(2).data(), (int)cobsFrame(2).size());

    Diag d;
    b.getDiag(d);
    assert(d.gaps == 1);
    assert(d.lostMsgs == 1);
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

// lostMsgs counts each dropped out-of-order frame immediately
// (not on a later expiry — there is no hold left to expire).
void test_lost_msgs_counts_each_dropped_gap_frame() {
    NullArqCache cache;
    AutoLinkConfig cfg;
    std::cout
        << "\n=== Test: lostMsgs Counts Each Dropped Gap Frame Immediately "
           "==="
        << std::endl;
    MockHal mHal, sHal;
    Link a(mHal, cache, true, cfg);
    Link b(sHal, cache, false, cfg);

    b.onRx(cobsFrame(0).data(), (int)cobsFrame(0).size());
    b.onRx(cobsFrame(4).data(), (int)cobsFrame(4).size()); // gap, dropped
    Diag d;
    b.getDiag(d);
    assert(d.gaps == 1);
    assert(d.lostMsgs == 1);

    b.onRx(cobsFrame(5).data(), (int)cobsFrame(5).size()); // still a gap
    b.onRx(cobsFrame(7).data(), (int)cobsFrame(7).size()); // still a gap
    b.onRx(cobsFrame(9).data(), (int)cobsFrame(9).size()); // still a gap
    b.getDiag(d);
    assert(d.gaps == 4);
    assert(d.lostMsgs == 4);
    assert(d.rxSeq == 0 && "pinned at the last in-order seq — 1 never came");

    b.dropLink();
    b.onRx(cobsFrame(0).data(), (int)cobsFrame(0).size());
    b.getDiag(d);
    // dropLink() resets seq state but not the gaps/lostMsgs
    // counters (that's resetDiag()'s job) — they hold at their
    // pre-drop values. CONTRACT CORRECTION: earlier tail
    // also asserted d.rxSeq == 0 with a comment claiming "the
    // fresh post-drop frame 0 is accepted" — that was never
    // true: dropLink() leaves the link in SWP, where onPayload
    // rejects every frame, and the assertion only passed because
    // the reset's resting rxSeq value happened to be 0. The
    // first-session ASYNC seq-0 seeding changed the resting
    // value and exposed the vacuous pin. The real contract here
    // is: post-drop frames are ignored (SWP) and neither
    // counter moves.
    assert(d.lostMsgs == 4);
    assert(d.gaps == 4);
    std::cout << "PASS" << std::endl;
}

int main() {
    std::cout << "=== Running ALinkCobsSeq Tests (GBN drop-on-gap) ==="
              << std::endl;
    test_first_frame_accepted();
    test_consecutive_frames_advance();
    test_gap_drops_frame_immediately();
    test_missing_seq_delivers_in_order_after_gap();
    test_gap_drops_everything_behind_it();
    test_duplicate_is_stale();
    test_wraparound_continuity();
    test_wraparound_then_gap();
    test_sender_cobsSeq_increments();
    test_drop_resets_cobsSeq();
    test_wire_byte_shift_caught_at_cobsSeq();
    test_gap_stale_counters_accessible();
    test_app_buffer_full_does_not_drop_link();
    test_lost_msgs_counts_each_dropped_gap_frame();
    std::cout << "\n=== ALinkCobsSeq Tests Completed Successfully ==="
              << std::endl;
    return 0;
}

#endif
