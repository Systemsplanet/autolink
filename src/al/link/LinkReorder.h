// Out-of-order frame hold buffer + flush logic.
// Owns the 256-slot reorder table; Link owns the
// I/O surface (pushAppBuf, sendAckFrame, rxSeq).
// Pool-backed so the ISR-adjacent hot path never
// touches malloc — matches the ArqCache pattern.
#pragma once
#include "al/link/LinkContext.h"
#include <stdint.h>
#include <stddef.h>

namespace autolink {

class LinkReorder {
public:
    // 1:1 with the cobsSeq space.
    static constexpr int SLOTS = 256;
    // Each held frame is at most MAX_CHUNK
    // bytes (see Link.h). Mirrored here so
    // the header is self-contained; the .cpp
    // static_asserts the value matches.
    static constexpr int REORDER_POOL_BUF_MAX = 250;
    // Enough headroom for a full retx
    // window plus the gap before it,
    // same reasoning as ArqCache::POOL_SIZE
    // vs WINDOW. Sized for the worst-case
    // contiguous out-of-order burst that
    // can fit before rxSeq wraps (≈ SLOTS/2).
    // 128 × 250B = 32 KB on ESP32.
    static constexpr int REORDER_POOL_SIZE = 128;

    static_assert(REORDER_POOL_SIZE >= 1,
                  "REORDER_POOL_SIZE must hold at least one frame");

    LinkReorder() = default;
    ~LinkReorder();

    // Caller holds the link lock.
    void clearAll();

    // Drop slots older than cfg.reorderHoldMs.
    // Returns the number of slots dropped.
    int dropExpired(uint32_t nowMs, int holdMs);

    // Flush whatever is contiguous in the held
    // buffer. Returns the number of frames
    // delivered. Link handles the errs-clear
    // post-condition once it sees >0.
    int flushContiguous(LinkContext &ctx, uint32_t nowMs);

    // Hold an out-of-order frame in slot `seq`.
    // `len == 0` is a keepalive gap: caller has
    // already incremented lostMsgs and we leave
    // the slot un-reserved.
    // Returns true if the slot was newly reserved,
    // false if the slot was already in use (caller
    // has already bumped lostMsgs). Pool exhaustion
    // is logged and surfaces as false so the caller
    // drops the slot and bumps lostMsgs, matching
    // the prior malloc-failure behaviour without
    // fragmenting the heap.
    bool hold(uint8_t seq, const uint8_t *b, int n, uint32_t nowMs);

    // Test inspection hooks used by LinkReorderTest.
    bool slotInUse(uint8_t seq) const { return slots_[seq].in_use; }
    uint16_t slotLen(uint8_t seq) const { return slots_[seq].len; }

    // Test-only fixtures used by LinkReorderTest to
    // pin pool-exhaustion behaviour without driving
    // the link layer.
    void testFillPool();
    void testEmptyPool();

#ifdef AUTOLINK_HOST_TEST
    void assertInvariants() const;
#else
    void assertInvariants() const {}
#endif

private:
    struct Slot {
        uint16_t len = 0;
        uint32_t heldAtMs = 0;
        uint8_t poolIdx = 0xFF;
        bool in_use = false;
    };
    Slot slots_[SLOTS] = {};
    uint8_t pool_[REORDER_POOL_SIZE][REORDER_POOL_BUF_MAX] = {};
    bool poolUsed_[REORDER_POOL_SIZE] = {};
};

} // namespace autolink