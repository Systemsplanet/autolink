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
    // Link is an ISweepCtx, which extends IHalCtx — waitForAck
    // takes the latter, so hand the Link itself through.
    IHalCtx &halCtx() { return l_; }
    uint8_t lastNakSeq() const { return l_.lastNakSeq(); }
    uint8_t lastAckSeq() const { return l_.lastAckSeq(); }
    void noteTxReject() { l_.noteTxReject_unlocked(); }
    int okTick() const { return l_.okTickMs(); }
    // F6: bump the ARQ clearAllEpoch_ (session-
    // mismatch / clearAll detection) counter
    // so the test can simulate a session
    // teardown. Note: the lap qualifier on
    // bytesForMessage is txSeqLap_, not
    // clearAllEpoch_ — the bump accessor is
    // the test hook for the clearAll wakeup
    // mechanism in waitForAck. Tests that
    // want a new lap should use
    // bumpTxSeqLapForTest(). Pinned by
    // ClearAllWakesWaitForAckTest (a
    // waitForAck in flight returns false on a
    // clearAll() — the clearAllEpoch_ bump is
    // the signal).
    void bumpClearAllEpochForTest() {
        l_.hw.lock();
        l_.arq_.bumpClearAllEpochForTest_unlocked();
        l_.hw.unlock();
    }
    // F6: read the tx-side cobsSeq wrap count.
    // The lap qualifier on bytesForMessage is
    // driven by this counter, not by the
    // session-mismatch clearAllEpoch_ (which
    // is still used by waitForAck as the
    // clearAll detection signal). Pinned by
    // BytesForMessageLapQualifierTest.
    uint8_t txSeqLapForTest() const { return l_.txSeqLap_; }
    void bumpTxSeqLapForTest() {
        l_.hw.lock();
        l_.txSeqLap_++;
        l_.hw.unlock();
    }
    // G3: drive an ACK/NAK from the
    // test side so the
    // txSmallCobs_unlocked txAvail
    // gate is exercised. The
    // accessor is host-test only
    // (the production build compiles
    // it out via AUTOLINK_HOST_TEST).
    void sendAckFrameForTest(uint8_t ackedCobsSeq, uint16_t bytesRecvd) {
        l_.hw.lock();
        l_.sendAckFrame_unlocked(ackedCobsSeq, bytesRecvd);
        l_.hw.unlock();
    }
    // F10: SessionResetsResendSourceTest
    // needs to read and set the
    // resend source flag the
    // production code path mutates.
    void setResendSourceForTest(ResendSource s) {
        l_.hw.lock();
        l_.resendSource_ = s;
        l_.hw.unlock();
    }
    ResendSource resendSourceForTest() const { return l_.resendSource_; }
    // D12: read the baud-derived dedup window. The
    // test asserts the window is < 500 ms (the
    // syncAckTimeoutMs floor the unwalked shape
    // used). Pinned by
    // GbnResendSameEventDedupeTest.
    int gbnResendFlightMs() {
        l_.hw.lock();
        int w = l_.gbnResendFlightMs_unlocked();
        l_.hw.unlock();
        return w;
    }
    // D11: test-only knob to seed a held NAK
    // state (simulates the onNak path's
    // holdNakActive_=true / holdNakSeq_=N /
    // holdNakWrap_=W assignment without
    // driving a real NAK). Pinned by
    // HoldNakWrapSessionResetTest.
    void setHoldNakForTest(uint8_t seq, uint8_t wrap) {
        l_.hw.lock();
        l_.holdNakActive_ = true;
        l_.holdNakSeq_ = seq;
        l_.holdNakWrap_ = wrap;
        l_.rxSeqWrap_ = wrap;
        l_.hw.unlock();
    }
    // D11: read holdNakActive_ to verify the
    // session reset cleared it. Pinned by
    // HoldNakWrapSessionResetTest.
    bool holdNakActiveForTest() const { return l_.holdNakActive_; }
    // AL97-2: read holdNakLastMs_ to verify the liveness re-emit
    // timer is being stamped correctly (fresh hold, drain re-emit,
    // and the liveness-timer re-emit all stamp it). Pinned by
    // HoldNakLivenessCadenceTest.
    uint32_t holdNakLastMsForTest() const { return l_.holdNakLastMs_; }
    static constexpr uint32_t HOLD_NAK_LIVENESS_MS_FOR_TEST =
        Link::HOLD_NAK_LIVENESS_MS;
    // D11: invoke the same reset path the
    // teardown uses (endedOkSession branch in
    // reset_unlocked). The test seeds a held NAK
    // state, calls this, and verifies the hold
    // cleared. Pinned by
    // HoldNakWrapSessionResetTest.
    void endedOkSessionForTest() {
        l_.hw.lock();
        l_.reset_unlocked(false, false, ResetReason::UserDropLink);
        l_.hw.unlock();
    }
    // AL-D1: drives Link::onPayload directly — the
    // real receive-path function the app-buf-full
    // hold-NAK logic (AL89-5) lives in — rather than
    // re-declaring or grepping for its logic.
    // onPayload is private and normally reached only
    // via onRx's COBS-decoded dispatch; this
    // accessor takes the lock onPayload itself
    // assumes is already held (every real call site
    // is inside onRx's hw.lock()/unlock() pair) and
    // calls it with a hand-built payload, skipping
    // COBS decode entirely — the same abstraction
    // level LinkArqTest.cpp already uses for
    // decideSlot(). Pinned by
    // HoldNakSelfDescribingTest.
    bool onPayloadForTest(uint8_t cobsSeq, const uint8_t *b, int n) {
        l_.hw.lock();
        bool r = l_.onPayload(cobsSeq, b, n);
        l_.hw.unlock();
        return r;
    }
    // AL97-5: drives findMsgHeaderResync_unlocked directly. Normally
    // reached only from inside recvMsg's already-locked stale-
    // abandon and beginMsg-failure branches. Pinned by
    // ResyncScanReportsDroppedBytesTest.
    int findMsgHeaderResyncForTest(int maxScan) {
        l_.hw.lock();
        int r = l_.findMsgHeaderResync_unlocked(maxScan);
        l_.hw.unlock();
        return r;
    }
    // AL-D1: drives Link::onNak directly, the real
    // NAK-dispatch function the base-stuck resend
    // suppression (AL89-6) lives in. Same locking
    // shape as onPayloadForTest above — every real
    // call site is inside onRx's lock.
    bool onNakForTest(uint8_t missingCobsSeq) {
        l_.hw.lock();
        bool r = l_.onNak(missingCobsSeq);
        l_.hw.unlock();
        return r;
    }

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
        // D13: callers in the GbnBurstCapTest want
        // the burst's applyRetx to count toward the
        // retxCount_ storm-stuck gate (the test
        // checks retxCountTotal after the call).
        // gbnResendWindow_unlocked reads
        // resendSource_ to decide whether to bump
        // retxCount_; default the accessor to Rto
        // so the test's pre-existing assertion
        // (retxAfter == cap) keeps working. Tests
        // that want the Nak path stamp
        // `l_.resendSource_ = ResendSource::Nak`
        // before calling. Pinned by
        // GbnBurstCapTest pin 3 +
        // GbnStuckNakCountGateTest.
        l_.resendSource_ = ResendSource::Rto;
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
    // Backdoor: seed gbnBackoffMs_ directly. Fix-2's stuck-window
    // clamp (max(gbnBaseStuckThresholdMs_, gbnBackoffMs_ + ackRtoMs))
    // needs a test that stages the backoff value ahead of any RTO
    // ladder, rather than driving several real rounds to grow it
    // organically. Pinned by GbnBackoffTest.
    void setGbnBackoffMsForTest(uint32_t v) {
        l_.hw.lock();
        l_.gbnBackoffMs_ = v;
        l_.hw.unlock();
    }
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
    // AL90-2/3 test hooks: plant the
    // first-TX / first-peer-response markers
    // for a post-lock evidence-gate test.
    void setPostLockFirstTxDone(uint32_t t) { l_.postLockFirstTxDone_ = t; }
    void setFirstPeerResponseSeen(bool v) { l_.firstPeerResponseSeen_ = v; }
    bool txQuiet() const { return l_.txQuiet_unlocked(); }

    // One chunk's wire-ACK byte count; the per-message variant sums
    // every chunk sharing a baseSeq. F8: the two-arg form (with
    // lap) is the production surface; the single-arg form is
    // host-test only (the un-lapped walk can alias a seq
    // re-stamped across a 254-lap wrap). The accessor exposes
    // both so the host test can call either.
    uint16_t bytesRecvdFor(uint8_t seq) const { return l_.bytesRecvdFor(seq); }
    uint16_t bytesRecvdForMessage(uint8_t baseSeq, uint8_t baseLap) const {
        return l_.bytesRecvdForMessage(baseSeq, baseLap);
    }
#ifdef AUTOLINK_HOST_TEST
    uint16_t bytesRecvdForMessage(uint8_t baseSeq) const {
        return l_.bytesRecvdForMessage(baseSeq);
    }
#endif

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
    static constexpr uint8_t NO_PREFERRED_BAUD_FOR_TEST =
        Link::NO_PREFERRED_BAUD;
    static constexpr int BAUD_UPGRADE_MAX_ATTEMPTS_FOR_TEST =
        Link::BAUD_UPGRADE_MAX_ATTEMPTS;
    uint8_t bestProvenBaudForTest() const { return l_.bestProvenBaud_; }
    void setBestProvenBaudForTest(uint8_t b) { l_.bestProvenBaud_ = b; }
    uint32_t baudUpgradeAtMsForTest() const { return l_.baudUpgradeAtMs_; }
    int baudUpgradeAttemptsForTest() const { return l_.baudUpgradeAttempts_; }
    // Drives the real lock-completion path (bestProvenBaud_
    // tracking + upgrade arming both live here), rather than
    // forceState(OK) which bypasses it entirely.
    void lockOkForTest(int idx) {
        l_.hw.lock();
        l_.lockOk_unlocked(idx, "test");
        l_.hw.unlock();
    }
    // Drives the real OK-tick path (the upgrade trigger lives at
    // its top) instead of calling the private timer method
    // directly.
    bool onTimerOkForTest() {
        l_.hw.lock();
        bool r = l_.onTimerOk_unlocked();
        l_.hw.unlock();
        return r;
    }
    void setWasEverOkForTest(bool v) { l_.wasEverOk_ = v; }
    uint32_t resweepPrefBudgetMs() const {
        return l_.resweepPrefBudgetMs_unlocked();
    }
    int resweepPrefAttemptsForTest() const { return l_.resweepPrefAttempts_; }
    bool breakStormPendingForTest() const { return l_.breakStormPending_; }
    bool breakConfirmPendingForTest() const { return l_.breakConfirmPending_; }
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
    // Backdoor for facade mode-agreement tests: the production
    // code path is `link->setMode(m)` → `hal->setMode(m)` →
    // `hal.getMode()`, but `Link::hw` is private. Pinned by
    // ModeSyncBeforeBeginTest.
    IHal &hwForTest() { return l_.hw; }
    // True while the post-lock settle window is still
    // open. Tests that must run inside it assert this
    // rather than assume lockPair returned fast enough.
    bool settleWindowOpenForTest() const {
        return l_.hw.nowMs() < l_.settleUntilMs_;
    }
    SendMsgReason lastSendMsgReasonForTest() const {
        return l_.lastSendMsgReason();
    }
    // E3: read the Stats snapshot so the
    // async-wedge test can verify
    // txRingStallDrops advanced. Pinned by
    // SendMsgTxRingStallDropsCountedTest.
    Stats getStatsForTest() const {
        Stats s{};
        l_.getStats(s);
        return s;
    }
    void setLastValidRxMsForTest(uint32_t t) { l_.lastValidRxMs = t; }
    uint32_t lastValidRxMsForTest() const { return l_.lastValidRxMs; }

private:
    Link &l_;
};

} // namespace autolink
