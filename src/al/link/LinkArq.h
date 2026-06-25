// Per-cobsSeq ARQ state and retransmit policy.
// Owns the 256-slot ack/retx tables; Link holds
// the I/O surface (tx, nowMs, cache hooks) and
// calls into this class for state transitions.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace autolink {
class Link;

class LinkArq {
public:
    LinkArq() = default;

    // Caller holds the link lock.
    void clearAll();

    // Mark seq as in-flight (sent but not ACKed).
    void onSent(uint8_t seq, uint8_t baseSeq, uint32_t nowMs);

    // Mark seq as acknowledged.
    void onAcked(uint8_t seq);

    // Mark seq for immediate retransmit on the
    // next timer tick (NAK-driven fast retx).
    void onNaked(uint8_t missingCobsSeq, uint32_t nowMs);

    // Set/check pending state.
    void setPending(uint8_t seq, bool v = true) { ackedPending_[seq] = v; }
    bool isPending(uint8_t seq) const { return ackedPending_[seq]; }

    // Sync-mode wait helper: caller has the link
    // lock. waitForAck drops it, blocks until the
    // peer ACKs `seq` or `timeoutMs` elapses, then
    // takes the lock again before returning.
    // `timeoutMs` is the caller's RTO.
    // Returns true if ACKed in time, false if the
    // wait timed out (slot is cleared either way).
    bool waitForAck(Link &l, uint8_t seq, uint32_t timeoutMs);

    // Pop one stale slot whose retransmit budget
    // has elapsed. Caller passes nowMs to keep
    // this class hardware-free.
    int popRetransmitSlot(uint32_t nowMs, uint32_t ackRtoMs);

    // Stats / inspection.
    int pendingCount() const;
    bool isAcked(uint8_t cobsSeq) const { return !ackedPending_[cobsSeq]; }

    // Sweep decision for the ASYNC timer loop:
    // Hold / Retx / Drop. The link's ARQ timer
    // tick walks all 256 slots and asks this for
    // the action each one should take.
    enum class Action { Hold, Retx, Drop };

    Action decideSlot(uint8_t seq, uint32_t nowMs, uint32_t ackRtoMs,
                      uint8_t maxRetx) const;

    // Apply a Retx decision: bump counters, return
    // the seq for the retx callback to fetch from
    // the ARQ cache.
    uint8_t applyRetx(uint8_t seq, uint32_t nowMs);

    // Drop bookkeeping for the link (clears slot).
    void clearSlot(uint8_t seq);

private:
    bool ackedPending_[256] = {};
    uint8_t retxCount_[256] = {};
    uint32_t sentAtMs_[256] = {};
    uint8_t baseSeq_[256] = {};
};

} // namespace autolink