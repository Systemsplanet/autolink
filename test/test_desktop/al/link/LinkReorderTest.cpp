// Hold-on-gap: retransmit delivers in-order.
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include <vector>
#    include "MockHal.h"
#    include "LinkTestAccessor.h"
#    include "al/util/UtilCrc.h"
#    include "al/util/UtilCobs.h"

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
    AutoLinkConfig cfg;
    cfg._test_forwardResync = false;
    std::cout
        << "\n=== Test: Gap + Retransmit Delivers In-Order (hold-on-gap) ==="
        << std::endl;
    MockHal mHal, sHal;
    Link a(mHal, true, cfg);
    Link b(sHal, false, cfg);

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
    AutoLinkConfig cfg;
    cfg._test_forwardResync = true;
    std::cout
        << "\n=== Test: Gap + Retransmit Drops as Stale (forward-resync, the bug) ==="
        << std::endl;
    MockHal mHal, sHal;
    Link a(mHal, true, cfg);
    Link b(sHal, false, cfg);

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
    std::cout << "\n=== Test: Out-of-order keepalive does not "
                 "occupy a reorder slot ==="
              << std::endl;
    AutoLinkConfig cfg;
    cfg._test_forwardResync = false;
    MockHal mHal, sHal;
    Link b(sHal, false, cfg);

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

int main() {
    std::cout << "=== Running LinkReorder Tests (hold-on-gap toggle) ==="
              << std::endl;
    test_gap_then_retransmit_delivers_in_order();
    test_gap_then_retransmit_drops_as_stale_under_forward_resync();
    test_keepalive_gap_does_not_occupy_reorder_slot();
    std::cout << "\n=== LinkReorder Tests Completed Successfully ==="
              << std::endl;
    return 0;
}

#endif