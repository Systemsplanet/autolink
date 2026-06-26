// LinkTimers -- onBreak, onTimer (the dispatcher), onTimerOk_unlocked,
// pendingAcks / isAcked, onTimerSwp_unlocked, onTimerLck_unlocked.
//
// onTimer dispatches to onTimerOk_unlocked (OK steady-state),
// onTimerSwp_unlocked (SWP handshake), or onTimerLck_unlocked
// (LCK retry). The OK-state timer is the heart of the
// runtime: it watches idle timeouts, fires heartbeats,
// emits keepalive frames, and runs the ARQ retransmit
// loop. The SWP-state timer advances the baud-sweep
// phase machine. The LCK-state timer retries the
// handshake when the master doesn't get a baud reply.
//
// onBreak is the UART_BREAK dispatcher. Pre the
// latest BREAK-debounce fix it
// hard-reset on every break event, which caused a SWP
// livelock (the post-setSpd UART_BREAK fires immediately
// after a baud switch and tore down the SWP state on
// the very tick the link entered P2). The fix mirrors
// err_unlocked's state guard: in non-OK states the break
// is a no-op except for clearing the appBuf.
#include "al/link/Link.h"
#include "al/link/arq/ArqCache.h"
#include "al/link/arq/LinkArq.h"
#include "al/link/sweep/LinkDecision.h"
#include "al/link/sweep/LinkSweep.h"
#include "al/util/Log.h"

static constexpr const char *TAG = "AutoLink";

namespace autolink {
static_assert(
    MAX_CHUNK + MSG_HDR <= ArqCache::POOL_BUF_MAX,
    "MAX_CHUNK + MSG_HDR > ArqCache::POOL_BUF_MAX: bump POOL_BUF_MAX or shrink MAX_CHUNK");

// Heartbeat constants live in this TU because every
// heartbeat call site (onTimerOk_unlocked) is here. They
// stay file-static so the linker enforces single-
// definition; the other Link TUs don't reference them.
static constexpr int HEARTBEAT_MS = 100;
static constexpr int HEARTBEAT_MISS_LIMIT = 3;
// Asymmetric idle: TX active, RX silent
// → peer gone, drop fast.
static constexpr int FAST_IDLE_RX_MS = 300;
static constexpr int FAST_IDLE_TX_MS = 1000;

void Link::onBreak() {
    hw.lock();
    // Mid-sweep the break is self-induced: a
    // baud mismatch on a quiet line emits
    // UART_BREAK right after a setSpd, and
    // the UART event task hands it back here.
    // Hard-resetting on every one tears the
    // SWP state down on the very tick it
    // enters P2, so the link cycles
    // P1->P2->BREAK->P1 forever. err_unlocked
    // already short-circuits in non-OK states;
    // mirror that guard here. A real break
    // (link lock lost, peer reset) only matters
    // once the link is up.
    if (state != State::OK) {
        hw.clearAppBuf();
        hw.unlock();
        return;
    }
    Log::log().info(TAG, "BREAK -> resweep");
    reset_unlocked(true);
    hw.unlock();
}

void Link::onTimer() {
    hw.lock();
    State s = state;
    int cur = spdI;
    if (s == State::OK)
        onTimerOk_unlocked();
    else if (s == State::SWP)
        onTimerSwp_unlocked();
    else if (s == State::LCK && isMaster)
        onTimerLck_unlocked();
    else {
        hw.unlock();
        (void)cur;
        return;
    }
    hw.unlock();
    if (hasPendingRetx_) {
        // Cache holds the payload; link drives
        // the resend. A cache miss is a no-op
        // (the ARQ bit will time out on its own).
        uint8_t base = pendingRetxBase_;
        hasPendingRetx_ = false;
        const uint8_t *buf = nullptr;
        int len = 0;
        if (!arqCache_.slotInUse(base)) {
            Log::log().info(TAG,
                            "ARQ retx cobsSeq=%u cache miss (chunk already "
                            "delivered); pending bit left to time out",
                            (unsigned)base);
        } else if (arqCache_.peekForRetx(base, &buf, &len)) {
            Log::log().warning(TAG, "ARQ retx cobsSeq=%u (%d bytes) — verbatim",
                               (unsigned)base, len);
            resendCobsFrame_unlocked(base, buf, len);
        } else {
            Log::log().info(TAG,
                            "ARQ retx cobsSeq=%u (keepalive, no pool buf) — "
                            "verbatim 0 bytes",
                            (unsigned)base);
            resendCobsFrame_unlocked(base, nullptr, 0);
        }
    }
}

void Link::onTimerOk_unlocked() {
    if (cfg.idleTimeoutMs <= 0)
        return;
    uint32_t now = hw.nowMs();
    if (linkPaused_)
        return;
    int dropped = reorder_.dropExpired(now, cfg.reorderHoldMs);
    if (dropped > 0)
        lostMsgs += (uint64_t)dropped;
    {
        uint32_t rxAge = now - lastRxMs;
        uint32_t txAge = now - lastTxMs;
        // SYNC mode: skip the asymmetric
        // idle check. The sender blocks
        // inline for each frame's ACK, so
        // "TX active, RX silent" is the
        // expected steady state between
        // messages. The sender's own
        // syncAckTimeoutMs watchdog catches
        // actual hangs.
        if (cfg.mode != AutoLinkConfig::Mode::SYNC) {
            // TX active, RX silent → peer gone.
            if (rxAge > (uint32_t)FAST_IDLE_RX_MS &&
                txAge < (uint32_t)FAST_IDLE_TX_MS) {
                Log::log().warning(TAG, "asymmetric idle -> drop");
                reset_unlocked(true);
                hw.unlock();
                hw.sendBreak();
                return;
            }
        }
        if (decideIdleWatchdog(rxAge, txAge, cfg.idleTimeoutMs) ==
            IdleAction::Drop) {
            reset_unlocked(true);
            hw.unlock();
            hw.sendBreak();
            return;
        }
    }
    if ((now - lastHeartbeatMs_) >= (uint32_t)HEARTBEAT_MS) {
        lastHeartbeatMs_ = now;
        sendFrame_unlocked(PING_CMD);
        heartbeatPingsMissed_++;
        if (heartbeatPingsMissed_ >= HEARTBEAT_MISS_LIMIT) {
            Log::log().warning(TAG, "HB: %d missed -> drop",
                               heartbeatPingsMissed_);
            reset_unlocked(true);
            hw.unlock();
            hw.sendBreak();
            return;
        }
    }
    if (decideKeepalive(now - lastTxMs, cfg.idleTimeoutMs, false) ==
        KeepaliveAction::Emit) {
        sendCobsFrame_unlocked(nullptr, 0);
        lastTxMs = now;
    }
    // ASYNC only: the ARQ retransmit
    // machinery lives in the link task's
    // timer tick. In SYNC mode the sender
    // blocks inline for the receiver's ACK
    // and the ARQ pool is never populated,
    // so any ackedPending_[] bits left over
    // from a timed-out SYNC wait are stale.
    // Running the retransmit loop here would
    // call arqCache_.peekForRetx() for slots whose
    // pool buffer was never inserted, i.e.
    // spurious retransmits.
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
                hw.unlock();
                hw.sendBreak();
                return;
            }
            arq_.applyRetx((uint8_t)s, now);
            pendingRetxBase_ = (uint8_t)s;
            hasPendingRetx_ = true;
            break;
        }
    }
    hw.startTimer(okTickMs());
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
                decideMasterPhase2Timeout(spdI, cfg.allowedBaudsCount);
            if (a == SwpPhaseAction::FallbackLockSlowest) {
                int lb = cfg.allowedBaudsCount - 1;
                sweep_.reset();
                hw.setSpd(cfg.allowedBauds[lb]);
                spdI = lb;
                sendFrame_unlocked(LOCK_CMD + (uint8_t)lb);
                lockOk_unlocked(lb, "p2-fallback");
                return;
            }
            hw.setSpd(cfg.allowedBauds[spdI]);
            sendFrame_unlocked(PING_CMD);
            hw.startTimer(sweep_.dwells().phase2[spdI]);
            return;
        }
        if (sweep_.phase() == SweepPhase::PHASE3) {
            int next = sweep_.phase3Baud() + 1;
            SwpPhaseAction a =
                decideMasterPhase3Timeout(next, cfg.allowedBaudsCount);
            if (a == SwpPhaseAction::FallbackLockSlowest) {
                int lb = cfg.allowedBaudsCount - 1;
                sweep_.reset();
                spdI = lb;
                hw.setSpd(cfg.allowedBauds[lb]);
                lockOk_unlocked(lb, "p3-fallback");
                return;
            }
            sweep_.reset();
            spdI = next;
            hw.setSpd(cfg.allowedBauds[spdI]);
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
                        spdI, (unsigned long)cfg.allowedBauds[spdI],
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
        hw.setSpd(cfg.allowedBauds[spdI]);
        hw.startTimer(dwell);
        return;
    }
    hw.startTimer(sweep_.dwells().phase2[spdI]);
}

void Link::onTimerLck_unlocked() {
    int max = (int)cfg.allowedBaudsCount * 2;
    lckRetries++;
    if (decideLckTick(lckRetries, max) == LckAction::SendReq) {
        sendFrame_unlocked(REQ_CMD);
        hw.startTimer(cfg.delayMs);
    } else {
        reset_unlocked(true);
        hw.unlock();
        hw.sendBreak();
    }
}

} // namespace autolink
