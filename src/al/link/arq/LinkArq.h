
#pragma once
#include "al/link/arq/ArqCache.h"
#include "al/link/IHalCtx.h"
#include <stdint.h>
#include <stddef.h>

// FreeRTOS semaphore handle is a pointer-typed
// typedef in <freertos/semphr.h>, but that
// header is a "second-pass" file in modern
// FreeRTOS-Kernel — it #error's if FreeRTOS.h
// wasn't included first. We can't guarantee
// that here (LinkArq.h is pulled in by Link.h
// before AutoLink.h has had a chance to
// include FreeRTOS.h via UtilBlink.h), so the
// real include is deferred to LinkArq.cpp and
// the field is held as a void* in the header.
// The .cpp casts back to the real
// SemaphoreHandle_t on every FreeRTOS API call
// via a static inline helper. Pinned by
// FieldWedgeFixesTest pin 11 — the header must
// never include <freertos/semphr.h> (directly
// or transitively) or the second-pass trap
// returns. See LinkArq.h::ack_sem_ below.

namespace autolink {

// GBN sender-window bookkeeping. All five per-seq fields are
// indexed by budgetIdx(seq) = seq % ARQ_CHUNK_BUDGET — a fixed
// function of seq, never gbnBase_-relative. idxOf(seq) drifts
// every time gbnBase_ advances, so a write at send-time and a
// read after a later cumulative ACK can land on different slots;
// it survives only as decideSlot()'s "is seq in the window at
// all" guard. ARQ_CHUNK_BUDGET = 2 * AUTOLINK_ARQ_PIPELINE_WINDOW
// and at most W chunks are ever in flight, so a slot's previous
// occupant is always ACKed before the same seq % B index is
// reused. Pinned by LinkArqTest and LinkBaseSeqTrackingTest.
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
    static int budgetIdx(uint8_t seq) { return (int)seq % ARQ_CHUNK_BUDGET; }

    void onSent(uint8_t seq, uint8_t baseSeq, uint32_t nowMs);
    // Resets retxCount_ + sentAtMs_ only: gives the base one more
    // RTO after a maxRetx Keep verdict. Pinned by GbnKeepRearmTest.
    void rearmSlot(uint8_t seq, uint32_t nowMs);
    void onAcked(uint8_t seq, uint16_t bytesRecvd = 0);
    uint16_t bytesFor(uint8_t seq) const;
    uint16_t bytesForMessage(uint8_t baseSeq) const {
        uint16_t total = 0;
        for (int i = 0; i < ARQ_CHUNK_BUDGET; i++)
            if (baseSeq_[i] == baseSeq)
                total += bytesRecvd_[i];
        return total;
    }
    void onNaked(uint8_t missingCobsSeq, uint32_t nowMs);
    void setPending(uint8_t seq, bool v = true);
    bool isPending(uint8_t seq) const;
    bool waitForAck(IHalCtx &ctx, uint8_t seq, uint32_t timeoutMs);
    int pendingCount() const;
    int retxCountTotal() const;
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
    // Counting semaphore given by onAcked / clearAll /
    // every generation-bump event the SYNC wait
    // might race. waitForAck takes it inside the
    // spin so a 100 ms ACK-latency stretch costs
    // zero CPU instead of running the spin against
    // a bitfield. ESP32 only — host tests don't
    // need it (the test thread calls onRx
    // synchronously, so a yield loop is the
    // cheaper test-side path) and the
    // CompileCheckTest host stub doesn't define
    // SemaphoreHandle_t. Held as void* in the
    // header so the freertos include stays in
    // the .cpp (semphr.h is a second-pass
    // header that #error's without FreeRTOS.h
    // first); the .cpp casts back to the real
    // SemaphoreHandle_t on every FreeRTOS API
    // call. Pinned by FieldWedgeFixesTest
    // pin 11 — the header must not include
    // <freertos/semphr.h> (directly or via any
    // transitive chain) or the second-pass
    // trap returns. The host busy-spin in
    // waitForAck never touches the field.
    void *ack_sem_ = nullptr;
    void ensureAckSem_unlocked();
};

} // namespace autolink
