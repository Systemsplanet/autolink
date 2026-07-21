
#include "al/link/Link.h"
#include "al/link/arq/ArqCache.h"
#include "al/link/timers/LinkHealth.h"
#include "al/link/arq/LinkArq.h"
#include "al/link/sweep/LinkDecision.h"
#include "al/link/sweep/LinkSweep.h"
#include "al/util/Log.h"

static constexpr const char *TAG = "AutoLink";

namespace autolink {
static_assert(
    MAX_CHUNK + MSG_HDR <= ArqCache::POOL_BUF_MAX,
    "MAX_CHUNK + MSG_HDR > ArqCache::POOL_BUF_MAX: bump POOL_BUF_MAX or shrink MAX_CHUNK");


void Link::onTimer() {
    hw.lock();
    // Consume the deferred storm escalation. Must run
    // BEFORE the OK/SWP tick so a P1 walk produced here
    // supersedes any keepalive sendMsg. routed through
    // reset_unlocked so preferredBaud_, the ARQ
    // bookkeeping and the counters all clear in one
    // place.
    if (breakStormPending_) {
        breakStormPending_ = false;
        Log::log().warning(TAG,
                           "BREAK storm: clearing preferredBaud_ "
                           "and forcing P1 walk");
        reset_unlocked(true, false, ResetReason::PeerBaudMismatch);
        // Capture `state` under the lock after the
        // reset; the RX task can flip it between
        // `hw.unlock()` and the read.
        State sAfterReset = state;
        hw.unlock();
        if (sAfterReset != State::OK)
            hw.sendBreak();
        return;
    }
    bool brk = false;
    State s = state;
    if (s == State::OK)
        brk = onTimerOk_unlocked();
    else if (s == State::SWP)
        onTimerSwp_unlocked();
    hw.unlock();
    if (brk)
        hw.sendBreak();
}

void Link::retxSeq_unlocked(uint8_t seq) {
    const uint8_t *buf = nullptr;
    int len = 0;
    if (!arqCache_.slotInUse(seq)) {
        Log::log().info(TAG,
                        "ARQ retx cobsSeq=%u cache miss (chunk already "
                        "delivered); pending bit left to time out",
                        (unsigned)seq);
    } else if (arqCache_.peekForRetx(seq, &buf, &len)) {
        Log::log().warning(TAG, "ARQ retx cobsSeq=%u (%d bytes) — verbatim",
                           (unsigned)seq, len);
        resendCobsFrame_unlocked(seq, buf, len);
    } else {
        Log::log().info(TAG,
                        "ARQ retx cobsSeq=%u (no pool buf) — "
                        "verbatim 0 bytes",
                        (unsigned)seq);
        resendCobsFrame_unlocked(seq, nullptr, 0);
    }
}

// The GBN receiver discards out-of-order frames, so recovery
// needs the base *and* the in-order prefix behind it — a
// single-frame resend never makes progress. But a full-window
// resend on a stuck base saturates the UART and starves the
// peer's reverse ACK path, which is the death-spiral seed;
// hence the per-RTO burst cap. Later RTOs, spaced by
// decideGbnBackoff, replay the next prefix. Pinned by
// GbnBurstCapTest.


HealthAction Link::applyHealth_unlocked(uint32_t now) {
    HealthState h;
    h.rejFirstMs = txRejFirstMs_;
    h.rejLastMs = txRejLastMs_;
    // lastValidRxMs (CRC-validated frames only), not
    // lastRxMs (any byte). lastRxMs is stamped on a noise
    // byte that happens to land on a 0xAA 0x55 preamble
    // and clear CTRL carry state; the health machine
    // reading that as ongoing traffic would let a
    // dead-noise link ride the silent-peer / asym
    // watchdogs forever. Pinned by LastValidRxMsTest.
    h.lastRxMs = lastValidRxMs;
    h.lastAnyRxMs = lastRxMs;
    h.lastTxMs = lastTxMs;
    // RTO is baud-aware, not a static constant: a 9600-baud
    // locked link needs a much longer RTO than a 512000-baud
    // one (round-trip time at 9600 is ~2.1 ms per 5-byte
    // frame, at 512000 it's ~0.04 ms). The prior static
    // cfg.syncAckTimeoutMs made DropAsymIdle fire faster than
    // the time needed to actually transmit the queued payload
    // at the locked baud — every post-BREAK resweep at 9600
    // doomed the very first send before it could land on the
    // peer. Pinned by LinkHealthBaudAwareTest.
    h.rtoMs = (uint32_t)baudAwareRtoMs_unlocked();
    h.pending = arq_.pendingCount();
    h.frameErrs = frameErrs;
    h.poolFull = !arqCache_.hasRoom();
    h.sync = (cfg.mode == AutoLinkConfig::Mode::SYNC);
    // GBN-retx status mirrored from the link. The
    // DropAsymIdle gate suppresses its fire while
    // retx repair is in flight with budget
    // remaining — the watchdog's 2xRTO horizon
    // (~1 s) is shorter than the ARQ layer's
    // maxRetx * syncAckTimeoutMs budget (2.5 s at
    // defaults), and a mid-repair drop leaves a
    // stuck base. Pinned by LinkHealthTest.
    h.gbnActive = arq_.gbnActive();
    h.gbnAttempts = gbnAttempts_;
    h.gbnBudgetOpen = gbnAttempts_ < (int)cfg.maxRetx;
    // ACK-owed direction stamp for the peer-stalled
    // watchdog. ackRxMs is the same CRC-valid RX
    // clock as lastRxMs (Link::lastValidRxMs is the
    // source of truth) but carried under a dedicated
    // field so the watchdog can suppress the
    // mutual-quiet cross-check while still respecting
    // the ack-direction gate. peerStalledMs is the
    // baud-derived threshold — 2 s floor at high
    // baud, 2x the window-drain at 9600. Both fields
    // are passed through decideHealth's existing
    // HealthState (the helper is a pure decision
    // function and the link owns the baud lookup).
    h.ackRxMs = lastValidRxMs;
    uint32_t lockedBaud = (spdI >= 0 && spdI < cfg.clampedCount())
        ? cfg.allowedBaudSafe(spdI)
        : 0;
    h.peerStalledMs = healthPeerStalledMs(lockedBaud, h.pending);

    HealthAction a = cfg.idleTimeoutMs > 0
        ? decideHealth(h, now, cfg.idleTimeoutMs,
                       healthDeadPeerMs(cfg.idleTimeoutMs), h.rtoMs)
        : HealthAction::Keep;
    // Peer-baud-mismatch escalation runs after decideHealth
    // (so the standard drop verdicts still fire on real
    // drops — silence, pool-exhaust, etc.) but BEFORE the
    // Keep return so a high locksWithoutRecv_ forces a
    // reset even when the rest of the health machine sees
    // nothing wrong. Pinned by PeerBaudMismatchTest.
    // Same fair-chance horizon as the increment site in
    // reset_unlocked: never escalate before the current session
    // has been up long enough that the quiet gate has released tx
    // and a round trip could have completed — otherwise the
    // escalation fires inside the quiet window on state carried
    // over from prior sessions, before this session could produce
    // the crossing that clears it. Pinned by PeerBaudMismatchTest.
    uint32_t pbmFairChanceMs =
        (uint32_t)(cfg.postLockQuietMs > 0 ? cfg.postLockQuietMs : 0) +
        (uint32_t)(2 * cfg.syncAckTimeoutMs);
    bool pbmFairChance =
        lockedAtMs_ != 0 && (uint32_t)(now - lockedAtMs_) >= pbmFairChanceMs;
    if (a == HealthAction::Keep && pbmFairChance &&
        shouldEscalatePeerBaudMismatch(locksWithoutRecv_,
                                       kPeerBaudMismatchThreshold)) {
        a = HealthAction::DropPeerBaudMismatch;
    }
    if (a == HealthAction::Keep)
        return a;
    switch (a) {
    case HealthAction::DropTxStall:
        Log::log().warning(TAG,
                           "tx backpressure stall -> drop "
                           "(stalledMs=%lu arqPending=%d)",
                           (unsigned long)(now - h.rejFirstMs), h.pending);
        break;
    case HealthAction::DropAsymIdle:
        // The warning must carry its evidence so the
        // operator can disambiguate the clock-stale
        // fire (live CRC-valid RX from a slow peer)
        // from a genuine asymmetric-idle wedge
        // without rxAge/txAge/pending/rto in the
        // line. Pinned by LinkHealthTest log-hygiene
        // pin.
        Log::log().warning(
            TAG,
            "asymmetric idle -> drop "
            "(rxAge=%lu txAge=%lu pending=%d rtoMs=%lu "
            "rxIdleFloor=%d lastValidRxMs=%lu lastRxMs=%lu)",
            (unsigned long)(now - h.lastRxMs),
            (unsigned long)(now - h.lastTxMs), h.pending,
            (unsigned long)h.rtoMs, healthFastIdleRxMsAtBaud(h.rtoMs),
            (unsigned long)lastValidRxMs, (unsigned long)lastRxMs);
        break;
    case HealthAction::DropIdle:
        Log::log().warning(TAG,
                           "idle watchdog -> drop "
                           "(rxAge=%lu txAge=%lu idleTimeoutMs=%d "
                           "arqPending=%d frameErrs=%lu)",
                           (unsigned long)(now - h.lastRxMs),
                           (unsigned long)(now - h.lastTxMs), cfg.idleTimeoutMs,
                           h.pending, (unsigned long)h.frameErrs);
        break;
    case HealthAction::DropDeadLink:
        Log::log().warning(TAG,
                           "dead-link watchdog -> drop "
                           "(rxAge=%lu txAge=%lu idleTimeoutMs=%d "
                           "arqPending=%d sync=%d)",
                           (unsigned long)(now - h.lastRxMs),
                           (unsigned long)(now - h.lastTxMs), cfg.idleTimeoutMs,
                           h.pending, h.sync ? 1 : 0);
        break;
    case HealthAction::DropSilentPeer:
        Log::log().warning(TAG,
                           "silent-peer watchdog -> drop "
                           "(rxAge=%lu deadPeerMs=%d idleTimeoutMs=%d "
                           "arqPending=%d frameErrs=%lu)",
                           (unsigned long)(now - h.lastRxMs),
                           healthDeadPeerMs(cfg.idleTimeoutMs),
                           cfg.idleTimeoutMs, h.pending,
                           (unsigned long)h.frameErrs);
        break;
    case HealthAction::DropPeerStalled:
        // The peer-stalled watchdog fires when our
        // own pending ARQ is non-zero AND the
        // peer's last CRC-valid frame is past the
        // baud-derived threshold. The slave's
        // echo traffic keeps lastTxMs fresh so
        // DropDeadLink's mutual-quiet gate never
        // trips; the ack-direction stamp (ackRxMs)
        // ages independently. Log carries the
        // baud-derived window so the operator can
        // see the threshold the verdict used.
        Log::log().warning(TAG,
                           "peer-stalled watchdog -> drop "
                           "(ackRxAge=%lu peerStalledMs=%d pending=%d baud=%lu "
                           "idleTimeoutMs=%d)",
                           (unsigned long)(now - h.ackRxMs), h.peerStalledMs,
                           h.pending, (unsigned long)lockedBaud,
                           cfg.idleTimeoutMs);
        break;
    case HealthAction::DropPeerBaudMismatch:
        // N consecutive successful locks each produced
        // zero valid application frames before the next
        // reset — the peer is on a different baud than
        // the one we just locked at. Clear preferredBaud_
        // before the reset so the fast-path condition in
        // reset_unlocked cannot engage even on slow
        // reset cycles (>10 s) where recentDiscs_ never
        // hits DISC_STORM_THRESHOLD. The
        // locksWithoutRecv_ < kPeerBaudMismatchThreshold
        // gate inside reset_unlocked is the second-line
        // defense for any future caller that passes
        // preserve=true without clearing here. Pinned by
        // PeerBaudMismatchTest Pin 4.
        preferredBaud_ = NO_PREFERRED_BAUD;
        resweepPrefPending_ = false;
        Log::log().warning(TAG,
                           "peer-baud-mismatch -> drop "
                           "(locksWithoutRecv=%d threshold=%d)",
                           locksWithoutRecv_, kPeerBaudMismatchThreshold);
        break;
    default:
        Log::log().warning(TAG, "ARQ pool exhausted (pending=%d) -> drop",
                           h.pending);
        break;
    }
    // The master must preserve the proven baud here too,
    // symmetric with the slave's BREAK-triggered reset.
    // The prior shape unconditionally passed
    // preservePreferredBaud=false, so a master health
    // drop walked P1 from 9600 while the slave camped
    // P3 at 512000 — mutual deadlock (master sat in
    // P1 for 16-20 s, never locked until manual
    // re-kickoff). The existing recentDiscs_ <
    // DISC_STORM_THRESHOLD guard inside reset_unlocked
    // prevents camping on a genuinely-bad baud, and
    // the slave's bounded P3 camp gives the slave a
    // coordinated P1 fall. Pinned by
    // WireSimReConvergeTest.
    reset_unlocked(true, /*preservePreferredBaud=*/true,
                   ResetReason::HealthWatchdog);
    return a;
}

// GBN's only retransmit driver: RTO on the oldest unacked. A
// NAK matching gbnBase_ fires the same path early; this is the
// backstop for a lost NAK. Returns true if the caller must
// sendBreak (link was dropped).
bool Link::sweepRetx_unlocked(uint32_t now) {
    if (cfg.mode == AutoLinkConfig::Mode::SYNC || !arq_.gbnActive())
        return false;
    // Per-tick baud-aware recompute. The cached
    // value from lockOk_ only saw pending=0 and
    // pinned the threshold to the floor; a real
    // 32-chunk 250 B window at 9600 (~8.3 s drain)
    // needs the in-flight pendingCount to drive the
    // drain estimate, otherwise honest-drop trips at
    // syncAckTimeoutMs = 500 ms (~1/8 of the drain
    // time). Skipped while a test override is armed
    // (GbnBackoffTest Pin 4) so the test-seeded value
    // survives. Pinned by BaudAwareStuckThresholdTest.
    if (!gbnBaseStuckThresholdOverridden_)
        gbnBaseStuckThresholdMs_ = baudAwareStuckThresholdMs_unlocked();
    // CPU-stall detector: if the OK-timer task itself
    // was starved for >~3x the tick interval between
    // this and the previous tick, then (a) the
    // gbnBaseStuckSinceMs_ clock has been ticking
    // without the link layer being able to do
    // anything about it and (b) the missing ACKs
    // that drove the stuck verdict are likely
    // sitting in the UART RX FIFO right now, just
    // undelivered because the link lock was held
    // elsewhere. The field log showed exactly this
    // shape: a 900 ms mutual silence window on both
    // devices, master's base storm-stuck 500 ms
    // fired mid-stall and wiped 18 chunks the slave
    // had already ACKed. Re-arm the stuck clock so
    // the next tick gets a fresh window to observe
    // the pending RX. The threshold (3x okTickMs,
    // floored at 1 RTT) is small enough to catch
    // every real CPU stall, large enough not to
    // false-fire on a single missed tick under
    // backpressure. Pinned by
    // CpuStallReArmsBaseStuckTest.
    uint32_t priorOkTick = lastOkTickMs_;
    uint32_t okTick = (uint32_t)okTickMs();
    uint32_t stallFloor = okTick * 3u;
    if (stallFloor < (uint32_t)baudAwareRtoMs_unlocked())
        stallFloor = (uint32_t)baudAwareRtoMs_unlocked();
    // Drain-RX check: if the app's RX buffer holds
    // bytes (the peer sent payloads the app hasn't
    // drained), the peer is alive — don't fire the
    // stuck verdict on a backlog the app owns, not
    // the link. The field log showed master's app
    // buffer still had queued echo data while the
    // base-stuck detector fired; the wedge shape
    // was the storm-stuck verdict claiming a
    // dead peer on evidence the receiver had
    // already accepted. Re-arm on the same gate.
    // Pinned by DrainRxPreventsStuckHonestDropTest.
    bool appBacklog = hw.appBufAvailable() > 0;
    // Stamp the OK-tick wall clock after capturing
    // the prior entry's stamp. The priorOkTick shadow
    // above is what the gap-delta below compares.
    lastOkTickMs_ = now;
    // Storm-immune progress clock: reset only when the base
    // itself changes (real progress), never by a resend attempt.
    // See the field comment in Link.h for why decideSlot()'s own
    // age clock can't be trusted here under a NAK burst.
    // Re-arm on either of two conditions BEFORE the
    // base-change branch: (a) the CPU stall detector
    // flagged this tick as "link layer was starved"
    // (treat the gap as observation noise, not peer
    // silence), (b) the app's RX buffer holds the
    // peer's data (the peer IS sending — the GBN is
    // just out of sync with the app). Both conditions
    // are re-arms, not "honest progress": the base
    // may not have advanced, but the absence we're
    // measuring isn't the peer's fault.
    if (priorOkTick != 0 && (uint32_t)(now - priorOkTick) > stallFloor)
        gbnBaseStuckSinceMs_ = now;
    if (appBacklog)
        gbnBaseStuckSinceMs_ = now;
    if (arq_.gbnBase() != gbnBaseStuckTrackedSeq_) {
        gbnBaseStuckTrackedSeq_ = arq_.gbnBase();
        gbnBaseStuckSinceMs_ = now;
    }
    bool baseStormStuck =
        (uint32_t)(now - gbnBaseStuckSinceMs_) >= gbnBaseStuckThresholdMs_;
    // Baud-aware RTO: same drain formula as the stuck
    // threshold. A per-chunk RTO tied to the locked
    // baud means a 32-chunk 250-byte window at 9600
    // (~8.3 s drain) gives every chunk a real chance
    // to be ACKed before honest-drop, instead of the
    // prior fixed syncAckTimeoutMs which would have
    // tripped at ~1/8 of the drain time. Pinned by
    // BaudAwareStuckThresholdTest.
    uint32_t ackRtoMs = (uint32_t)baudAwareRtoMs_unlocked();
    if (ackRtoMs < (uint32_t)cfg.syncAckTimeoutMs)
        ackRtoMs = (uint32_t)cfg.syncAckTimeoutMs;
    LinkArq::Action a =
        arq_.decideSlot(arq_.gbnBase(), now, ackRtoMs, cfg.maxRetx);
    if (a == LinkArq::Action::Hold && !baseStormStuck)
        return false;
    if (a == LinkArq::Action::Drop || baseStormStuck) {
        if (baseStormStuck && a != LinkArq::Action::Drop)
            Log::log().warning(
                TAG,
                "seq=%u base storm-stuck %lu ms with no progress "
                "(decideSlot Hold — resend storm masking RTO) -> "
                "forcing honest-drop evaluation",
                (unsigned)arq_.gbnBase(),
                (unsigned long)(now - gbnBaseStuckSinceMs_));
        // maxRetx on the base is an honest drop only if the peer
        // is actually silent. A stuck base with reverse traffic
        // still arriving is our own resend storm starving the
        // peer's ACK path — congestion, not peer-gone. Pinned by
        // GbnDropPolicyTest.
        if (decideGbnDropOnMaxRetx(now, lastRxMs,
                                   (uint32_t)cfg.syncAckTimeoutMs) ==
            GbnDropDecision::Keep) {
            // A dead peer whose floating RX line keeps stamping
            // lastRxMs (line noise, half-duplex leakage) would
            // ride Keep forever and never drop. Cap the streak.
            if (consecutiveKeep_ >= gbnKeepRescueCap_unlocked()) {
                Log::log().warning(TAG,
                                   "seq=%u maxRetx (GBN base) rescue cap "
                                   "(%d consecutive Keeps) exhausted -> "
                                   "honest link drop",
                                   (unsigned)arq_.gbnBase(), consecutiveKeep_);
                consecutiveKeep_ = 0;
                reset_unlocked(true, false, ResetReason::GbnKeepRescue);
                return state == State::SWP;
            }
            consecutiveKeep_++;
            // Without the rearm the next tick re-hits
            // decideSlot -> Drop -> Keep immediately (the RTO is
            // still expired, the retx budget still exhausted):
            // a keep-livelock. Pinned by GbnKeepRearmTest.
            arq_.rearmSlot(arq_.gbnBase(), now);
            gbnAttempts_ = 0;
            gbnBackoffMs_ = 0;
            gbnLastRetxBase_ = 0xFF;
            Log::log().info(
                TAG,
                "seq=%u maxRetx (GBN base) but reverse channel alive "
                "(lastRx %lu ms ago) -> keep #%d, rearm base + "
                "reset backoff",
                (unsigned)arq_.gbnBase(), (unsigned long)(now - lastRxMs),
                consecutiveKeep_);
            return false;
        }
        consecutiveKeep_ = 0;
        Log::log().warning(TAG, "seq=%u maxRetx (GBN base) -> honest link drop",
                           (unsigned)arq_.gbnBase());
        // preserve=true: an honest drop is a recovery
        // of a link whose BAUD was proven fine — the
        // peer's BREAK-triggered reset preserves and
        // camps P3 at that baud, so a preserve=false
        // walk from P1-slowest here recreates the
        // master-walks/slave-camps sweep deadlock.
        // The disc-storm and locksWithoutRecv_ guards in
        // reset_unlocked bound wrong-baud camping the
        // same as for every other preserving reset.
        reset_unlocked(true, true, ResetReason::GbnMaxRetx);
        return state == State::SWP;
    }
    // Base advanced since the last round -> the stall is over;
    // restart the backoff from the base RTO.
    if (gbnLastRetxBase_ != 0xFF && gbnLastRetxBase_ != arq_.gbnBase()) {
        gbnAttempts_ = 0;
        gbnBackoffMs_ = 0;
        consecutiveKeep_ = 0;
    }
    gbnAttempts_++;
    gbnLastRetxBase_ = arq_.gbnBase();
    gbnBackoffMs_ =
        decideGbnBackoff(gbnAttempts_, (uint32_t)cfg.syncAckTimeoutMs,
                         gbnBackoffCapMs_unlocked());
    gbnResendWindow_unlocked(now);
    return false;
}

bool Link::onTimerOk_unlocked() {
    if (linkPaused_) {
        hw.startTimer(okTickMs());
        return false;
    }
    uint32_t now = hw.nowMs();
    if (breakSuspectMs_ != 0) {
        if ((uint32_t)(now - breakSuspectMs_) >=
            ::autolink::breakConfirmMs_unlocked(*this)) {
            Log::log().info(TAG, "BREAK -> resweep");
            breakSuspectMs_ = 0;
            breakSuspectSeen_ = 0;
            // Confirm branch: a genuine peer drop. App data
            // queued before the BREAK is now stale (the peer's
            // gone, the seq stamps it carries mean nothing to
            // the next session), so this is the right place to
            // clear it. Was previously cleared on the FIRST
            // (unconfirmed) BREAK, which silently discarded
            // queued app data on a healthy link. Pinned by
            // BreakConfirmClearsAppBufTest.
            hw.clearAppBuf();
            reset_unlocked(true, /*preservePreferredBaud=*/true,
                           ResetReason::HealthWatchdog);
            return true;
        }
        hw.startTimer(okTickMs());
        return false;
    }
    if (applyHealth_unlocked(now) != HealthAction::Keep)
        return true;
    if (sweepRetx_unlocked(now))
        return true;
    // An idle-but-healthy link must not be torn down by the
    // silent-peer watchdog. PING (not an ACK: an ACK carries a
    // cobsSeq and would walk the peer's gbnBase) — the peer's OK
    // receive path answers with PONG, so one frame refreshes
    // lastRxMs at both ends. Pinned by OkKeepaliveTest.
    if (cfg.idleTimeoutMs > 0 && arq_.pendingCount() == 0 &&
        (now - lastTxMs) > (uint32_t)cfg.idleTimeoutMs / 2) {
        sendSweepFrame_unlocked(PING_CMD);
        lastTxMs = now;
    }
    uint32_t next = okTickMs();
    if (gbnBackoffMs_ > next)
        next = gbnBackoffMs_;
    hw.startTimer(next);
    return false;
}

int Link::pendingAcks() const {
    hw.lock();
    int n = arq_.pendingCount();
    hw.unlock();
    return n;
}
bool Link::isAcked(uint8_t cobsSeq) const { return arq_.isAcked(cobsSeq); }
} // namespace autolink
