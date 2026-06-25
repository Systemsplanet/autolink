// Test-only shim for Link. The test_*
// methods (test_markAckedPending,
// test_sendMsgBegin, test_sendMsgStillWaiting,
// syncAckTimeoutMsForTest, test_reorderSlotInUse,
// test_reorderSlotLen, arqCacheForTest) are
// private on Link; this shim is a `friend`
// and is the only path a host test reaches
// them through.
//
// Build only under -DAUTOLINK_HOST_TEST. Do
// NOT include from src/ or include/ headers.
// The shim has no Arduino-side equivalent:
// production sketches must not poke at the
// internals these methods expose.
#pragma once
#ifndef AUTOLINK_HOST_TEST
#    error "Build with -DAUTOLINK_HOST_TEST (see test/test_desktop/Makefile)"
#endif

#include "al/link/Link.h"
#include <cstdint>

namespace autolink {

class LinkTestAccessor {
public:
    explicit LinkTestAccessor(Link &l) : l_(l) {}
    explicit LinkTestAccessor(const Link &l) : l_(const_cast<Link &>(l)) {}

    // Mark seq as ack-pending in the ARQ table.
    // Used by facade tests to drive the free-slot
    // path without a real receiver ACK round-trip.
    void markAckedPending(uint8_t s) { l_.arq_.setPending(s, true); }

    // SYNC-mode host test hooks. Split
    // sendMsg() into a non-blocking "begin"
    // (sends the frame and remembers the seq)
    // and a "still waiting" check. The test
    // pumps time between calls so the wire
    // can deliver the ACK.
    bool sendMsgBegin(const uint8_t *b, int len) {
        return l_.test_sendMsgBegin(b, len);
    }
    bool sendMsgStillWaiting() { return l_.test_sendMsgStillWaiting(); }
    int syncAckTimeoutMs() const { return l_.cfg.syncAckTimeoutMs; }

    // Reorder buffer inspection.
    bool reorderSlotInUse(uint8_t cobsSeq) const {
        return l_.reorder_.slotInUse(cobsSeq);
    }
    uint16_t reorderSlotLen(uint8_t cobsSeq) const {
        return l_.reorder_.slotLen(cobsSeq);
    }
    // Reorder buffer direct drive used by
    // LinkReorderTest::test_pool_exhaustion.
    bool reorderHold(uint8_t seq, const uint8_t *b, int n, uint32_t nowMs) {
        return l_.reorder_.hold(seq, b, n, nowMs);
    }
    void reorderFillPool() { l_.reorder_.testFillPool(); }
    void reorderEmptyPool() { l_.reorder_.testEmptyPool(); }

    // ARQ cache reference (lifetime: cache
    // outlives the link, enforced by the
    // Link ctor taking IArqCache&; AutoLink
    // owns the cache by value and constructs
    // the link first, so dtor order is safe).
    IArqCache &arqCache() const { return l_.arqCache_; }

    // Direct LinkArq handle for tests that
    // drive waitForAck / clearAll by hand
    // (e.g. the ABA-hazard regression in
    // LinkArqTest).
    LinkArq &arq() { return l_.arq_; }

    // Pass-through to the link itself for
    // fixtures that already hold a Link&.
    Link &link() { return l_; }
    const Link &link() const { return l_; }

private:
    Link &l_;
};

} // namespace autolink
