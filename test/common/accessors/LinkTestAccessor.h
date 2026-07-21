// Friend shim: the only path a host test reaches Link's internals
// through. Build under -DAUTOLINK_HOST_TEST only; never include from
// src/ or include/.
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

    // Test seam: drive a retx-shape TX (same code path
    // as resendCobsFrame_unlocked). Pinned by
    // RateLimitRtxChargedTest.
    void buildAndTxCobsFrameForTest(uint8_t seq, const uint8_t *b, int n) {
        l_.hw.lock();
        l_.buildAndTxCobsFrame_unlocked(seq, b, n);
        l_.hw.unlock();
    }

    // A seq outside the window would silently no-op the slot write, so
    // shift the base to land `s` at the head of it.
    void markAckedPending(uint8_t s) {
        if (l_.arq_.idxOf(s) < 0) {
            l_.arq_.setGbnBase(s);
            l_.arq_.setGbnActive(true);
        }
        l_.arq_.setPending(s, true);
    }

    // SYNC sendMsg blocks on the injected clock; these two halves let a
    // single-threaded host test pump time between send and ACK.
    bool sendMsgBegin(const uint8_t *b, int len) {
        return l_.test_sendMsgBegin(b, len);
    }
    bool sendMsgStillWaiting() { return l_.test_sendMsgStillWaiting(); }
    int syncAckTimeoutMs() const { return l_.cfg.syncAckTimeoutMs; }
    bool syncBegin(const uint8_t *b, int n) {
        return l_.test_sendMsgBegin(b, n);
    }
    bool syncRtoStep() { return l_.test_syncRtoStep(); }
    int syncAttempt() const { return l_.test_syncAttempt(); }

    IArqCache &arqCache() const { return l_.arqCache_; }
    LinkArq &arq() { return l_.arq_; }
    LinkSweep &sweep() { return l_.sweep_; }
    const LinkSweep &sweep() const { return l_.sweep_; }
    Link &link() { return l_; }
    const Link &link() const { return l_; }

    SweepPhase sweepPhase() const { return l_.sweep_.phase(); }
    int phase3Acks() const { return l_.sweep_.phase3Acks(); }
    int phase3Baud() const { return l_.sweep_.phase3Baud(); }
    void setSweepPhase(SweepPhase p) { l_.sweep_.setPhase(p); }
    int baudAwareRtoMsForTest() const { return l_.baudAwareRtoMs_unlocked(); }
    void setSpdIForTest(uint8_t v) { l_.spdI = v; }

    // The no-lock setters exist because a MockHal::onSetSpd hook fires
    // inside the link's own lock release; re-entering hw.lock() would
    // deadlock the host's non-recursive std::mutex.
    void setSpdI(int i) { l_.setCurrentSpdI(i); }
    void forceStateNoLock(State st) { l_.changeState_unlocked(st); }

    bool onNak(uint8_t seq) { return l_.onNak(seq); }
    uint8_t lastNakSeq() const { return l_.lastNakSeq(); }
    uint8_t lastAckSeq() const { return l_.lastAckSeq(); }
    void noteTxReject() { l_.noteTxReject_unlocked(); }
    int okTick() const { return l_.okTickMs(); }

    void ackFrame(uint8_t seq) {
        l_.hw.lock();
        l_.sendAckFrame_unlocked(seq);
        l_.hw.unlock();
    }
    void nakFrame(uint8_t seq) {
        l_.hw.lock();
        l_.sendNakFrame_unlocked(seq);
        l_.hw.unlock();
    }
    void driveOnAck(uint8_t seq, uint16_t bytes = 0) {
        l_.hw.lock();
        (void)l_.onAck(seq, bytes);
        l_.hw.unlock();
    }
    int applyHealth(uint32_t now) {
        l_.hw.lock();
        int a = (int)l_.applyHealth_unlocked(now);
        l_.hw.unlock();
        return a;
    }
    bool sweepRetx(uint32_t now) {
        l_.hw.lock();
        bool brk = l_.sweepRetx_unlocked(now);
        l_.hw.unlock();
        return brk;
    }
    // Backdoor: seed the baud-aware storm threshold
    // (normally set by lockOk_unlocked). Tests that
    // forceState(OK) bypass lockOk and would otherwise
    // see the threshold at its default 0 — the storm
    // check trips on the first call and the test fails
    // for an unrelated reason. Pinned by
    // BaudAwareStuckThresholdTest.
    void setGbnBaseStuckThresholdMsForTest(uint32_t ms) {
        l_.hw.lock();
        l_.gbnBaseStuckThresholdMs_ = ms;
        l_.gbnBaseStuckThresholdOverridden_ = true;
        l_.hw.unlock();
    }
    uint32_t gbnBaseStuckThresholdMsForTest() const {
        return l_.gbnBaseStuckThresholdMs_;
    }
    // PONG counter accessors: the P2/P3 fallback paths
    // consume sweepPongCount_ to refuse a PONG-less
    // fallback. Host tests want to stage a nonzero
    // counter and verify a sweep entry clears it.
    int sweepPongCountForTest() const { return l_.sweepPongCount_; }
    void setSweepPongCountForTest(int v) {
        l_.hw.lock();
        l_.sweepPongCount_ = v;
        l_.hw.unlock();
    }
    // reset_unlocked is private; expose a thin wrapper
    // so host tests can drive the per-round counter
    // reset path.
    void resetUnlockedForTest() {
        l_.hw.lock();
        l_.reset_unlocked(true, false, ResetReason::HealthWatchdog);
        l_.hw.unlock();
    }
    // breakSuspectSeen_ accessor: the two-frame-clear
    // path advances this counter, and the
    // second-BREAK-after-frame gate consumes it.
    // Host tests stage a partial count to drive the
    // test path.
    void setBreakSuspectSeenForTest(uint8_t v) {
        l_.hw.lock();
        l_.breakSuspectSeen_ = v;
        l_.hw.unlock();
    }
    // Rate-window accessors: the rate-limit admission
    // check uses rateWindowStartMs_ and
    // rateWindowBytes_. Host tests stage these to
    // drive the rollover branch.
    void setRateWindowStartMsForTest(uint32_t v) {
        l_.hw.lock();
        l_.rateWindowStartMs_ = v;
        l_.hw.unlock();
    }
    void setRateWindowBytesForTest(uint32_t v) {
        l_.hw.lock();
        l_.rateWindowBytes_ = v;
        l_.hw.unlock();
    }
    uint32_t rateWindowBytesForTest() const { return l_.rateWindowBytes_; }
    uint32_t rateWindowStartMsForTest() const { return l_.rateWindowStartMs_; }
    void setRateNextAllowedMsForTest(int32_t v) {
        l_.hw.lock();
        l_.rateNextAllowedMs_ = v;
        l_.hw.unlock();
    }
    int32_t rateNextAllowedMsForTest() const { return l_.rateNextAllowedMs_; }
    void gbnResendWindow(uint32_t now) {
        l_.hw.lock();
        l_.gbnResendWindow_unlocked(now);
        l_.hw.unlock();
    }
    void forceState(State st) {
        l_.hw.lock();
        l_.changeState_unlocked(st);
        l_.hw.unlock();
    }
    void resetLink(bool count) {
        l_.hw.lock();
        l_.reset_unlocked(count);
        l_.hw.unlock();
    }
    void resetLink(bool count, ResetReason r) {
        l_.hw.lock();
        l_.reset_unlocked(count, false, r);
        l_.hw.unlock();
    }
    void resetLink(bool count, bool preserve, ResetReason r) {
        l_.hw.lock();
        l_.reset_unlocked(count, preserve, r);
        l_.hw.unlock();
    }

    void setGbnBase(uint8_t seq) {
        l_.arq_.setGbnBase(seq);
        l_.arq_.setGbnActive(true);
        l_.gbnAttempts_ = 0;
    }
    uint8_t gbnBase() const { return l_.arq_.gbnBase(); }
    bool gbnActive() const { return l_.arq_.gbnActive(); }
    int gbnAttemptsForTest() const { return l_.gbnAttempts_; }
    uint32_t gbnBackoffMsForTest() const { return l_.gbnBackoffMs_; }
    uint8_t gbnLastRetxBaseForTest() const { return l_.gbnLastRetxBase_; }
    int consecutiveKeepForTest() const { return l_.consecutiveKeep_; }
    int gbnKeepRescueCapForTest() const {
        return l_.gbnKeepRescueCap_unlocked();
    }
    uint8_t arqBaseForTest() const { return l_.arq_.gbnBase(); }
    int arqPendingCountForTest() const { return l_.arq_.pendingCount(); }

    uint64_t getDiagCountForTest() const {
        Stats s;
        l_.getStats(s);
        return s.discCount;
    }
    State getStateForTest() const { return l_.getState(); }
    int interChunkGapMsForTest() const { return l_.interChunkGapMs_unlocked(); }

    void setLastRx(uint32_t t) { l_.lastRxMs = t; }
    void setLastTx(uint32_t t) { l_.lastTxMs = t; }
    void setLastRxSeqForTest(uint8_t s) { l_.lastRxSeq_ = s; }
    uint8_t getLastRxSeqForTest() const { return l_.lastRxSeq_; }
    void setResweepPrefPendingForTest(bool v) { l_.resweepPrefPending_ = v; }
    bool resweepPrefPendingForTest() const { return l_.resweepPrefPending_; }
    void setResweepPrefAttemptsForTest(uint8_t v) {
        l_.resweepPrefAttempts_ = v;
    }
    void setSweepEpochForTest(uint8_t v) { l_.sweepEpoch_ = v; }
    uint8_t sweepEpochForTest() const { return l_.sweepEpoch_; }
    void setPeerSweepEpochForTest(uint8_t v) {
        l_.peerSweepEpoch_ = v;
        l_.peerSweepEpochKnown_ = true;
    }
    uint8_t peerSweepEpochForTest() const { return l_.peerSweepEpoch_; }
    bool peerSweepEpochKnownForTest() const { return l_.peerSweepEpochKnown_; }
    void setLockedAt(uint32_t t) { l_.lockedAtMs_ = t; }
    void setRecentDiscs(int n, uint32_t atMs) {
        l_.recentDiscs_ = n;
        l_.lastDiscMs_ = atMs;
    }
    bool txQuiet() const { return l_.txQuiet_unlocked(); }

    // One chunk's wire-ACK byte count; the per-message variant sums
    // every chunk sharing a baseSeq.
    uint16_t bytesRecvdFor(uint8_t seq) const { return l_.bytesRecvdFor(seq); }
    uint16_t bytesRecvdForMessage(uint8_t baseSeq) const {
        return l_.bytesRecvdForMessage(baseSeq);
    }

    // Inject a synthetic frame straight into the framer, so the
    // onAck/onNak dispatch can be exercised without a round-trip.
    int utilFrameRxFeed(const uint8_t *data, int len) {
        return l_.frameRx.feed(data, len);
    }

    uint32_t breakSuspectMsForTest() const { return l_.breakSuspectMs_; }
    ResetReason lastResetReasonForTest() const { return l_.lastResetReason_; }
    int recentDiscsForTest() const { return l_.recentDiscs_; }
    uint64_t breaksSuppressedForTest() const { return l_.breaksSuppressed_; }

    int locksWithoutRecvForTest() const { return l_.locksWithoutRecv_; }
    void setLocksWithoutRecvForTest(int n) { l_.locksWithoutRecv_ = n; }
    uint8_t preferredBaudForTest() const { return l_.preferredBaud_; }
    void setPreferredBaudForTest(uint8_t b) { l_.preferredBaud_ = b; }
    bool breakStormPendingForTest() const { return l_.breakStormPending_; }
    // gbnBaseStuckSinceMs_ / gbnBaseStuckTrackedSeq_ accessors:
    // sweepRetx_unlocked's CPU-stall re-arm and drain-RX
    // re-arm read+write these fields. Pinned by
    // FieldWedgeFixesTest.
    uint32_t gbnBaseStuckSinceMs_for_test() const {
        return l_.gbnBaseStuckSinceMs_;
    }
    void gbnBaseStuckSinceMs_set_for_test(uint32_t v) {
        l_.hw.lock();
        l_.gbnBaseStuckSinceMs_ = v;
        l_.hw.unlock();
    }
    uint8_t gbnBaseStuckTrackedSeq_for_test() const {
        return l_.gbnBaseStuckTrackedSeq_;
    }
    void gbnBaseStuckTrackedSeq_set_for_test(uint8_t v) {
        l_.hw.lock();
        l_.gbnBaseStuckTrackedSeq_ = v;
        l_.hw.unlock();
    }
    // lastOkTickMs_ accessor: the CPU-stall detector
    // stamps this at the top of sweepRetx_unlocked and
    // reads it to compute the gap-delta. Pinned by
    // FieldWedgeFixesTest.
    uint32_t lastOkTickMs_for_test() const { return l_.lastOkTickMs_; }
    void lastOkTickMs_set_for_test(uint32_t v) {
        l_.hw.lock();
        l_.lastOkTickMs_ = v;
        l_.hw.unlock();
    }
    int getErrCount() const { return l_.getErrCount(); }
    int peerBaudMismatchThresholdForTest() const {
        return l_.kPeerBaudMismatchThreshold;
    }
    // True while the post-lock settle window is still
    // open. Tests that must run inside it assert this
    // rather than assume lockPair returned fast enough.
    bool settleWindowOpenForTest() const {
        return l_.hw.nowMs() < l_.settleUntilMs_;
    }
    SendMsgReason lastSendMsgReasonForTest() const {
        return l_.lastSendMsgReason();
    }
    void setLastValidRxMsForTest(uint32_t t) { l_.lastValidRxMs = t; }
    uint32_t lastValidRxMsForTest() const { return l_.lastValidRxMs; }

private:
    Link &l_;
};

} // namespace autolink
