// OK-state timer: health watchdogs (LinkHealth.h) +
// ARQ retransmit sweep. SWP timer: baud-sweep phase
// machine. LCK timer: handshake retry.
//
// onBreak: mid-sweep a UART_BREAK is self-induced
// (baud mismatch after setSpd), so in non-OK states
// it's a no-op except clearing the appBuf — a hard
// reset there livelocks the sweep (P1→P2→BREAK→P1).
#include "al/link/Link.h"
#include "al/link/arq/ArqCache.h"
#include "al/link/LinkHealth.h"
#include "al/link/arq/LinkArq.h"
#include "al/link/sweep/LinkDecision.h"
#include "al/link/sweep/LinkSweep.h"
#include "al/util/Log.h"

static constexpr const char *TAG = "AutoLink";

namespace autolink {
static_assert(
    MAX_CHUNK + MSG_HDR <= ArqCache::POOL_BUF_MAX,
    "MAX_CHUNK + MSG_HDR > ArqCache::POOL_BUF_MAX: bump POOL_BUF_MAX or shrink MAX_CHUNK");

void Link::onBreak() {
    hw.lock();
    if (state != State::OK) {
        hw.clearAppBuf();
        hw.unlock();
        return;
    }
    Log::log().info(TAG, "BREAK -> resweep");
    reset_unlocked(true);
    hw.unlock();
}

// Timer callbacks report "send BREAK" back here so
// the wire op happens after the single unlock — the
// prior shape unlocked inside the drop paths and then
// again here (double unlock: UB on the host mutex).
void Link::onTimer() {
    hw.lock();
    bool brk = false;
    State s = state;
    if (s == State::OK)
        brk = onTimerOk_unlocked();
    else if (s == State::SWP)
        onTimerSwp_unlocked();
    else if (s == State::LCK && isMaster)
        brk = onTimerLck_unlocked();
    hw.unlock();
    if (brk)
        hw.sendBreak();
}

// Resend one cached frame verbatim; caller holds the
// lock. Cache miss is a no-op (pending bit times out).
// Inline, not one-per-tick: a NAK burst or multi-frame
// RTO expiry must all resend in the same pass to stay
// ahead of the receiver's reorder expiry.
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

// Returns true when the caller must send a BREAK
// after unlocking. The FreeRTOS timer is one-shot:
// every exit that keeps the link in OK must re-arm,
// or the ARQ sweep and watchdogs die silently — a
// pause/resume or idleTimeoutMs<=0 previously did
// exactly that.
bool Link::onTimerOk_unlocked() {
    if (linkPaused_) {
        hw.startTimer(okTickMs());
        return false;
    }
    uint32_t now = hw.nowMs();
    // Drop reorder gaps past their hold and advance
    // rxSeq so contiguous delivery can resume; see
    // LinkRx::onPayload.
    uint8_t droppedSeqs[16];
    int dropped = reorder_.dropExpired(now, cfg.reorderHoldMs, droppedSeqs, 16);
    if (dropped > 0) {
        lostMsgs += (uint64_t)dropped;
        for (int i = 0; i < dropped; i++) {
            reorderAdvanceRxSeq(droppedSeqs[i]);
        }
    }
    // All keep/drop watchdog decisions live in
    // decideHealth (LinkHealth.h) — one pure function,
    // truth-table pinned, no per-mode branch to miss.
    HealthState h;
    h.rejFirstMs = txRejFirstMs_;
    h.rejLastMs = txRejLastMs_;
    h.lastRxMs = lastRxMs;
    h.lastTxMs = lastTxMs;
    h.pending = arq_.pendingCount();
    h.frameErrs = frameErrs;
    h.poolFull = !arqCache_.hasRoom();
    h.sync = (cfg.mode == AutoLinkConfig::Mode::SYNC);
    // idleTimeoutMs <= 0 disables the time-based
    // watchdogs only; the ARQ sweep below still runs.
    HealthAction a = cfg.idleTimeoutMs > 0
                         ? decideHealth(h, now, cfg.idleTimeoutMs)
                         : HealthAction::Keep;
    if (a != HealthAction::Keep) {
        switch (a) {
        case HealthAction::DropTxStall:
            Log::log().warning(TAG,
                               "tx backpressure stall -> drop "
                               "(stalledMs=%lu arqPending=%d)",
                               (unsigned long)(now - h.rejFirstMs), h.pending);
            break;
        case HealthAction::DropAsymIdle:
            Log::log().warning(TAG, "asymmetric idle -> drop");
            break;
        case HealthAction::DropIdle:
            Log::log().warning(TAG,
                               "idle watchdog -> drop "
                               "(rxAge=%lu txAge=%lu idleTimeoutMs=%d "
                               "arqPending=%d frameErrs=%lu)",
                               (unsigned long)(now - h.lastRxMs),
                               (unsigned long)(now - h.lastTxMs),
                               cfg.idleTimeoutMs, h.pending,
                               (unsigned long)h.frameErrs);
            break;
        default:
            Log::log().warning(TAG, "ARQ pool exhausted (pending=%d) -> drop",
                               h.pending);
            break;
        }
        reset_unlocked(true);
        return true;
    }
    // ASYNC only: SYNC blocks inline on each ACK and
    // never populates the pool, so leftover pending
    // bits from a timed-out SYNC wait would retransmit
    // garbage. Backstop for lost NAKs: resend every
    // RTO-expired frame in one pass; maxRetx forces a
    // resweep.
    if (cfg.mode != AutoLinkConfig::Mode::SYNC) {
        for (int s = 0; s < 256; s++) {
            if (!arq_.isPending(s))
                continue;
            LinkArq::Action a = arq_.decideSlot(
                (uint8_t)s, now, (uint32_t)cfg.syncAckTimeoutMs, cfg.maxRetx);
            if (a == LinkArq::Action::Hold)
                continue;
            if (a == LinkArq::Action::Drop) {
                Log::log().error(TAG, "seq=%u maxRetx -> drop", (unsigned)s);
                reset_unlocked(true);
                return true;
            }
            arq_.applyRetx((uint8_t)s, now);
            retxSeq_unlocked((uint8_t)s);
        }
    }
    hw.startTimer(okTickMs());
    return false;
}

int Link::pendingAcks() const {
    hw.lock();
    int n = arq_.pendingCount();
    hw.unlock();
    return n;
}
bool Link::isAcked(uint8_t cobsSeq) const { return arq_.isAcked(cobsSeq); }

void Link::onTimerSwp_unlocked() {
    if (isMaster) {
        if (sweep_.phase() == SweepPhase::PHASE1) {
            // P1 timeout is a wire-side "send PING and
            // re-arm" — no state decision involved. The
            // decision function decideMasterPhase1Timeout
            // exists for symmetry and returns Stay; we
            // call it so the truth-table test pins the
            // action.
            (void)decideMasterPhase1Timeout(0, 0);
            sendFrame_unlocked(PING_CMD);
            pingSample++;
            hw.startTimer(phase1ArmMs());
            return;
        }
        if (sweep_.phase() == SweepPhase::PHASE2) {
            spdI++;
            SwpPhaseAction a =
                decideMasterPhase2Timeout(spdI, cfg.clampToMaxBauds());
            if (a == SwpPhaseAction::FallbackLockSlowest) {
                int lb = cfg.clampToMaxBauds() - 1;
                sweep_.reset();
                hw.setSpd(cfg.allowedBaudSafe(lb));
                spdI = lb;
                sendFrame_unlocked(LOCK_CMD + (uint8_t)lb);
                lockOk_unlocked(lb, "p2-fallback");
                return;
            }
            hw.setSpd(cfg.allowedBaudSafe(spdI));
            sendFrame_unlocked(PING_CMD);
            hw.startTimer(sweep_.dwells().phase2[spdI]);
            return;
        }
        if (sweep_.phase() == SweepPhase::PHASE3) {
            int next = sweep_.phase3Baud() + 1;
            SwpPhaseAction a =
                decideMasterPhase3Timeout(next, cfg.clampToMaxBauds());
            if (a == SwpPhaseAction::FallbackLockSlowest) {
                int lb = cfg.clampToMaxBauds() - 1;
                sweep_.reset();
                spdI = lb;
                hw.setSpd(cfg.allowedBaudSafe(lb));
                lockOk_unlocked(lb, "p3-fallback");
                return;
            }
            sweep_.reset();
            spdI = next;
            hw.setSpd(cfg.allowedBaudSafe(spdI));
            sendFrame_unlocked(PING_CMD);
            hw.startTimer(sweep_.dwells().phase2[spdI]);
            return;
        }
        sweep_.enterPhase1(*this);
        return;
    }
    if (emptySweeps == 0 || emptySweeps % 5 == 0) {
        Log::log().info(TAG,
                        "pong SWP baud[%d]=%lu "
                        "phase=%d",
                        spdI, (unsigned long)cfg.allowedBaudSafe(spdI),
                        (int)sweep_.phase());
    }
    emptySweeps++;
    if (emptySweeps == 11) {
        if (!wasEverOk_) {
            // First crossing only — log once and stay
            // silent. The counter keeps climbing so we
            // don't re-fire every ~1.5 s on a dead wire.
            Log::log().error(TAG,
                             "WIRING? no PING after %d ticks:"
                             " need TX->RX crossover, shared GND",
                             emptySweeps);
        }
    }
    if (sweep_.phase() == SweepPhase::PHASE1) {
        (void)decidePongPhase1Timeout();
        hw.startTimer(phase1ArmMs());
        return;
    }
    if (sweep_.phase() == SweepPhase::PHASE2) {
        int dwell = sweep_.dwells().phase2Slave[spdI];
        spdI--;
        SwpPhaseAction a = decidePongPhase2Timeout(spdI, 0);
        if (a == SwpPhaseAction::DropToPhase1) {
            sweep_.enterPhase1(*this);
            return;
        }
        hw.setSpd(cfg.allowedBaudSafe(spdI));
        hw.startTimer(dwell);
        return;
    }
    hw.startTimer(sweep_.dwells().phase2[spdI]);
}

bool Link::onTimerLck_unlocked() {
    int max = (int)cfg.clampToMaxBauds() * 2;
    lckRetries++;
    if (decideLckTick(lckRetries, max) == LckAction::SendReq) {
        sendFrame_unlocked(REQ_CMD);
        hw.startTimer(cfg.delayMs);
        return false;
    }
    reset_unlocked(true);
    return true;
}

} // namespace autolink
