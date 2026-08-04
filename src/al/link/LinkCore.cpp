
#include "al/link/Link.h"
#include "al/link/arq/ArqCache.h"
#include "al/link/arq/LinkArq.h"
#include "al/link/sweep/LinkSweep.h"
#include "al/util/Log.h"

#ifdef ARDUINO
#    include <esp_system.h>
#endif

static constexpr const char *TAG = "AutoLink";

namespace autolink {
static_assert(
    MAX_CHUNK + MSG_HDR <= ArqCache::POOL_BUF_MAX,
    "MAX_CHUNK + MSG_HDR > ArqCache::POOL_BUF_MAX: bump POOL_BUF_MAX or shrink MAX_CHUNK");

const char *StateToStr(State s) {
    switch (s) {
    case State::OK:
        return "OK";
    case State::SWP:
        return "SWP";
    default:
        return "UNK";
    }
}

Link::Link(IHal &h, IArqCache &cache, bool isMasterNode,
           const AutoLinkConfig &config)
    : hw(h), arqCache_(cache), isMaster(isMasterNode), cfg(config),
      state(State::OK), errs(0), spdI(0), pingSample(0), emptySweeps(0),
      baudSweep((int)config.clampedCount()), rxIdx(0), frameRx(*this),
      lastRxMs(0), lastTxMs(0), txBytes(0), rxBytes(0), discCount(0),
      frameErrs(0), droppedChunksOnReset(0) {
    UtilBaudSweep::Config sc;
    sc.pingSamplesPerBaud = config.pingSamplesPerBaud;
    sc.minAcceptRate = config.minAcceptRate;
    sc.expectedSamples = -1;
    baudSweep.configure(sc);

    hw.setEvents(*this);
    Log::log().info(TAG, "Init as %s", isMaster ? "Ping" : "Pong");

    {
        const int msgChunks = chunksForMsgLen((int)cfg.maxMsg);
        const int window = arqCache_.window();
        if (msgChunks > window) {
            Log::log().error(TAG,
                             "maxMsg=%u needs %d chunks > GBN window %d — "
                             "every ASYNC send will trip the window "
                             "admission guard; lower cfg.maxMsg, raise "
                             "MAX_CHUNK, or widen the window",
                             (unsigned)cfg.maxMsg, msgChunks, window);
        } else if (msgChunks * 2 > window) {
            Log::log().warning(
                TAG,
                "maxMsg=%u takes %d chunks — the window admission guard "
                "allows only one such message in flight at a time "
                "(window=%d)",
                (unsigned)cfg.maxMsg, msgChunks, window);
        }
    }
}

Link::~Link() = default;

void Link::resetSeq_unlocked() {
    txSeq = 0;
    // First-ever-session, ASYNC-ONLY: expect the peer's first
    // frame to be exactly cobsSeq 0 (seed rxSeq = COBS_SEQ_MAX so
    // classifyGap's normal expected=rxSeq+1-wrapped path computes
    // 0), instead of classifyGap's rxSeqSet=false bootstrap that
    // ASYNC gap-recovery makes it safe to seed the
    // expected-next seq to COBS_SEQ_MAX; SYNC's gap path
    // never advances rxSeq so the same seed desyncs the
    // session permanently. Scoped to ASYNC, !wasEverOk_.
    // classifyGap is untouched; pinned by
    // FirstAsyncSessionExpectsZeroTest.
    if (!wasEverOk_ && cfg.mode == AutoLinkConfig::Mode::ASYNC) {
        rxSeqSet = true;
        rxSeq = (uint8_t)COBS_SEQ_MAX;
        return;
    }
    // Default bootstrap: trust whatever arrives first
    // as the next seq. This can permanently lose
    // the first message if the peer's settle gate
    // drops seq 0; ASYNC's gap-recover path covers
    // it, so the default is safe for the modes that
    // ship today.
    rxSeqSet = false;
    rxSeq = 0;
}

uint8_t Link::reorderExpectedSeq() const {
    return (uint8_t)((rxSeq == (uint8_t)COBS_SEQ_MAX) ? 0 : rxSeq + 1);
}

void Link::begin() {
    hw.lock();
    sweep_.computeDwells(*this);
    hw.unlock();
    kickedOff_ = false;
    // Fresh boot: the link is not OK yet (no wire exchange
    // has happened) so kickoff()'s state-!=OK gate will fire
    // on the first call below. Once the link has reached OK
    // once, kickedOff_ is irrelevant: the gate is on link
    // state. Pinned by KickoffGatesOnStateTest.
    if (state == State::OK) {
        changeState_unlocked(State::SWP);
    }
    // A pending break suspicion from a peer-driven BREAK
    // (the master's kickoff arrives before the slave's
    // begin() completes) must not carry into the next OK
    // session. Pinned by BreakConfirmTest.
    breakSuspectMs_ = 0;
    breakSuspectSeen_ = 0;
    if (linkPaused_) {
        Log::log().info(TAG,
                        "begin: link initialised; kickoff deferred "
                        "(linkPaused=true)");
        return;
    }
    kickoff();
}

void Link::kickoff() {
    // A permanent kickedOff_ latch turned every recovery path
    // after the first wedge into a silent no-op. The start
    // button on the web UI fires kickoff() through
    // Ping::setPaused(false); the first deadlock the link
    // hits on a stale kickedOff_ makes the operator's Start
    // toggle a no-op until reboot. Gate on link state, not on
    // a one-shot latch: a locked (OK) link ignores kickoff,
    // and a SWP link re-arms the wire (re-fires break on
    // master, re-enters P1 on slave) on every call. Pinned by
    // KickoffGatesOnStateTest.
    if (state == State::OK) {
        Log::log().debug(TAG, "kickoff: link already OK; no-op");
        return;
    }
    if (linkPaused_) {
        Log::log().warning(TAG,
                           "kickoff: linkPaused_=true; refusing to fire "
                           "wire-side start. Call setLinkPaused(false) "
                           "first.");
        return;
    }
    kickedOff_ = true;
    if (isMaster) {
        hw.lock();
        reset_unlocked(false, false, ResetReason::Kickoff);
        hw.unlock();
        hw.sendBreak();
        Log::log().info(TAG, "kickoff: master sent break; entering P1");
    } else {
        hw.lock();
        changeState_unlocked(State::SWP);
        spdI = cfg.clampToMaxBauds() - 1;
        pingSample = 0;
        rxIdx = 0;
        // A pending break suspicion from a peer-driven
        // BREAK (the master's kickoff arrives before the
        // slave's begin() completes) must not carry into
        // the next OK session — it would fast-confirm the
        // first real post-lock BREAK and tear down a
        // healthy link. Pinned by BreakConfirmTest.
        breakSuspectMs_ = 0;
        breakSuspectSeen_ = 0;
        msgRx_.reset();
        frameRx.reset();
        baudSweep.resetAll();
        resetSeq_unlocked();
        sweep_.setPhase(SweepPhase::PHASE1);
        hw.unlock();
        hw.clearAppBuf();
        hw.setSpd(cfg.allowedBaudSafe(spdI));
        Log::log().info(TAG, "SWP Pong P1 baud[%d]=%lu", spdI,
                        (unsigned long)cfg.allowedBaudSafe(spdI));

        hw.startTimer(sweep_.dwells().phase2[0] + 200);
        Log::log().info(TAG, "kickoff: slave armed P1 listener");
    }
}

void Link::changeState_unlocked(State newState) {
    if (state != newState) {
        Log::log().debug(TAG, "%s -> %s", StateToStr(state),
                         StateToStr(newState));
        state = newState;
        // Mirror to the HAL: EspHal's UART event task runs outside
        // Link's lock and can't safely read `state` directly, but
        // needs to know whether a delivered BREAK is safe to flush
        // RX on. See EspHalUartEvent.h's UART_BREAK handling.
        hw.setOkState(newState == State::OK);
    }
}

void Link::reset_unlocked(bool count, bool preservePreferredBaud,
                          ResetReason reason) {
    // Captured before changeState below: several counters must
    // distinguish "an OK session ended" from "SWP-state churn
    // re-reset" (epoch slides, handshake races during a walk).
    bool endedOkSession = state == State::OK;
    // The trigger-reason log is unconditional so a future
    // unexplained OK -> SWP transition (debug-flooded log
    // sink, drop-after-stall) is traceable from the log even
    // if every per-loop log line above was dropped. Pinned by
    // ResetReasonDiagTest.
    Log::log().info(
        TAG, "reset_unlocked reason=%s count=%d preserve=%d",
        reason == ResetReason::Kickoff                 ? "Kickoff"
            : reason == ResetReason::UserDropLink      ? "UserDropLink"
            : reason == ResetReason::ErrThreshold      ? "ErrThreshold"
            : reason == ResetReason::ErrRate           ? "ErrRate"
            : reason == ResetReason::HealthWatchdog    ? "HealthWatchdog"
            : reason == ResetReason::GbnMaxRetx        ? "GbnMaxRetx"
            : reason == ResetReason::BaudUpgrade       ? "BaudUpgrade"
            : reason == ResetReason::GbnKeepRescue     ? "GbnKeepRescue"
            : reason == ResetReason::PeerEpochMismatch ? "PeerEpochMismatch"
                                                       : "?",
        count ? 1 : 0, preservePreferredBaud ? 1 : 0);
    resetReasonCounts_[(size_t)reason]++;
    resetCount_++;
    lastResetReason_ = reason;

    if (count && state == State::OK) {
        discCount++;
        // Same gate discCount uses, so the epoch moves in lockstep
        // with the disconnect count for a GENUINE restart of our
        // own session. A reset triggered by PeerEpochMismatch is
        // the opposite case: we're not restarting, we're syncing
        // up to a restart the peer already announced. Bumping our
        // own epoch here would hand the peer a "new" epoch on our
        // next keepalive PING; the peer runs this exact same
        // mismatch check, sees what looks like OUR restart, and
        // resets right back — two correctly-functioning detectors
        // handing one glitch back and forth forever. That's the
        // field's epoch slide (peer 0->1->2->3->4 on BOTH sides,
        // never converging) — not a peer that actually kept
        // restarting four times. Pinned by EpochBounceTest.
        if (reason != ResetReason::PeerEpochMismatch)
            sweepEpoch_++;
        uint32_t dnow = hw.nowMs();
        // Kickoff-noise BREAKs (pre-first-lock) must not arm the
        // post-lock quiet gate; only a working link that dropped does.
        // endedOkSession, not wasEverOk_: a "disc" is an OK link
        // dropping. wasEverOk_ is sticky-true forever after the
        // first lock, so SWP-state churn resets during a recovery
        // walk (peer epoch slides, handshake races) also counted
        // — the recovery cascade's own resets pushed recentDiscs_
        // to 5-7, tripping DISC_STORM_THRESHOLD and vetoing the
        // preserved fast path for the very recovery in progress.
        // Baud-quality evidence is only "the link locked and then
        // failed", which is exactly endedOkSession. Pinned by
        // DiscStormEscalationTest.
        if (wasEverOk_ && endedOkSession) {
            recentDiscs_ = (lastDiscMs_ != 0 && dnow - lastDiscMs_ < 10000)
                ? recentDiscs_ + 1
                : 1;
            lastDiscMs_ = dnow;
            // Track consecutive locks that produced zero
            // valid crossings — the peer-baud-mismatch
            // escalation signal. recvMsg / a CRC-valid ACK or
            // NAK clears it. Only count a session that lived
            // long enough for a crossing to have been POSSIBLE:
            // the post-lock quiet gate holds our own tx for
            // postLockQuietMs (x recentDiscs), so a session
            // torn down before quiet + one round trip could not
            // have produced the ACK that clears this counter no
            // matter how good the baud is. Counting those makes
            // the escalation self-sustaining: it kills each
            // PeerBaudMismatchTest pins the second-line
            // defence: the post-lock fair-chance window
            // must elapse before locksWithoutRecv_
            // increments, so a peer that takes longer
            // than the quiet window to land its first
            // frame doesn't trigger a re-sweep.
            uint32_t fairChanceMs =
                (uint32_t)(cfg.postLockQuietMs > 0 ? cfg.postLockQuietMs : 0) +
                (uint32_t)(2 * cfg.syncAckTimeoutMs);
            if (lockedAtMs_ != 0 &&
                (uint32_t)(dnow - lockedAtMs_) >= fairChanceMs)
                locksWithoutRecv_++;
        }
#ifdef ARDUINO

        Log::log().info(TAG, "resweep: disc=%lu freeHeap=%u",
                        (unsigned long)discCount,
                        (unsigned)esp_get_free_heap_size());
#endif
    }
    changeState_unlocked(State::SWP);

    spdI = 0;
    // Only the master fast-paths to the proven baud under a
    // routine preserve. The slave is now allowed the same
    // fast path on a HealthWatchdog-reason reset, because the
    // master's preservePreferredBaud fast path and the slave's
    // unconditional P1 walk run on uncoordinated clocks — a
    // slave that always restarts at P1-slowest essentially
    // never overlaps the master at the preserved baud inside
    // the P2 dwell budget, so the master falls back to
    // 9600 every time. A HealthWatchdog drop is presumed
    // line-quality (the baud itself is fine; the link just
    // went away), so the slave honoring the same preserved
    // baud on this reason lets both sides converge on the
    // proven baud without a full P1 walk. Pinned by
    // WireSimReConvergeTest.
    bool slaveMayPreserve = !isMaster && reason == ResetReason::HealthWatchdog;
    if (!preservePreferredBaud || (!isMaster && !slaveMayPreserve)) {
        preferredBaud_ = NO_PREFERRED_BAUD;
        // A from-scratch renegotiation invalidates the fast-baud
        // memory too, otherwise the upgrade would keep aiming at a
        // baud this pair can no longer sustain.
        bestProvenBaud_ = NO_PREFERRED_BAUD;
        baudUpgradeAtMs_ = 0;
        baudUpgradeAttempts_ = 0;
        resweepPrefPending_ = false;
    }
    // Per-round PONG tally reset. The fallback paths
    // consume this counter; clearing it here (in
    // addition to P1/P2/P3 entries) keeps the
    // invariant that a fresh session cannot inherit a
    // PONG from a prior one.
    sweepPongCount_ = 0;
    baudRetries_ = 0;
    pingSample = 0;
    rxIdx = 0;
    okCarryLen_ = 0;
    // Recovery reset (line trouble on a session that
    // stays logically alive) wipes ARQ bookkeeping
    // unconditionally; only a genuinely new session
    // (Kickoff, UserDropLink, PeerEpochMismatch — the
    // peer says it restarted) re-seeds the receiver's
    // in-progress parse. The forward-delta recovery case
    // is reclassified to HealthWatchdog before reaching
    // here, see processCtrlFrame_unlocked.
    msgRx_.reset();
    frameRx.reset();
    baudSweep.resetAll();
    errs = 0;
    emptySweeps = 0;
    errWindowStartMs_ = hw.nowMs();
    errWindowCount_ = 0;
    lastRxMs = hw.nowMs();
    txRejFirstMs_ = txRejLastMs_ = 0;
    // Surface, don't hide: everything still pending in
    // the ARQ window is about to be wiped un-delivered.
    // See the Stats field comment in Link.h.
    //
    // Gate: only wipe the ARQ window when we
    // transitioned out of an OK session. SWP -> SWP
    // resets have no in-flight data to lose. Pinned by
    // WipeGatedToOkTransitionTest.
    if (endedOkSession) {
        if (arq_.gbnActive() && arq_.pendingCount() > 0) {
            droppedChunksOnReset += (uint64_t)arq_.pendingCount();
            Log::log().warning(TAG,
                               "reset wiping %d accepted-undelivered ARQ "
                               "chunk(s) (droppedChunksOnReset=%llu total)",
                               arq_.pendingCount(),
                               (unsigned long long)droppedChunksOnReset);
        }
        arq_.clearAll();
        resetSeq_unlocked();
        lastAckSeq_ = 0xFF;
        lastNakSeq_ = 0xFF;
        lastRxSeq_ = 0xFF;
        holdNakActive_ = false;
        holdNakSeq_ = 0xFF;
        postLockQuietLogged_ = false;
        gbnAttempts_ = 0;
        gbnBackoffMs_ = 0;
        gbnLastRetxBase_ = 0xFF;
        gbnBaseStuckSinceMs_ = 0;
        gbnBaseStuckTrackedSeq_ = 0xFF;
        consecutiveKeep_ = 0;
    }
    // A pending break suspicion belongs to the session that just
    // ended, whichever path triggered this reset — must not carry
    // into the next OK session and fire against a fresh lockedAtMs_.
    breakSuspectMs_ = 0;
    breakSuspectSeen_ = 0;
    hw.clearAppBuf();
    // resweepPrefPending_ arms P3's timeout to fall back to a full
    // P1 walk if the fast re-lock misses. recentDiscs_ is also the
    // circuit breaker for a baud that keeps dropping: after
    // DISC_STORM_THRESHOLD fast disconnects inside the 10 s window,
    // the "proven" baud isn't proven — stop fast-relocking back
    // into whatever's causing the churn and force a full sweep.
    // Pinned by DiscStormEscalationTest.
    //
    // locksWithoutRecv_ is the second circuit breaker: once
    // applyHealth_unlocked's peer-baud-mismatch escalation
    // has already fired once on this counter, the next
    // preserve=true reset must NOT fast-path back into the
    // same wrong baud. recentDiscs_ only resets inside the
    // 10 s window, but a slave camp re-arm (5 s) + P3
    // dwell + watchdog horizon can push the cycle past 10 s,
    // pinning recentDiscs_ at 1 forever — and recentDiscs_
    // alone was the gap the prior-release escalation sat inside.
    // The mismatch verdict itself clears preferredBaud_
    // (see LinkTimersOk.cpp:applyHealth_unlocked), so this
    // is the second-line defense that holds even if a
    // future caller passes preserve=true without clearing.
    // Pinned by PeerBaudMismatchTest Pin 4.
    if (preservePreferredBaud && wasEverOk_ &&
        preferredBaud_ != NO_PREFERRED_BAUD &&
        preferredBaud_ < (uint8_t)cfg.clampToMaxBauds() &&
        recentDiscs_ < DISC_STORM_THRESHOLD &&
        locksWithoutRecv_ < kPeerBaudMismatchThreshold) {
        spdI = preferredBaud_;
        sweep_.enterPhase3(*this, preferredBaud_);
        resweepPrefPending_ = true;
        resweepPrefAttempts_ = 0;
        resweepPrefDeadlineMs_ = hw.nowMs() + resweepPrefBudgetMs_unlocked();
    } else {
        sweep_.enterPhase1(*this);
        resweepPrefPending_ = false;
        resweepPrefAttempts_ = 0;
        resweepPrefDeadlineMs_ = 0;
    }
    // arqCache_.clearAll() and hw.discardTx() are
    // unconditional: the cache holds the actual chunk
    // bytes; on a SWP->SWP reset (epoch slide) the
    // cache might still hold stale bytes from a prior
    // failed lock, and a fresh lock needs a clean
    // cache. The arq_ bookkeeping is gated above
    // (WipeGatedToOkTransitionTest), but the cache
    // clear is paired with hw.discardTx() so the TX
    // path sees the same shape on every entry.
    // A preserve gate that split window bookkeeping
    // from chunk bytes left the GBN window permanently
    // full with no data behind it.
    // behind it.
    arqCache_.clearAll();
    hw.discardTx();
    // Heap log: every reset_unlocked exit gets a
    // freeHeap reading so the operator can spot a
    // pre-allocate vs honest-drop leak. Pinned by
    // HeapLogAtResetTest.
#ifdef ARDUINO
    Log::log().info(TAG, "reset exit: freeHeap=%u (endedOk=%d)",
                    (unsigned)esp_get_free_heap_size(), (int)endedOkSession);
#endif
}

void Link::getStats(Stats &s) const {
    hw.lock();
    s.tx = txBytes;
    s.rx = rxBytes;
    s.discCount = discCount;
    s.frameErrs = frameErrs;
    s.droppedChunksOnReset = droppedChunksOnReset;
    s.postLockQuietDrops = postLockQuietDrops_;
    s.rateLimitedDrops = rateLimitedCount_;
    s.settleDrops = settleDrops_;
    hw.unlock();
}
void Link::resetStats() {
    hw.lock();
    txBytes = rxBytes = 0;
    hw.unlock();
}
void Link::resetErrors() {
    hw.lock();
    discCount = frameErrs = droppedChunksOnReset = 0;
    errWindowCount_ = 0;
    errWindowStartMs_ = hw.nowMs();
    hw.unlock();
}
void Link::resetDiag() {
    hw.lock();
    gaps = stale = lostMsgs = 0;
    hw.unlock();
}

State Link::getState() const {
    hw.lock();
    State s = state;
    hw.unlock();
    return s;
}
int Link::getErrCount() const {
    hw.lock();
    int e = errs;
    hw.unlock();
    return e;
}
int Link::getCurrentSpdIndex() const {
    hw.lock();
    int i = spdI;
    hw.unlock();
    return i;
}
uint32_t Link::getCurrentBaud() const {
    hw.lock();
    uint32_t b = (spdI >= 0 && spdI < cfg.clampedCount())
        ? cfg.allowedBaudSafe(spdI)
        : 0;
    hw.unlock();
    return b;
}
void Link::getDiag(Diag &d) const {
    hw.lock();
    d.txSeq = txSeq;
    d.rxSeqSet = rxSeqSet;
    d.rxSeq = rxSeq;
    d.gaps = gaps;
    d.stale = stale;
    d.lostMsgs = lostMsgs;
    d.baudRetries = (uint64_t)baudRetries_;
    d.preferredBaud = preferredBaud_;
    for (size_t i = 0; i < (size_t)ResetReason::ResetReasonCount; i++) {
        d.resetReasons[i] = resetReasonCounts_[i];
    }
    d.resetCount = resetCount_;
    d.breaksSuppressed = breaksSuppressed_;
    hw.unlock();
}

} // namespace autolink
