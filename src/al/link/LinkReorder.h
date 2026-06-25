// Out-of-order frame hold buffer + flush logic.
// Owns the 256-slot reorder table; Link owns the
// I/O surface (pushAppBuf, sendAckFrame, rxSeq).
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace autolink {
class Link;

class LinkReorder {
public:
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
    int flushContiguous(Link &l, uint32_t nowMs);

    // Hold an out-of-order frame in slot `seq`.
    // `len == 0` is a keepalive gap: caller has
    // already incremented lostMsgs and we leave
    // the slot un-reserved.
    // Returns true if the slot was newly reserved,
    // false if the slot was already in use (caller
    // has already bumped lostMsgs).
    bool hold(uint8_t seq, const uint8_t *b, int n, uint32_t nowMs);

    // Test inspection hooks used by LinkReorderTest.
    bool slotInUse(uint8_t seq) const { return slots_[seq].in_use; }
    uint16_t slotLen(uint8_t seq) const { return slots_[seq].len; }

private:
    struct Slot {
        uint8_t *buf = nullptr;
        uint16_t len = 0;
        uint32_t heldAtMs = 0;
        bool in_use = false;
    };
    Slot slots_[256] = {};
};

} // namespace autolink