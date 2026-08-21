
#include "al/link/Link.h"
#include "al/link/arq/ArqCache.h"
#include "al/link/arq/LinkArq.h"
#include "al/link/sweep/LinkSweep.h"
#include "al/util/log/Log.h"

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
    // F7: bump txSeqLap_ (do not zero it) so
    // a post-reset seq can never carry a
    // pre-reset lap value. The two halves of
    // the same identifier (txSeq / txSeqLap_)
    // now reset together — same shape as the
    // rxSeqWrap_ / holdNakWrap_ pairing in
    // endedOkSession (D11). Pinned by
    // TxSeqLapBumpsOnResetTest.
    txSeqLap_++;
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

bool Link::begin() {
    // J3 + J4: drive the HAL's
    // config-aware begin
    // through the IHal&
    // reference. The facade
    // already does this in
    // production (under
    // #ifdef ARDUINO); the
    // host tests need the same
    // path so the
    // MockHal::begin(cfg)
    // override is reachable
    // when Link is constructed
    // from a non-concrete
    // IHal. The HAL is
    // responsible for
    // idempotency
    // (MockHal: beginCalled_
    // guard; EspHal: `if
    // (running) return;`).
    hw.begin(cfg);
    // I1: gate on the ring the HAL actually installed, not the theoretical
    // floor the config implies. EspHal's begin() runs the floor through
    // capFloorByHeap with the live free-heap reading; a tight heap can clamp
    // the ring to 0. hw.txRingSize() reports the installed value (EspHal:
    // tx_buffer_size_; MockHal: txCap). 0 means the HAL doesn't model a ring
    // — fall back to the theoretical floor (the historical H1 path) so HALs
    // that haven't upgraded still get a useful gate. Pinned by
    // BeginRejectsHeapClampedRingTest.
    size_t hwRing = hw.txRingSize();
    size_t installFloor =
        hwRing > 0 ? hwRing : uartTxBufferFloorCapped(cfg, SIZE_MAX, 0);
    if (installFloor < (size_t)kWorstCaseCobsFrame) {
        Log::log().error(
            TAG,
            "begin: installed ring=%d < kWorstCaseCobsFrame=%d — "
            "ring cannot fit a single COBS frame. SYNC mode wedges; "
            "ASYNC mode wedges mid-message. Raise cfg.txBufferSize or "
            "free heap to at least kWorstCaseCobsFrame=%d",
            (int)installFloor, (int)kWorstCaseCobsFrame,
            (int)kWorstCaseCobsFrame);
        // H2: signal the failure to the caller. Force the link into SWP so
        // the app sees the broken state on its next state read.
        hw.lock();
        state = State::SWP;
        hw.unlock();
        return false;
    }
    // AL87-18: window() is a compile-time default
    // (AUTOLINK_ARQ_PIPELINE_WINDOW) picked without knowledge of the
    // actual installed ring — a heap-clamped or config-shrunk ring
    // can hold fewer worst-case frames than the window would admit
    // in flight at once. sendMsg's admission gate only checks
    // "does inflight+chunks fit the window", not "would the ring
    // physically hold that many bytes", so a full window burst hit
    // "TX ring can't fit header" on an install this narrow — the
    // frames were admitted, then had nowhere to go. Clamp down to
    // what installFloor can hold; setWindow() never grows past the
    // window the cache was constructed with, so a roomy ring is a
    // no-op here.
    //
    // Per-chunk divisor must match whichever assumption
    // uartTxBufferFloor applied when sizing this same ring, or the
    // clamp under-counts a ring that was deliberately sized to
    // hold exactly chunksForMsgLen(maxMsg) chunks and
    // false-rejects the maxMsg it was provisioned for. SYNC's
    // floor reserves a whole kWorstCaseCobsFrame per chunk (plus
    // one); ASYNC's floor uses the looser MAX_CHUNK+4 per-chunk
    // estimate. Pinned by PipelineWindowClampedToTxRingTest.
    int perChunkFloor = cfg.mode == AutoLinkConfig::Mode::SYNC
        ? kWorstCaseCobsFrame
        : (MAX_CHUNK + 4);
    int ringFrames = (int)(installFloor / (size_t)perChunkFloor);
    if (ringFrames < 1)
        ringFrames = 1;
    // AL89-4: receiver-capacity clamp. A full
    // GBN window in flight (32×250 B = 8000 B)
    // overruns a 4108 B receiver stream buffer
    // every time — the receiver's appBufFree()
    // check in LinkRx.cpp's all-or-nothing
    // Forward path rejects the overflow frames
    // and re-NAKs them in a 16 ms loop, the
    // sender's same-event dedup window (8 ms) is
    // shorter than the NAK cadence so it re-fires
    // a full-window resend for every NAK, and the
    // peer's NAK-count climbs without the base
    // ever advancing. The window the ring can
    // hold must not exceed the window the
    // receiver can accept. Use the same
    // streamBufferFloor() the receiver was sized
    // against so the two clamps never drift apart.
    // Pinned by ArqWindowClampedToReceiverTest.
    size_t rxBuf = hw.rxRingSize();
    if (rxBuf == 0)
        rxBuf = streamBufferFloor(cfg);
    int rxFrames =
        (rxBuf > 0) ? (int)(rxBuf / (size_t)(MAX_CHUNK + MSG_HDR)) : 0;
    if (rxFrames > 0 && ringFrames > rxFrames)
        ringFrames = rxFrames;
    arqCache_.setWindow(ringFrames);
    // AL88-7: the clamp above was previously invisible in the field
    // log — an operator had to derive it from txBuf/perChunk by
    // hand. Log it plainly, and flag the case that actually costs
    // throughput: a ring too small to hold the full compile-time
    // window, which floors the GBN pipeline depth for the session.
    if (ringFrames < AUTOLINK_ARQ_PIPELINE_WINDOW) {
        Log::log().info(TAG,
                        "begin: arqWindow=%d (clamped from %d by "
                        "installed TX ring=%d B / RX buf=%d B, "
                        "perChunkFloor=%d B)",
                        ringFrames, AUTOLINK_ARQ_PIPELINE_WINDOW,
                        (int)installFloor, (int)rxBuf, perChunkFloor);
    } else {
        Log::log().info(TAG, "begin: arqWindow=%d (unclamped)", ringFrames);
    }
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
        return true;
    }
    kickoff();
    return true;
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
    // A fresh Kickoff is this session's own restart — the one
    // case that should get a clean first-storm grace on the P3
    // camp again. Pinned by PostSoakFieldFixesTest (AL-07 pin).
    breakStormSeen_ = false;
    if (isMaster) {
        hw.lock();
        reset_unlocked(false, false, ResetReason::Kickoff);
        // Kickoff is a GENUINE restart of our own session, but the
        // count=false path in reset_unlocked exists to avoid
        // touching discCount_ on a never-was-OK first boot. That
        // gate is correct for discCount_; it's wrong for
        // sweepEpoch_ — the field's master kicked off with
        // epoch=0 and never bumped it, while the slave (below)
        // climbed to epoch=4 across its own kickoff/recovery
        // cycle. The slave's epoch-mismatch check then read the
        // master as a stuck-at-zero peer and fired
        // PeerEpochMismatch on every post-lock PING, with the
        // master never aware the slave had re-launched. Bump
        // epoch here, outside the count && state==OK gate, so a
        // fresh kickoff is observable to the peer even on a first
        // boot. Runtime-pinned by EpochBounceTest.cpp and
        // PeerResyncOnMissedBreakTest.cpp, both of which assert
        // sweepEpochForTest()==1 immediately after begin().
        sweepEpoch_++;
        hw.unlock();
        hw.sendBreak();
        Log::log().info(TAG, "kickoff: master sent break; entering P1");
    } else {
        hw.lock();
        changeState_unlocked(State::SWP);
        spdI = cfg.clampToMaxBauds() - 1;
        // Bump on slave kickoff too, for symmetry with the master
        // branch above. The slave never produces
        // PeerEpochMismatch against its own restart, but a slave
        // that didn't bump here would still present the master's
        // kickoff bump (epoch=0 -> epoch=1) as a mismatch on its
        // own first post-lock PING. Pinned by
        // EpochBounceTest.cpp & PeerResyncOnMissedBreakTest.cpp (same runtime
        // pins).
        sweepEpoch_++;
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
        msgRxStartedMs_ = 0;
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
    //
    // Reason -> string is a table indexed by the enum, not a
    // ternary chain. The prior ternary chain omitted
    // PeerBaudMismatch (=8) entirely, so the field saw
    // `reason=?` three times for the BREAK-storm reset — the
    // exact reason an operator most needed to see named. The
    // static_assert below fails the build the moment a new
    // reason is added to the enum without a matching table row,
    // instead of silently falling through to "?" again. Pinned
    // by ResetReasonTableIndexedByEnumTest.
    static constexpr const char *kReasonNames[] = {
        "Kickoff",           // 0
        "UserDropLink",      // 1
        "ErrThreshold",      // 2
        "ErrRate",           // 3
        "HealthWatchdog",    // 4
        "GbnMaxRetx",        // 5
        "GbnKeepRescue",     // 6
        "PeerEpochMismatch", // 7
        "PeerBaudMismatch",  // 8
        "BaudUpgrade",       // 9
    };
    static_assert(sizeof(kReasonNames) / sizeof(kReasonNames[0]) ==
                      (size_t)ResetReason::ResetReasonCount,
                  "kReasonNames table size != ResetReasonCount — "
                  "add a row for the new reason");
    size_t reasonIdx = (size_t)reason;
    const char *reasonStr =
        (reasonIdx < sizeof(kReasonNames) / sizeof(kReasonNames[0]))
        ? kReasonNames[reasonIdx]
        : "?";
    Log::log().info(TAG, "reset_unlocked reason=%s count=%d preserve=%d",
                    reasonStr, count ? 1 : 0, preservePreferredBaud ? 1 : 0);
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
    msgRxStartedMs_ = 0;
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
        // AL89-5: clear the hold-NAK drain
        // snapshot on session reset so a fresh
        // hold on the new session starts from
        // the current appBufFree rather than a
        // stale value from a prior session.
        holdNakFreeAtSet_ = 0;
        // D11: reset the wrap counters that
        // qualify holdNakSeq_ so a hold-NAK
        // deferred from a prior session can't fire
        // on the first message of a new session
        // when the new rxSeq_ matches the held
        // seq under the same wrap index. Reset
        // both wraps together — they live and die
        // with the session. Pinned by
        // HoldNakWrapSessionResetTest (a fresh
        // session after a hold-NAK deferral must
        // not re-fire the hold on a same-wire seq).
        holdNakWrap_ = 0;
        rxSeqWrap_ = 0;
        postLockQuietLogged_ = false;
        // AL89-9: re-arm the first-TX marker
        // on session reset so a fresh session
        // starts with the evidence gate closed
        // and the very first send admitted.
        // Sentinel = lockedAtMs_ (which is also
        // reset to 0 here, so the marker is
        // already 0 by construction — the
        // explicit stamp makes the intent
        // obvious at the call site).
        postLockFirstTxDone_ = 0;
        // AL90-2: clear the peer-response
        // marker too. Otherwise the first
        // re-lock after one ACK has been seen
        // latches the gate open for the
        // process lifetime.
        firstPeerResponseSeen_ = false;
        gbnAttempts_ = 0;
        gbnBackoffMs_ = 0;
        gbnLastRetxBase_ = 0xFF;
        gbnLastResendBase_ = 0xFF;
        // D13: reset the resend source flag so a
        // session teardown can't carry a stale
        // Rto / Nak into the next session's
        // applyRetx call. Pinned by
        // SessionResetsResendSourceTest (a new
        // session after a held NAK + RTO must
        // not have the source flag set to either
        // value when its first gbnResend fires).
        resendSource_ = ResendSource::Rto;
        gbnLastResendMs_ = 0;
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
        locksWithoutRecv_ < kPeerBaudMismatchThreshold &&
        // Once a BREAK-storm window has fired this session, the
        // preserved baud is no longer proven — re-entering the
        // camp would re-arm the exact master-walks/slave-camps
        // mismatch that produced the storm. The first storm is
        // the proof; the camp isn't worth a second try. Pinned by
        // PostSoakFieldFixesTest (AL-07 pin).
        !breakStormSeen_) {
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
    s.badHeaderErrs = badHeaderErrs_;
    s.overLenErrs = overLenErrs_;
    s.crcFailErrs = crcFailErrs_;
    s.droppedChunksOnReset = droppedChunksOnReset;
    s.postLockQuietDrops = postLockQuietDrops_;
    s.rateLimitedDrops = rateLimitedCount_;
    s.gbnWindowFullDrops = gbnWindowFullDrops_;
    s.poolExhaustDrops = poolExhaustDrops_;
    s.txRingStallDrops = txRingStallDrops_;
    s.settleDrops = settleDrops_;
    s.acksSent = acksSent_;
    s.naksSent = naksSent_;
    s.holdNaksLiveness = holdNaksLiveness_;
    s.resyncDroppedBytes = resyncDroppedBytes_;
    s.staleAmbiguous = staleAmbiguous_;
    s.txBlockedMs = txBlockedMs_;
    s.rxOverflows = hw.rxOverflowCount();
    s.rxFrameErrs = hw.rxFrameErrCount();
    s.logDrops = Log::log().droppedLines();
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
    badHeaderErrs_ = overLenErrs_ = crcFailErrs_ = 0;
    lastBadHeaderLogMs_ = 0;
    gbnWindowFullDrops_ = 0;
    poolExhaustDrops_ = 0;
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

// Per-tx wall-time accumulation routed from EspHal::tx().
// The HAL measures dt around uart_write_bytes and forwards
// it via onTxBlockedNote; the link layer is the only
// place that owns the running aggregate, since the HAL
// side doesn't know the periodic stats line.
//
// Every call site for hw.tx() is on the link lock
// (sendSweepFrame_unlocked, buildAndTxCobsFrame_unlocked,
// txSmallCobs_unlocked). Taking the lock here would
// self-deadlock the ESP32 build (the UART event
// task on the same Link holds the same non-recursive
// mutex; the very first sendAckFrame_unlocked from
// onRx hangs the task permanently). The accumulator
// is a single uint64_t and the access is word-sized
// on every platform the project ships to, so the
// unlocked accumulate is safe — the worst case is a
// torn write that adds a millisecond either side of
// the true value, which is well below the periodic
// stats line's reporting precision. Pinned by
// TxBlockedNoteNoReentrantLockTest (regression:
// re-introducing the lock trips the test under
// HostHalWithLock that re-checks hw.lock() on entry).
void Link::onTxBlockedNote(uint32_t ms) { txBlockedMs_ += ms; }

} // namespace autolink
