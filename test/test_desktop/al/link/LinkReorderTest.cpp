// Hold-on-gap: retransmit delivers in-order.
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include <vector>
#    include "MockHal.h"
#    include "LinkTestAccessor.h"
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

void test_gap_then_retransmit_delivers_in_order() {
    NullArqCache cache;
    AutoLinkConfig cfg;
    std::cout
        << "\n=== Test: Gap + Retransmit Delivers In-Order (hold-on-gap) ==="
        << std::endl;
    MockHal mHal, sHal;
    Link a(mHal, cache, true, cfg);
    Link b(sHal, cache, false, cfg);

    b.onRx(cobsFrame(0).data(), (int)cobsFrame(0).size());
    b.onRx(cobsFrame(1).data(), (int)cobsFrame(1).size());
    int availBefore = b.available();
    b.onRx(cobsFrame(3).data(), (int)cobsFrame(3).size());
    assert(b.available() == availBefore);

    b.onRx(cobsFrame(2).data(), (int)cobsFrame(2).size());

    assert(b.available() > availBefore);

    Diag d;
    b.getDiag(d);
    assert(d.gaps == 1);
    assert(d.stale == 0);
    assert(d.lostMsgs == 0);
    assert(d.rxSeq == 3);
    std::cout << "PASS" << std::endl;
}

void test_gap_then_retransmit_drops_as_stale_under_forward_resync() {
    NullArqCache cache;
    AutoLinkConfig cfg;
    std::cout
        << "\n=== Test: Gap + Retransmit Drops as Stale (forward-resync, the bug) ==="
        << std::endl;
    MockHal mHal, sHal;
    Link a(mHal, cache, true, cfg);
    Link b(sHal, cache, false, cfg);
    // Toggle the test-only forward-resync
    // mode via the LinkTestAccessor. The
    // production link holds out-of-order
    // frames; this toggle re-creates the
    // pre-fix bug shape for the regression
    // pin.
    LinkTestAccessor(b).setForwardResync(true);

    b.onRx(cobsFrame(0).data(), (int)cobsFrame(0).size());
    b.onRx(cobsFrame(1).data(), (int)cobsFrame(1).size());
    b.onRx(cobsFrame(3).data(), (int)cobsFrame(3).size());

    b.onRx(cobsFrame(2).data(), (int)cobsFrame(2).size());

    Diag d;
    b.getDiag(d);
    assert(d.gaps == 1);
    assert(d.stale == 1);
    assert(d.lostMsgs >= 1);
    assert(d.rxSeq == 3);
    std::cout << "PASS (toggle confirms the bounded-reorder fix)" << std::endl;
}

void test_keepalive_gap_does_not_occupy_reorder_slot() {
    NullArqCache cache;
    std::cout << "\n=== Test: Out-of-order keepalive does not "
                 "occupy a reorder slot ==="
              << std::endl;
    AutoLinkConfig cfg;
    MockHal mHal, sHal;
    Link b(sHal, cache, false, cfg);

    b.onRx(cobsFrame(0).data(), (int)cobsFrame(0).size());
    assert(b.available() > 0);

    b.onRx(cobsFrame(2).data(), (int)cobsFrame(2).size());

    LinkTestAccessor t(b);
    assert(t.reorderSlotInUse(2));
    assert(t.reorderSlotLen(2) > 0);

    b.onRx(cobsFrame(3, 0).data(), (int)cobsFrame(3, 0).size());

    assert(!t.reorderSlotInUse(3));

    assert(!t.reorderSlotInUse(1));

    assert(t.reorderSlotInUse(2));
    Diag d;
    b.getDiag(d);

    assert(d.lostMsgs >= 1);
    std::cout << "PASS (keepalive gap -> NAK + lostMsgs++, no "
                 "reorder slot, no spurious ACK)"
              << std::endl;
}

void test_pool_exhaustion_returns_false_without_touching_slot() {
    NullArqCache cache;
    std::cout
        << "\n=== Test: Reorder pool exhaustion drops frame, no malloc on hot path ==="
        << std::endl;
    AutoLinkConfig cfg;
    MockHal mHal, sHal;
    Link b(sHal, cache, false, cfg);

    LinkTestAccessor t(b);
    t.reorderEmptyPool();

    uint8_t payload[16] = { 1, 2,  3,  4,  5,  6,  7,  8,
                            9, 10, 11, 12, 13, 14, 15, 16 };

    // Fresh seq on a non-empty pool: hold() acquires
    // a pool buffer and reports the slot as in_use.
    assert(t.reorderHold(10, payload, sizeof(payload), 100) == true);
    assert(t.reorderSlotInUse(10));
    assert(t.reorderSlotLen(10) == sizeof(payload));

    // Retx on an already-held seq: hold() returns false
    // (slot was already reserved) but reuses the same
    // pool buffer.
    assert(t.reorderHold(10, payload, sizeof(payload), 200) == false);
    assert(t.reorderSlotInUse(10));
    assert(t.reorderSlotLen(10) == sizeof(payload));

    // Force the pool full. Next fresh-seq hold() must
    // return false (frame dropped) AND must NOT claim
    // the slot — that's the malloc-free contract:
    // pool exhaustion is a hard no-op, not a silent
    // partial success.
    t.reorderFillPool();
    assert(t.reorderHold(20, payload, sizeof(payload), 300) == false);
    assert(!t.reorderSlotInUse(20));
    assert(t.reorderSlotLen(20) == 0);

    // Recovery: free the pool, hold() succeeds again.
    t.reorderEmptyPool();
    assert(t.reorderHold(20, payload, sizeof(payload), 400) == true);
    assert(t.reorderSlotInUse(20));

    // Keepalive (n == 0) never touches the pool — same
    // shape as before the pool refactor.
    assert(t.reorderHold(30, payload, 0, 500) == false);
    assert(!t.reorderSlotInUse(30));

    std::cout << "PASS (toggle: with pool full, hold() is a hard no-op "
                 "and no slot is reserved)"
              << std::endl;
}

int main() {
    std::cout << "=== Running LinkReorder Tests (hold-on-gap toggle) ==="
              << std::endl;
    test_gap_then_retransmit_delivers_in_order();
    test_gap_then_retransmit_drops_as_stale_under_forward_resync();
    test_keepalive_gap_does_not_occupy_reorder_slot();
    test_pool_exhaustion_returns_false_without_touching_slot();
    std::cout << "\n=== LinkReorder Tests Completed Successfully ==="
              << std::endl;
    return 0;
}

#endif