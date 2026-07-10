
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
    // preservePreferredBaud=true: the BREAK peer-detach path
    // (vs. a watchdog drop) treats `wasEverOk_`+`preferredBaud_`
    // as a previously-proven baud worth attempting a fast P3
    // re-lock at. Watchdog drops keep the default (clear) so a
    // failed link isn't optimistically re-locked at a baud
    // that may have drifted.
    reset_unlocked(true, /*preservePreferredBaud=*/true);
    hw.unlock();
}

void Link::onTimer() {
    hw.lock();
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

// Go-Back-N resend: on RTO of the base or a NAK matching it,
// resend every currently-outstanding frame from gbnBase_
// through the newest sent seq, verbatim, in one shot — a
// receiver that discards out-of-order frames needs the WHOLE
// window replayed, not just its oldest member. One shared
// attempt counter covers the round (not per-frame).
void Link::gbnResendWindow_unlocked(uint32_t now) {
    if (!arq_.gbnActive())
        return;
    uint8_t s = arq_.gbnBase();
    int n = arq_.pendingCount();
    for (int i = 0; i < n; i++) {
        retxSeq_unlocked(s);
        arq_.applyRetx(s, now);
        s = (s == COBS_SEQ_MAX) ? 0 : (uint8_t)(s + 1);
    }
}

// Returns the drop action taken (Keep = link stays up).
HealthAction Link::applyHealth_unlocked(uint32_t now) {
    HealthState h;
    h.rejFirstMs = txRejFirstMs_;
    h.rejLastMs = txRejLastMs_;
    h.lastRxMs = lastRxMs;
    h.lastTxMs = lastTxMs;
    h.rtoMs = (uint32_t)cfg.syncAckTimeoutMs;
    h.pending = arq_.pendingCount();
    h.frameErrs = frameErrs;
    h.poolFull = !arqCache_.hasRoom();
    h.sync = (cfg.mode == AutoLinkConfig::Mode::SYNC);

    HealthAction a = cfg.idleTimeoutMs > 0
        ? decideHealth(h, now, cfg.idleTimeoutMs)
        : HealthAction::Keep;
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
        Log::log().warning(TAG, "asymmetric idle -> drop");
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
    default:
        Log::log().warning(TAG, "ARQ pool exhausted (pending=%d) -> drop",
                           h.pending);
        break;
    }
    reset_unlocked(true);
    return a;
}

// GBN's one retransmit driver: RTO on the oldest unacked
// (gbnBase_). A NAK matching gbnBase_ fires this early via
// onNak; this is the timer backstop for a lost NAK. maxRetx
// exceeded on the base is an honest link drop — under GBN the
// cache IS the window, so there's no cache-miss/fake-ACK
// retirement path left to take. Returns true if the caller
// must sendBreak (link was dropped).
bool Link::sweepRetx_unlocked(uint32_t now) {
    if (cfg.mode == AutoLinkConfig::Mode::SYNC || !arq_.gbnActive())
        return false;
    LinkArq::Action a = arq_.decideSlot(
        arq_.gbnBase(), now, (uint32_t)cfg.syncAckTimeoutMs, cfg.maxRetx);
    if (a == LinkArq::Action::Hold)
        return false;
    if (a == LinkArq::Action::Drop) {
        Log::log().warning(TAG, "seq=%u maxRetx (GBN base) -> honest link drop",
                           (unsigned)arq_.gbnBase());
        reset_unlocked(true);
        return state == State::SWP;
    }
    gbnAttempts_++;
    gbnResendWindow_unlocked(now);
    return false;
}

bool Link::onTimerOk_unlocked() {
    if (linkPaused_) {
        hw.startTimer(okTickMs());
        return false;
    }
    uint32_t now = hw.nowMs();
    if (applyHealth_unlocked(now) != HealthAction::Keep)
        return true;
    if (sweepRetx_unlocked(now))
        return true;
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
            // BREAK-triggered preferredBaud_ re-lock missed:
            // the proven baud didn't take. Fall back to a full
            // P1 walk rather than the sweep's normal
            // "advance to next baud" behavior, which would
            // pull the link off the proven baud sequence on
            // every drop.
            if (resweepPrefPending_) {
                Log::log().info(TAG,
                                "P3 preferredBaud_ relock missed -> "
                                "falling back to enterPhase1");
                resweepPrefPending_ = false;
                sweep_.reset();
                sweep_.enterPhase1(*this);
                return;
            }
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

} // namespace autolink
