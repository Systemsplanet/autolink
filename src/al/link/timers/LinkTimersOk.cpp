
#include "al/link/Link.h"
#include "al/link/arq/ArqCache.h"
#include "al/link/timers/LinkHealth.h"
#include "al/link/arq/LinkArq.h"
#include "al/link/sweep/LinkDecision.h"
#include "al/link/sweep/LinkSweep.h"
#include "al/util/log/Log.h"

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
        // Sticky for the rest of this session: vetoes the P3
        // preferredBaud_ camp on every future preserving reset,
        // not just this one. Pinned by
        // PostSoakFieldFixesTest (AL-07 pin).
        breakStormSeen_ = true;
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
    // Consume a deferred second-BREAK fast-confirm from onBreak().
    // Same deadlock class as the storm branch above (reset_unlocked
    // -> sweep_.enterPhase1/enterPhase3 -> hw.setSpd(), synchronous)
    // and the same fix: run it here, on the timer-daemon task,
    // instead of inline on the UART event task that detected the
    // second BREAK. No clearAppBuf / sendBreak here — matches what
    // the inline path did before the deferral. Pinned by
    // BreakOnBreakDefersToOnTimerTest.
    if (breakConfirmPending_) {
        breakConfirmPending_ = false;
        // Literal "Link" tag, not this file's TAG ("AutoLink"): this
        // log line moved here from LinkTimerBreak.cpp (TAG="Link")
        // as part of the onBreak() deferral fix, and a field log
        // reading this line right after "Link BREAK suspect, confirm
        // in..." should see the same subsystem tag, not a silent
        // switch to "AutoLink".
        Log::log().info("Link", "BREAK -> resweep");
        reset_unlocked(true, /*preservePreferredBaud=*/true,
                       ResetReason::HealthWatchdog);
        // AL97-6: raw UART driver RX flush moved here from the
        // event hook (EspHalUartEvent.h) — it now fires only on a
        // CONFIRMED peer drop, not on every delivered BREAK
        // (including the unconfirmed first one and any coalesced
        // duplicate a healthy link later clears). Mirrors the
        // timeout-confirm branch below. Pinned by
        // EspHalBreakFlushOnConfirmOnlyTest.
        hw.flushRxHw();
        hw.unlock();
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

// F1: drain the TX ring with a bounded RTO.
// Drops the link lock, sleeps, re-acquires.
// The only legal call site is the SYNC side
// (syncRtoStep_unlocked + sendMsg's SYNC and ASYNC branches). The single
// drain implementation: `need` lets the SYNC multi-chunk path reserve the
// whole frame set in one wait instead of spinning a frame at a time.
// Pinned by SyncDrainTxRingWithLockDropTest + AsyncLoopCallsDrainTxRingTest
// + SyncMultiChunkFullDrainTest + AckNakDoesNotBlockOnRxTest.
bool Link::drainTxRing_unlocked(SendMsgReason *outReason, int need) {
    if (outReason)
        *outReason = SendMsgReason::None;
    if (need < kWorstCaseCobsFrame)
        need = kWorstCaseCobsFrame;
    if (hw.txAvail() >= need)
        return true;
    uint32_t waitDeadline =
        hw.nowMs() + (uint32_t)baudAwareRtoMs_unlocked() + 1u;
    while (hw.txAvail() < need) {
        // G4: bail early on a state
        // change or a pool-exhaust
        // event. The state change
        // marks a reset (the
        // caller's loop is dead);
        // pool-exhaust is the
        // caller's problem, not the
        // ring's. Both are reported
        // via the outReason so the
        // caller can stamp the
        // right diagnostic.
        if (state != State::OK) {
            if (outReason)
                *outReason = SendMsgReason::NotOk;
            return false;
        }
        if (!arqCache_.hasRoom()) {
            noteTxReject_unlocked();
            poolExhaustDrops_++;
            if (outReason)
                *outReason = SendMsgReason::PoolExhaust;
            Log::log().warning(TAG,
                               "drainTxRing: ARQ cache exhausted mid-drain "
                               "(free=%d, need=%d) — drop",
                               hw.txAvail(), need);
            return false;
        }
        hw.unlock();
        // delayMs, not delayUs:
        // ets_delay_us is a busy
        // spin that holds the link
        // lock off-CPU, starving
        // onRx / onTimer. A 1 ms
        // vTaskDelay yields the
        // loop task.
        hw.delayMs(1);
        hw.lock();
        if ((int32_t)(hw.nowMs() - waitDeadline) >= 0) {
            txRingStallDrops_++;
            // AL92-4: this branch previously
            // left txRejFirstMs_/txRejLastMs_
            // untouched — the pool-exhaust
            // branch above stamps them, this
            // one didn't, even though both are
            // the same underlying signal (TX
            // couldn't make progress) that
            // decideHealth() (LinkTimersOk.cpp)
            // reads to catch a sustained
            // backpressure stall. A mid-loop
            // SYNC abort (AL91-1) that lands
            // here repeatedly was invisible to
            // the health machine.
            noteTxReject_unlocked();
            if (outReason)
                *outReason = SendMsgReason::TxRingStall;
            Log::log().warning(TAG,
                               "drainTxRing: ring didn't free within "
                               "per-frame RTO (%d ms) — abort",
                               (int)baudAwareRtoMs_unlocked());
            return false;
        }
    }
    return true;
}

bool Link::retxSeq_unlocked(uint8_t seq) {
    const uint8_t *buf = nullptr;
    int len = 0;
    // F2: the wire-write path's bool return
    // propagates here. A wire-stall on a retx
    // means the chunk is still pending (no
    // txBytes bump, no lastTxMs bump, no seq
    // advance — the seq is the retx target,
    // not a new send). The next GBN resend
    // burst or RTO step will retry the same
    // seq. The applyRetx stamp (in the caller)
    // doesn't run on the false return, so
    // the next retx sees the same seq on the
    // original RTO budget. Pinned by
    // ResendCobsFramePropagatesStallTest.
    if (!arqCache_.slotInUse(seq)) {
        Log::log().info(TAG,
                        "ARQ retx cobsSeq=%u cache miss (chunk already "
                        "delivered); pending bit left to time out",
                        (unsigned)seq);
        return true;
    } else if (arqCache_.peekForRetx(seq, &buf, &len)) {
        // AL89-11: demoted to debug. At ASYNC
        // pipeline rate this line fires once per
        // resend — 400+ chunks/s on a saturated
        // link, every one of them burning a log
        // ring entry. The retx itself is
        // observable from the per-frame wire trace
        // (debug level), and the aggregate
        // retxCountFor(seq) on a stuck slot still
        // logs at warning (it carries the
        // diagnostic value the previous shape
        // meant this line to provide). AGENTS.md
        // rule 16: hot-path chatter at the wire
        // rate is not a state-change cause, and
        // not a wire-op result worth a
        // per-iteration log line. Pinned by
        // RetxLogLevelDemotedTest.
        Log::log().debug(TAG, "ARQ retx cobsSeq=%u (%d bytes) — verbatim",
                         (unsigned)seq, len);
        return resendCobsFrame_unlocked(seq, buf, len);
    } else {
        Log::log().info(TAG,
                        "ARQ retx cobsSeq=%u (no pool buf) — "
                        "verbatim 0 bytes",
                        (unsigned)seq);
        return resendCobsFrame_unlocked(seq, nullptr, 0);
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
    // DropPeerReset is slave-only (see LinkHealth.h).
    h.isMaster = isMaster;
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
    // AL97-3: same backoff clamp LinkTimersGbn.cpp applies to its
    // own honest-drop clock (effectiveStuckThresholdMs), so this
    // watchdog and the GBN retx ladder never race on the same
    // stalled-peer condition. Pinned by
    // FieldWedgeFixesTest (Pin 9).
    h.peerStalledMs = healthPeerStalledMs(
        lockedBaud, h.pending, gbnBackoffMs_ + h.rtoMs);
    // Slave's pending-independent OK-exit watchdog.
    // campBudgetMs is the same value the P3 preferred-
    // baud camp uses (`resweepPrefBudgetMs_unlocked`,
    // 3-5 s) so the slave's exit window and the
    // master's camp window are bounded by the same
    // source of truth. decideHealth fires
    // DropPeerReset at 2 * campBudgetMs (well under
    // deadPeerMs = 3 * idleTimeoutMs) when the
    // slave has an empty TX window and no peer frame
    // in 2x the master's camp. Pinned by
    // SlaveFastExitOnPeerResetTest.
    h.campBudgetMs = resweepPrefBudgetMs_unlocked();

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
    case HealthAction::DropPeerReset:
        // Slave's pending-independent exit from OK. The
        // peer is presumed gone (no frame in
        // 2 * campBudgetMs) but the slave is alive
        // enough to keep its own timers armed. Distinct
        // log tag from HealthWatchdog so the operator
        // can tell the two verdicts apart at a glance.
        // Pinned by SlaveFastExitOnPeerResetTest.
        //
        // thresholdMs prints the value the verdict actually
        // compared against (2 * campBudgetMs), not
        // idleTimeoutMs. The field logs all showed
        // rxAge=~8200 against a printed idleTimeoutMs=10000 —
        // every drop looked like a false-positive firing
        // before its own stated deadline, when the real
        // threshold (LinkHealth.h's `rxAge > 2u *
        // campBudgetMs`) was 8000. idleTimeoutMs is kept
        // alongside since it's still a real input to the
        // wider health machine, just not this branch's gate.
        // Pinned by PostSoakFieldFixesTest (AL-06 pin).
        Log::log().warning(
            TAG,
            "peer-reset watchdog -> drop "
            "(rxAge=%lu campBudgetMs=%lu thresholdMs=%lu pending=%d "
            "baud=%lu idleTimeoutMs=%d)",
            (unsigned long)(now - h.lastRxMs), (unsigned long)h.campBudgetMs,
            (unsigned long)(2u * h.campBudgetMs), h.pending,
            (unsigned long)lockedBaud, cfg.idleTimeoutMs);
        break;
    default:
        Log::log().warning(TAG, "ARQ pool exhausted (pending=%d) -> drop",
                           h.pending);
        break;
    }
    // The master must preserve the proven baud here too,
    // symmetric with the slave's BREAK-triggered reset.
    // Without the preservePreferredBaud=true argument
    // a master health drop would walk P1 from 9600 while
    // the slave camped P3 at 512000 — mutual deadlock
    // (master sat in P1 for 16-20 s, never locked until
    // manual re-kickoff). The existing recentDiscs_ <
    // DISC_STORM_THRESHOLD guard inside reset_unlocked
    // prevents camping on a genuinely-bad baud, and the
    // slave's bounded P3 camp gives the slave a
    // coordinated P1 fall. Pinned by
    // WireSimReConvergeTest.
    reset_unlocked(true, /*preservePreferredBaud=*/true,
                   ResetReason::HealthWatchdog);
    return a;
}

// sweepRetx_unlocked and gbnRetxBaseAndRearm_unlocked live in
// timers/gbn/LinkTimersGbn.cpp — split out to keep this file
// under the 15 KB cap.

bool Link::onTimerOk_unlocked() {
    if (linkPaused_) {
        hw.startTimer(okTickMs());
        return false;
    }
    uint32_t now = hw.nowMs();
    // AL97-7: always-on wire-activity aggregate — the field-side
    // replacement for the per-ACK / per-freed-chunk verbose traces
    // now gated behind AUTOLINK_TRACE_WIRE. Fires at most once per
    // WIRE_AGG_WINDOW_MS, and only when there was anything to
    // report, so an idle link stays silent. Pinned by
    // AckPathNotVerboseByDefaultTest.
    if (wireAckAggAcks_ > 0 && wireAckAggLastMs_ != 0 &&
        (uint32_t)(now - wireAckAggLastMs_) >= WIRE_AGG_WINDOW_MS) {
        Log::log().debug(TAG, "wire: %llu acks, %llu chunks freed in last %lu ms",
                         (unsigned long long)wireAckAggAcks_,
                         (unsigned long long)wireAckAggFreed_,
                         (unsigned long)(now - wireAckAggLastMs_));
        wireAckAggAcks_ = 0;
        wireAckAggFreed_ = 0;
        wireAckAggLastMs_ = now;
    } else if (wireAckAggLastMs_ == 0) {
        // First tick after a reset (or process start): nothing to
        // compare against yet, just arm the window.
        wireAckAggLastMs_ = now;
    }
    // Bounded baud upgrade: this session locked slower than a baud
    // the pair has already proven, and has now held that slow lock
    // long enough to be considered settled. Aim preferredBaud_ back
    // at the proven baud and take a preserving reset, which enters
    // the P3 camp there. If the peer does not meet us the camp
    // expires and the normal P1 walk re-locks slow again, costing
    // one reset per attempt and capped at
    // BAUD_UPGRADE_MAX_ATTEMPTS. Without this a p2-fallback lock at
    // the slowest baud persists until something else drops the
    // link. Pinned by BaudUpgradeTest.
    if (baudUpgradeAtMs_ != 0 && (int32_t)(now - baudUpgradeAtMs_) >= 0 &&
        bestProvenBaud_ != NO_PREFERRED_BAUD &&
        (uint8_t)spdI > bestProvenBaud_ &&
        baudUpgradeAttempts_ < BAUD_UPGRADE_MAX_ATTEMPTS) {
        baudUpgradeAtMs_ = 0;
        baudUpgradeAttempts_++;
        Log::log().info(TAG,
                        "baud upgrade %d/%d: retrying proven baud[%u] from "
                        "baud[%d]",
                        baudUpgradeAttempts_, BAUD_UPGRADE_MAX_ATTEMPTS,
                        (unsigned)bestProvenBaud_, spdI);
        preferredBaud_ = bestProvenBaud_;
        reset_unlocked(false, true, ResetReason::BaudUpgrade);
        return state == State::SWP;
    }
    if (breakSuspectMs_ != 0) {
        uint32_t confirmMs = ::autolink::breakConfirmMs_unlocked(*this);
        uint32_t elapsed = (uint32_t)(now - breakSuspectMs_);
        if (elapsed >= confirmMs) {
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
            // AL97-6: raw UART driver RX flush moved here from the
            // event hook (EspHalUartEvent.h) — fires only on this
            // CONFIRMED peer drop, not on every delivered BREAK.
            // Mirrors the fast-confirm branch in onTimer() above.
            // Pinned by EspHalBreakFlushOnConfirmOnlyTest.
            hw.flushRxHw();
            // AL97-6: raw UART driver RX flush moved here from the
            // event hook — same reasoning as the fast-confirm
            // branch above. Pinned by
            // EspHalBreakFlushOnConfirmOnlyTest.
            hw.flushRxHw();
            reset_unlocked(true, /*preservePreferredBaud=*/true,
                           ResetReason::HealthWatchdog);
            return true;
        }
        // Not yet confirmed. This tick only runs at all because
        // onBreak() armed an early wake-up for a keepalive that
        // was due before the confirm deadline (see onBreak) — a
        // normal, no-op-idle onBreak arm sleeps straight through
        // to the expiry branch above. Send the keepalive now if
        // it's (still) due, then re-arm for whichever comes
        // first: the next keepalive-due point or the true confirm
        // deadline — NOT okTickMs(), which is commonly seconds
        // long and would blow straight past the deadline. Pinned
        // by BreakSuspectKeepaliveTest.
        uint32_t remain = confirmMs - elapsed;
        uint32_t nextPoll = remain;
        if (cfg.idleTimeoutMs > 0 && arq_.pendingCount() == 0) {
            uint32_t half = (uint32_t)cfg.idleTimeoutMs / 2;
            uint32_t sinceTx = (uint32_t)(now - lastTxMs);
            if (sinceTx >= half) {
                sendSweepFrame_unlocked(PING_CMD);
                lastTxMs = now;
                sinceTx = 0;
            }
            uint32_t dueIn = half - sinceTx;
            if (dueIn < nextPoll)
                nextPoll = dueIn;
        }
        if (nextPoll < 1)
            nextPoll = 1;
        hw.startTimer((int)nextPoll);
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
