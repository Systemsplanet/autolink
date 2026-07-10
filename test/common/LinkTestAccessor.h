// Test-only shim for Link. The test_*
// methods (test_markAckedPending,
// test_sendMsgBegin, test_sendMsgStillWaiting,
// syncAckTimeoutMsForTest, arqCacheForTest) are
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
    // The ARQ state is now a ring indexed by
    // (seq - gbnBase) % window; seqs outside the
    // window would silently no-op the slot write,
    // so we shift gbnBase forward when needed to
    // land `s` at the head of the window.
    void markAckedPending(uint8_t s) {
        if (l_.arq_.idxOf(s) < 0) {
            l_.arq_.setGbnBase(s);
            l_.arq_.setGbnActive(true);
        }
        l_.arq_.setPending(s, true);
    }

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

    // ARQ cache reference (lifetime: cache
    // outlives the link, enforced by the
    // Link ctor taking IArqCache&; AutoLink
    // owns the cache by value and constructs
    // the link first, so dtor order is safe).
    IArqCache &arqCache() const { return l_.arqCache_; }

    // Sweep-state inspection + drive
    // for LinkSweepPhaseTest. Lets
    // the test pin pong's P2->P3
    // transition (and other sweep
    // shape contracts) without
    // driving the full sweep handshake.
    SweepPhase sweepPhase() const { return l_.sweep_.phase(); }
    int phase3Acks() const { return l_.sweep_.phase3Acks(); }
    int phase3Baud() const { return l_.sweep_.phase3Baud(); }
    void setSweepPhase(SweepPhase p) { l_.sweep_.setPhase(p); }

    // spdI access for the OOB-closed-shape test.
    // The runtime pin sets spdI to a known tail
    // index and walks the read paths to verify
    // every one of them is bounded by
    // AUTOLINK_MAX_BAUDS — the test never
    // accesses cfg.allowedBauds[i] directly.
    void setSpdI(int i) { l_.setCurrentSpdI(i); }

    // NAK signal for the gap-stop entry-edge
    // test. Drives Link::onNak directly so the
    // test can verify that lastNakSeq() is
    // stamped (the signal side of the gap-stop
    // feature, complement to the PingGap.h
    // decision side).
    bool onNak(uint8_t seq) { return l_.onNak(seq); }
    uint8_t lastNakSeq() const { return l_.lastNakSeq(); }
    uint8_t lastAckSeq() const { return l_.lastAckSeq(); }

    // Stamp the tx-reject streak the way a failed
    // sendMsg does. Lets the SYNC stall-watchdog
    // regression drive the streak without spinning
    // a real waitForAck timeout (which would busy-
    // wait on the injected clock in a single-thread
    // host test).
    void noteTxReject() { l_.noteTxReject_unlocked(); }

    // Direct LinkArq handle for tests that
    // drive waitForAck / clearAll by hand
    // (e.g. the ABA-hazard regression in
    // LinkArqTest).
    LinkArq &arq() { return l_.arq_; }
    int okTick() const { return l_.okTickMs(); }
    uint8_t txSeqNow() const { return l_.txSeq; }
    void ackFrame(uint8_t seq) {
        l_.hw.lock();
        l_.sendAckFrame_unlocked(seq);
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
    // GBN base tracking (ASYNC sender oldest-unacked). gbnBase_/
    // gbnActive_ now live in arq_ (one owner with the cache it
    // describes); the accessor still drives them so existing
    // tests that seed the base without a full sendMsg keep
    // working.
    void setGbnBase(uint8_t seq) {
        l_.arq_.setGbnBase(seq);
        l_.arq_.setGbnActive(true);
        l_.gbnAttempts_ = 0;
    }
    uint8_t gbnBase() const { return l_.arq_.gbnBase(); }
    bool gbnActive() const { return l_.arq_.gbnActive(); }
    void setLastRx(uint32_t t) { l_.lastRxMs = t; }
    void setLastTx(uint32_t t) { l_.lastTxMs = t; }
    void nakFrame(uint8_t seq) {
        l_.hw.lock();
        l_.sendNakFrame_unlocked(seq);
        l_.hw.unlock();
    }

    // Direct LinkSweep handle for tests that
    // inspect the dwell table (e.g.
    // PongP1GuardOutlastsMasterP2Test pins
    // master/slave dwell values without
    // running the full sweep).
    LinkSweep &sweep() { return l_.sweep_; }
    const LinkSweep &sweep() const { return l_.sweep_; }

    // Toggle the test-only forward-resync
    // mode that re-creates the pre-fix
    // gap-handling (drop the gap, advance
    // rxSeq, drop the missing frame). Off
    // by default; the production link holds
    // out-of-order frames in the reorder
    // buffer. Replaces the AutoLinkConfig
    // test flag that previously leaked into
    // the user-facing struct.
    void setForwardResync(bool v) { l_.testForwardResync_ = v; }

    // SYNC resync-spiral fix hooks.
    // txQuiet gate: force the post-lock state a real drop would
    // leave (locked-at + disc streak), then read the admission gate.
    void setLockedAt(uint32_t t) { l_.lockedAtMs_ = t; }
    void setRecentDiscs(int n, uint32_t atMs) {
        l_.recentDiscs_ = n;
        l_.lastDiscMs_ = atMs;
    }
    bool txQuiet() const { return l_.txQuiet_unlocked(); }
    int postLockQuietMs() const { return l_.cfg.postLockQuietMs; }
    // Drive one SYNC ladder step + read the attempt counter.
    bool syncBegin(const uint8_t *b, int n) {
        return l_.test_sendMsgBegin(b, n);
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
    bool syncRtoStep() { return l_.test_syncRtoStep(); }
    int syncAttempt() const { return l_.test_syncAttempt(); }

    // Pass-through to the link itself for
    // fixtures that already hold a Link&.
    Link &link() { return l_; }
    const Link &link() const { return l_; }

    // bytes-recvd query pass-throughs. bytesRecvdFor
    // is one chunk's wire-ACK-reported bytes; the
    // per-message variant sums every chunk that
    // shares the same baseSeq (set by the sender in
    // LinkApi's ASYNC multi-chunk path). Used by
    // the base-seq tracking regression suite to pin
    // the contract that a 5120-byte message's 22
    // chunks contribute MSG_HDR + 5120 to the
    // per-message sum.
    uint16_t bytesRecvdFor(uint8_t seq) const { return l_.bytesRecvdFor(seq); }
    uint16_t bytesRecvdForMessage(uint8_t baseSeq) const {
        return l_.bytesRecvdForMessage(baseSeq);
    }

    // Direct drive of the UtilFrameRx::feed
    // path. Used by the rxBytes wire-ACK suite
    // to inject a synthetic ACK/NAK/CTRL frame
    // into the link's framer without going
    // through a full sender/receiver round-trip
    // (the rxBytes bump is in onAck/onNak which
    // need the framer to dispatch; calling
    // UtilFrameRx directly is the unit-test
    // shape that exercises the dispatch in
    // isolation).
    int utilFrameRxFeed(const uint8_t *data, int len) {
        return l_.frameRx.feed(data, len);
    }

private:
    Link &l_;
};

} // namespace autolink
