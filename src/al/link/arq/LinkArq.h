
#pragma once
#include "al/link/arq/ArqCache.h"
#include "al/link/IHalCtx.h"
#include <stdint.h>
#include <stddef.h>

namespace autolink {

// LinkArq is the ARQ protocol bookkeeping for the GBN
// sender window. All five per-seq fields (ackedPending_,
// retxCount_, sentAtMs_, baseSeq_, bytesRecvd_) are budget-depth
// (B = ARQ_CHUNK_BUDGET = 2*W, W = AUTOLINK_ARQ_PIPELINE_WINDOW),
// indexed via budgetIdx(seq) = seq % B — a FIXED function of seq
// alone, not gbnBase_-relative. At most W chunks are ever in
// flight and B = 2*W, so a slot's previous occupant is always
// ACKed before a new send can reuse the same seq%B index (see
// onSent()'s comment for the derivation) — mirroring
// ArqCache::POOL_SIZE's identical 2*W sizing rationale.
//
// idxOf(seq) (relative to gbnBase_) still exists and backs
// nothing but a single guard in decideSlot(): "is this seq
// currently within the tracked pending window at all". Do NOT
// use it to index a field that's read more than once per
// gbnBase_ advance, or read after the seq is ACKed — a seq's
// idxOf-relative slot drifts every time gbnBase_ moves, so a
// write at send-time and a read after a LATER, unrelated
// gbnBase_ advance can land on different physical slots. This
// bit two fields the hard way before landing on budgetIdx for
// all five:
//   - ackedPending_/retxCount_ first shipped idxOf-backed
//     (window-depth). pendingCount() reads the whole window, and
//     a burst-send of several chunks followed by staggered
//     (non-lockstep) cumulative ACKs orphaned stale `true` flags
//     at their original send-time offsets once gbnBase_ moved
//     past them — a 22-chunk burst showed pendingCount()==16
//     with only 1 chunk actually outstanding.
//   - sentAtMs_ shipped idxOf-backed even longer, and got away
//     with it because its one reader, decideSlot(), is always
//     called with seq == gbnBase_ (see sweepRetx_unlocked in
//     LinkTimers.cpp), so it only ever touches idxOf(gbnBase_)
//     == 0 — a single fixed offset, not a whole-window read. But
//     nothing re-stamps sentAtMs_[0] when a NEW seq becomes the
//     base after a multi-seq cumulative ACK: it can hold a
//     different, earlier seq's original send timestamp (or the
//     zero-initialized default, if index 0 was never written by
//     the new base's own onSent). LinkArqTest's
//     test_sentatms_survives_burst_send_and_gbnbase_advance
//     reproduces this: decideSlot() fired Retx 100 ms after a
//     chunk's real send because it read a stale/zero slot
//     instead of that chunk's own timestamp.
//
// Widening sentAtMs_ from window-depth to budget-depth to fix
// this costs back ~128 B/Link of the shrink below — a correct
// RTO clock is worth more than the last quarter of the save.
//
// Net save vs. the pre-shrink (full COBS-seq-space, 256-deep)
// shape: ~1728 B/Link across all five fields combined.
//
// Fixed alongside the shrink: onAcked() previously zeroed
// baseSeq_[seq] immediately on ACK, so bytesForMessage() only
// produced a correct sum by coincidence — for the first message
// sent after a link reset, whose baseSeq is 0 and thus matches
// the freshly-zeroed sentinel every other never-used slot also
// carried from clearAll()'s memset. A second message in the same
// session already got the wrong answer (its chunks' baseSeq_
// entries were wiped back to 0 the moment each chunk was ACKed).
// onAcked() now leaves baseSeq_[budgetIdx(seq)] alone.
// LinkBaseSeqTrackingTest Pin 6 pins the two-message case.
class LinkArq {
public:
    static constexpr int COBS_SEQ_SPACE = 256;

    LinkArq() { clearAll(); }

    void clearAll();

    uint32_t generation() const { return generation_; }

    uint8_t gbnBase() const { return gbnBase_; }
    void setGbnBase(uint8_t seq) { gbnBase_ = seq; }
    bool gbnActive() const { return gbnActive_; }
    void setGbnActive(bool v) { gbnActive_ = v; }

    int idxOf(uint8_t seq) const {
        int d = (int)seq - (int)gbnBase_;
        if (d < 0)
            d += COBS_SEQ_SPACE;
        return d < (int)AUTOLINK_ARQ_PIPELINE_WINDOW ? d : -1;
    }
    // Fixed physical slot for all five per-seq fields — a pure
    // function of seq, NOT relative to gbnBase_. See the class
    // comment for why this (not idxOf) backs them.
    static int budgetIdx(uint8_t seq) { return (int)seq % ARQ_CHUNK_BUDGET; }
    void seedGbn(uint8_t seq, bool active = true) {
        clearAll();
        gbnBase_ = seq;
        gbnActive_ = active;
    }

    void onSent(uint8_t seq, uint8_t baseSeq, uint32_t nowMs);
    void onAcked(uint8_t seq, uint16_t bytesRecvd = 0);
    uint16_t bytesFor(uint8_t seq) const;
    uint16_t bytesForMessage(uint8_t baseSeq) const {
        uint16_t total = 0;
        for (int i = 0; i < ARQ_CHUNK_BUDGET; i++)
            if (baseSeq_[i] == baseSeq)
                total += bytesRecvd_[i];
        return total;
    }
    // The walk above covers the budget-depth (ARQ_CHUNK_BUDGET)
    // baseSeq_/bytesRecvd_ ring. A message's chunks are ACKed by
    // the time a caller wants this sum — budgetIdx keeps them
    // resolvable past the ACK; see the class comment.
    void onNaked(uint8_t missingCobsSeq, uint32_t nowMs);
    void setPending(uint8_t seq, bool v = true);
    bool isPending(uint8_t seq) const;
    bool waitForAck(IHalCtx &ctx, uint8_t seq, uint32_t timeoutMs);
    int pendingCount() const;
    bool isAcked(uint8_t cobsSeq) const { return !isPending(cobsSeq); }

    enum class Action { Hold, Retx, Drop };
    Action decideSlot(uint8_t seq, uint32_t nowMs, uint32_t ackRtoMs,
                      uint8_t maxRetx) const;
    uint8_t applyRetx(uint8_t seq, uint32_t nowMs);

private:
    bool ackedPending_[ARQ_CHUNK_BUDGET] = {};
    uint8_t retxCount_[ARQ_CHUNK_BUDGET] = {};
    uint32_t sentAtMs_[ARQ_CHUNK_BUDGET] = {};
    uint8_t baseSeq_[ARQ_CHUNK_BUDGET] = {};
    uint16_t bytesRecvd_[ARQ_CHUNK_BUDGET] = {};
    uint32_t generation_ = 0;
    uint8_t gbnBase_ = 0;
    bool gbnActive_ = false;
};

} // namespace autolink
