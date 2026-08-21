
#pragma once
#include "al/link/arq/ArqCache.h"
#include "al/link/IHalCtx.h"
#include <stdint.h>
#include <stddef.h>
#include <cstdio>

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
    // idxOf() uses the wire's COBS_SEQ_SPACE (254, declared in
    // AutoLinkConfig.h as autolink::COBS_SEQ_SPACE). Earlier
    // shape had its own class constant here of 256, which
    // shadowed the wire value and broke wrap-around modular
    // distance: the field probe showed 61 (base, seq) pairs
    // where a live in-window chunk's ACK was falsely rejected
    // as a prior-lap re-ACK, plus 435 wrong-but-in-range
    // indices, every time the cobsSeq wrapped past 0xFD into
    // 0..0xFD's modular neighbours. Use the wire constant; never
    // a wider one. Pinned by ArqIdxOfWrapTest.

    LinkArq() { clearAll(); }

    void clearAll();
    // F6: test-only knob that bumps the
    // clearAllEpoch_ counter (the same
    // counter the real endedOkSession /
    // clearAll path uses) so the test
    // can simulate a resync without
    // driving a full GBN ladder. The
    // pre-F-pass shape was named
    // bumpClearAllEpochForTest_unlocked +
    // generation() — the names leaked
    // the implementation detail that
    // this counter was also the lap
    // source (it isn't — the lap is
    // txSeqLap_ from the caller's
    // onSent 4th arg). Pinned by
    // ClearAllWakesWaitForAckTest.
    void bumpClearAllEpochForTest_unlocked() { clearAllEpoch_++; }

    uint8_t gbnBase() const { return gbnBase_; }
    void setGbnBase(uint8_t seq) { gbnBase_ = seq; }
    bool gbnActive() const { return gbnActive_; }
    void setGbnActive(bool v) { gbnActive_ = v; }

    // Format gbnBase() for a log line. In SYNC (gbnActive_ ==
    // false), gbnBase_ is never advanced by a cumulative-ACK walk
    // — SYNC's single-slot lockstep has no cumulative base at
    // all, so the field's every base-keyed diagnostic
    // (gbnBaseStuckSinceMs_, gbnLastRetxBase_, the storm-stuck
    // verdict) read a constant 0 for the whole SYNC session,
    // making every line look identical and defeating the point
    // of the diagnostic. Returns "N/A" in that case so a reader
    // can tell "base=0 because the link is genuinely at seq=0"
    // apart from "base=0 because this diagnostic doesn't apply
    // in SYNC mode". Pinned by PostSoakFieldFixesTest (AL-10 pin).
    const char *gbnBaseStrForLog(char *buf, size_t buflen) const {
        if (!gbnActive_) {
            if (buflen >= 4) {
                buf[0] = 'N';
                buf[1] = '/';
                buf[2] = 'A';
                buf[3] = 0;
            } else if (buflen > 0) {
                buf[0] = 0;
            }
            return buf;
        }
        if (buflen >= 4) {
            int n = snprintf(buf, buflen, "%u", (unsigned)gbnBase_);
            if (n < 0)
                buf[0] = 0;
        } else if (buflen > 0) {
            buf[0] = 0;
        }
        return buf;
    }

    int idxOf(uint8_t seq) const {
        int d = (int)seq - (int)gbnBase_;
        if (d < 0)
            d += COBS_SEQ_SPACE;
        return d < (int)AUTOLINK_ARQ_PIPELINE_WINDOW ? d : -1;
    }
    static int budgetIdx(uint8_t seq) { return (int)seq % ARQ_CHUNK_BUDGET; }

    void onSent(uint8_t seq, uint8_t baseSeq, uint32_t nowMs,
                uint8_t baseLap = 0);
    // Resets retxCount_ + sentAtMs_ only: gives the base one more
    // RTO after a maxRetx Keep verdict. Pinned by GbnKeepRearmTest.
    void rearmSlot(uint8_t seq, uint32_t nowMs);
    void onAcked(uint8_t seq, uint16_t bytesRecvd = 0);
    // AL90-9 / AL92-6: bump the NAK
    // counter without touching
    // sentAtMs_. Used by the LinkRx path
    // for a NAK that is suppressed —
    // base-stuck or same-event dedup'd,
    // no resend goes out — so the
    // counter still needs to advance
    // (a sustained base-stuck must
    // eventually trip the watchdog) but
    // the RTO clock must not be
    // reseated by a NAK that produced
    // no action. A NAK that DOES trigger
    // a resend calls onNaked() instead.
    void noteSuppressedNak(uint8_t missingCobsSeq);
    uint16_t bytesFor(uint8_t seq) const;
    // Per-message byte sum: walks the full 64-slot ring
    // and sums bytesRecvd_ for every slot whose baseSeq_
    // matches. baseSeq_ survives the ACK (set on
    // onSent, never cleared), so a query that arrives
    // after a cumulative ACK advanced gbnBase_ past
    // the held baseSeq still finds the data — the
    // held slots' baseSeq_/bytesRecvd_ are intact
    // until the next lap's onSent overwrites them.
    // Pinned by LinkBaseSeqTrackingTest's two- and
    // three-message pins.
    //
    // The lap qualifier (baseLap_[i] == baseLap)
    // gates the match: a seq reused after a 254-lap
    // wrap (the COBS_SEQ_MAX = 0xFD wire space,
    // not 256) would otherwise pull a stale slot's
    // bytesRecvd_ into the new message's total and
    // over-report. Pinned by
    // BytesForMessageLapQualifierTest (a same-seq
    // re-stamp after two full 256-seq rolls must
    // return 0; the unwalked shape summed both
    // messages' bytes).
    uint16_t bytesForMessage(uint8_t baseSeq, uint8_t baseLap) const {
        uint16_t total = 0;
        for (int i = 0; i < ARQ_CHUNK_BUDGET; i++) {
            if (baseSeq_[i] == baseSeq && baseLap_[i] == baseLap)
                total += bytesRecvd_[i];
        }
        return total;
    }
    // Backwards-compatible single-arg overload used by
    // callers that don't track the lap (e.g. unit tests
    // building a single-message shape). Returns the
    // sum across all laps, which is the wrong answer
    // for a re-stamped seq but the right answer for a
    // never-reused seq. Kept so the older test surface
    // doesn't need to learn about lap. Pinned by
    // LinkBaseSeqTrackingTest.
    // F8: hidden behind AUTOLINK_HOST_TEST. The
    // production build compiles this out so the
    // unqualified walk cannot be reached from app
    // code. The host test surface
    // (LinkBaseSeqTrackingTest) uses the single-
    // message shape where the lap is unambiguous
    // and the single-arg form is fine.
#ifdef AUTOLINK_HOST_TEST
    uint16_t bytesForMessage(uint8_t baseSeq) const {
        uint16_t total = 0;
        for (int i = 0; i < ARQ_CHUNK_BUDGET; i++) {
            if (baseSeq_[i] == baseSeq)
                total += bytesRecvd_[i];
        }
        return total;
    }
#endif
    void onNaked(uint8_t missingCobsSeq, uint32_t nowMs);
    void setPending(uint8_t seq, bool v = true);
    bool isPending(uint8_t seq) const;
    bool waitForAck(IHalCtx &ctx, uint8_t seq, uint32_t timeoutMs);
    int pendingCount() const;
    int retxCountTotal() const;
    // Per-slot RTO-driven retx count for the honest-drop gate —
    // retxCountTotal() is window-wide and wrong here; the
    // storm-stuck escalation needs the base's own count. Pinned
    // by GbnStuckForcesRetxTest.
    uint8_t retxCountFor(uint8_t seq) const;
    // Per-slot NAK-driven resend count. Kept separate from
    // retxCount_ so two NAKs cannot satisfy the "two real failed
    // attempts" gate (retxCountFor(base) >= 2) the storm-stuck
    // verdict reads — a NAK is a peer's request for an immediate
    // resend, not a proof of a failed RTO. The honest-drop gate
    // is fed by retxCountFor() and the onNak path increments
    // nakCount_ instead. Pinned by NakCountSplitTest.
    uint8_t nakCountFor(uint8_t seq) const;
    // SYNC-only: a CRC-valid NAK naming the in-flight seq means the
    // peer has already told us it is missing, so waiting out the
    // full RTO is dead air. noteNakWake makes the current
    // waitForAck return false immediately so the SYNC ladder takes
    // its next retx step now. waitForAck clears the flag on entry,
    // which caps this at one wake per ladder attempt — a NAK burst
    // (17 in 65 ms observed in the field) cannot spin the ladder
    // through every attempt. Pinned by SyncNakFastRetxTest.
    void noteNakWake(uint8_t seq);
    bool isAcked(uint8_t cobsSeq) const { return !isPending(cobsSeq); }

    enum class Action { Hold, Retx, Drop };
    Action decideSlot(uint8_t seq, uint32_t nowMs, uint32_t ackRtoMs,
                      uint8_t maxRetx) const;
    // D13: applyRetx takes a flag distinguishing
    // RTO-driven retransmits (counted toward the
    // storm-stuck verdict's "two real failed
    // attempts" gate) from NAK-driven resends (the
    // peer's loss signal, not an RTO-elapsed
    // failure). nakCount_ already splits the NAK
    // path; this keeps retxCount_ for the RTO path
    // only. The unwalked shape called applyRetx
    // with the same source flag for both, so a NAK
    // burst could satisfy retxCountFor(base) >= 2
    // in well under 50 ms and open the storm-stuck
    // verdict on evidence that wasn't an RTO
    // failure. Pinned by RetxCountSourceTest
    // (a 17-NAK resend burst keeps
    // retxCountFor at 0; nakCountFor climbs).
    uint8_t applyRetx(uint8_t seq, uint32_t nowMs, bool rtoDriven = true);

private:
    bool ackedPending_[ARQ_CHUNK_BUDGET] = {};
    // RTO-driven retx count, capped at 0xFF (uint8_t max).
    // Earlier shape was a bare `retxCount_[bi]++` that
    // silently wrapped past 0xFF — a probe of 300 applyRetx
    // calls on the same seq read 44, not the expected 255 or
    // 300. The wrap hid honest-drop evidence from the
    // storm-stuck verdict mid-storm. applyRetx guards the
    // increment with `< 0xFF` so the count is the source
    // of truth. Pinned by RetxCountCapTest.
    uint8_t retxCount_[ARQ_CHUNK_BUDGET] = {};
    // NAK-driven resend count. Kept separate from retxCount_
    // (see nakCountFor() above) so a NAK burst cannot satisfy
    // the "two real failed attempts" gate that the storm-stuck
    // verdict reads. NAKs are the peer's request for an
    // immediate resend, not a proof of an RTO-elapsed failure.
    // Pinned by NakCountSplitTest.
    uint8_t nakCount_[ARQ_CHUNK_BUDGET] = {};
    // baseLap_[]: stamped on every onSent to the
    // same caller's txSeqLap_. bytesForMessage
    // and the cumulative walk compare the lap
    // before the seq+base pair — a seq reused
    // after a 254-lap wrap (e.g. seq 30
    // re-stamped after seq 30 is freed and
    // gbnBase_ rolls past 254) must not be
    // charged against the prior message's
    // budget. Stored as uint8_t and compared
    // with mod-256 wrap so the test can advance
    // the same seq through two complete laps and
    // verify the second message
    // walks past the first's bytesRecvd_ residue
    // (the unwalked shape summed both messages
    // into bytesForMessage() and over-reported).
    // Pinned by BytesForMessageLapQualifierTest.
    uint8_t baseLap_[ARQ_CHUNK_BUDGET] = {};
    volatile bool nakWake_ = false;
    uint8_t nakWakeSeq_ = 0xFF;
    uint32_t sentAtMs_[ARQ_CHUNK_BUDGET] = {};
    uint8_t baseSeq_[ARQ_CHUNK_BUDGET] = {};
    uint16_t bytesRecvd_[ARQ_CHUNK_BUDGET] = {};
    uint32_t clearAllEpoch_ = 0;
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
