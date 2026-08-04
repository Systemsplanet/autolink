
#include "al/link/Link.h"
#include "al/link/arq/ArqCache.h"
#include "al/link/sweep/LinkDecision.h"
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

// OK-state tick. ASYNC bounds it to one RTO — the tick drives the
// GBN retx sweep (RTO check on the oldest unacked), and there's no
// reorder hold left to race since out-of-order frames are dropped,
// not buffered.
int Link::okTickMs() const {
    int k = cfg.idleTimeoutMs / 3;
    if (k < 50)
        k = 50;
    if (k < cfg.syncAckTimeoutMs)
        k = cfg.syncAckTimeoutMs;

    if (cfg.mode == AutoLinkConfig::Mode::ASYNC) {
        if (k > cfg.syncAckTimeoutMs)
            k = cfg.syncAckTimeoutMs;
        if (k < 50)
            k = 50;
    }
    return k;
}

int Link::phase1ArmMs() { return sweep_.phase1ArmMs(*this); }

int Link::bestSpd_unlocked() const {
    int best = baudSweep.pickBest();
    if (best < 0)
        return 0;
    for (int j = 0; j < best; j++)
        if (baudSweep.scoreAt(j) > 0)
            return j;
    return best;
}

void Link::lockOk_unlocked(int idx, const char *tag) {
    // A lock can only legitimately complete from SWP: every real
    // sweep runs in SWP state, and a peer-driven resweep goes
    // through the epoch-mismatch reset (-> SWP) first. Reaching
    // here while ALREADY OK means an OK-state CTRL frame (the
    // keepalive PING shares its wire command with the sweep P3
    // PING) walked the sweep handler far enough to "re-lock" a
    // link that never went down — and the lock-time actions below
    // are destructive when the session is live: clearAppBuf()
    // discards delivered-but-undrained messages, flushRxHw() eats
    // in-flight wire bytes, and re-arming settleUntilMs_ silently
    // drops another 50 ms of frames mid-stream. Suppress: while
    // OK, a lock completion is always spurious. Pinned by
    // SpuriousRelockSuppressTest.
    if (state == State::OK) {
        Log::log().warning(TAG,
                           "lockOk (%s, baud[%d]) while already OK — "
                           "spurious re-lock suppressed",
                           tag ? tag : "?", idx);
        return;
    }
    hw.setSpd(cfg.allowedBaudSafe(idx));
    spdI = idx;
    errs = 0;
    preferredBaud_ = (uint8_t)idx;
    // Lower index = faster. Remember the fastest baud ever locked
    // so a slow fallback lock cannot erase it, and arm a bounded
    // upgrade when this lock is slower than what the pair has
    // already proven. Without this a p2-fallback lock at the
    // slowest baud is permanent: preferredBaud_ now names the slow
    // baud, so every later preserving reset camps there too.
    if (bestProvenBaud_ == NO_PREFERRED_BAUD || (uint8_t)idx < bestProvenBaud_)
        bestProvenBaud_ = (uint8_t)idx;
    if (bestProvenBaud_ != NO_PREFERRED_BAUD && (uint8_t)idx > bestProvenBaud_ &&
        baudUpgradeAttempts_ < BAUD_UPGRADE_MAX_ATTEMPTS) {
        baudUpgradeAtMs_ = hw.nowMs() + BAUD_UPGRADE_DELAY_MS;
        Log::log().info(TAG,
                        "locked baud[%d] below proven baud[%u] — upgrade "
                        "attempt %d/%d armed in %lu ms",
                        idx, (unsigned)bestProvenBaud_,
                        baudUpgradeAttempts_ + 1, BAUD_UPGRADE_MAX_ATTEMPTS,
                        (unsigned long)BAUD_UPGRADE_DELAY_MS);
    } else {
        baudUpgradeAtMs_ = 0;
        if ((uint8_t)idx <= bestProvenBaud_)
            baudUpgradeAttempts_ = 0;
    }
    baudRetries_ = 0;
    errWindowStartMs_ = hw.nowMs();
    errWindowCount_ = 0;
    lastRxMs = lastTxMs = hw.nowMs();
    // lastValidRxMs must be stamped here too. A fresh
    // OK session that inherits the prior session's
    // CRC-valid timestamp is exactly the shape the
    // health watchdog reads as a stale-but-recent link,
    // and rxAge-based verdicts (DropAsymIdle, DropIdle)
    // would under-count the new session's age by the
    // time spent in the SWP walk. Same reason
    // lastRxMs is reset — the link is just OK'd, the
    // wire's own clock is the only honest baseline.
    // Pinned by LastValidRxMsTest.
    lastValidRxMs = hw.nowMs();
    lockedAtMs_ = hw.nowMs();
    postLockQuietLogged_ = false;
    // Drain the app buf and the UART FIFO at lock
    // time. The baud switch (setSpd above) leaves
    // line garbage in the rx FIFO and any in-flight
    // frames from the prior baud in the app buf; both
    // must be cleared BEFORE the new session's first
    // onPayload. Pinned by SettleGateTest.
    // clearAppBuf is unconditional on every reset, so
    // the reset->relock path is covered; this call
    // covers lockOk_unlocked's other callers.
    hw.clearAppBuf();
    hw.flushRxHw();
    // Open the post-lock settle gate for
    // AUTOLINK_APP_SETTLE_MS. Frames arriving in this
    // window are dropped silently (no ACK, no NAK, no
    // app-buf write, no rxSeq advance) by onPayload /
    // onAck / onNak; the post-lock quiet window's
    // tx-side gate is a separate concept and is
    // controlled by txQuiet_unlocked + postLockQuietMs.
    settleUntilMs_ = hw.nowMs() + AUTOLINK_WIRE_SETTLE_MS;
    gbnAttempts_ = 0;
    gbnBackoffMs_ = 0;
    gbnLastRetxBase_ = 0xFF;
    consecutiveKeep_ = 0;
    // Baud-aware stuck threshold: cache the SWP-state
    // floor (syncAckTimeoutMs) here. Per-tick recompute
    // happens in sweepRetx_unlocked, where the actual
    // pendingCount is available — caching the baud-aware
    // value at lockOk would only see pending=0 and pin
    // the threshold to the floor. Production never sets
    // the override flag, so the per-tick recompute is
    // the live path. Pinned by BaudAwareStuckThresholdTest.
    gbnBaseStuckThresholdMs_ = (uint32_t)cfg.syncAckTimeoutMs;
    gbnBaseStuckThresholdOverridden_ = false;
    Log::log().debug(TAG,
                     "baud-aware stuck threshold floor: %lu ms "
                     "(pending=%d, baud=%lu, drain=%lu)",
                     (unsigned long)gbnBaseStuckThresholdMs_,
                     arq_.pendingCount(),
                     (unsigned long)cfg.allowedBaudSafe(idx),
                     (unsigned long)((uint32_t)arq_.pendingCount() *
                                     (uint32_t)(MAX_CHUNK + MSG_HDR) * 10u /
                                     cfg.allowedBaudSafe(idx)));
    // Heap baseline: the lockOk event is the only
    // natural "this session is healthy" checkpoint.
    // Logged on every lockOk (cheap: one line per
    // lock). Pinned by HeapLogAtLockOkTest.
#ifdef ARDUINO
    Log::log().info(TAG, "lockOk: freeHeap=%u",
                    (unsigned)esp_get_free_heap_size());
#endif
    Log::log().info(TAG, "Locked %lu baud (%s)",
                    (unsigned long)cfg.allowedBaudSafe(idx), tag);
    changeState_unlocked(State::OK);
    wasEverOk_ = true;
    resweepPrefPending_ = false;
    resweepPrefAttempts_ = 0;
    // The OK-state timer is the ONLY driver
    // of GBN RTO-driven retx in OK state
    // (the per-chunk RTO is not armed in OK).
    // Gating it on cfg.idleTimeoutMs > 0
    // leaves the link with no retx path
    // when idleTimeoutMs is 0 (a legitimate
    // config, and exactly what the plain
    // loopback uses), and any silently-lost
    // frame (settle gate, line noise without
    // a NAK) wedges the GBN window full
    // forever. Arm unconditionally; the
    // keepalive branch inside onTimerOk is
    // the only path that still gates on
    // idleTimeoutMs. Pinned by
    // OkTimerAlwaysArmsTest.
    hw.startTimer(okTickMs());
}

bool Link::handleSwp_unlocked(uint8_t cobsSeq, uint8_t payload) {
    // First sweep frame after a fresh peer arrival: latch the
    // peer's epoch so a later PING with a different epoch
    // (mid-handshake restart underneath) is detectable.
    if (!peerSweepEpochKnown_) {
        peerSweepEpoch_ = cobsSeq;
        peerSweepEpochKnown_ = true;
        Log::log().info(TAG,
                        "SWP: latched peer epoch=%u from first sweep frame",
                        (unsigned)peerSweepEpoch_);
    } else if (cobsSeq != peerSweepEpoch_) {
        Log::log().info(TAG, "SWP: peer epoch slid %u -> %u (mid-handshake)",
                        (unsigned)peerSweepEpoch_, (unsigned)cobsSeq);
        peerSweepEpoch_ = cobsSeq;
    }
    int lockIdx = -1;
    if (isLockPayload(payload, cfg.clampToMaxBauds(), &lockIdx)) {
        Log::log().info(TAG, "SWP: LOCK payload baudIdx=%u baud=%lu (phase=%d)",
                        (unsigned)lockIdx,
                        (unsigned long)cfg.allowedBaudSafe(lockIdx),
                        (int)sweep_.phase());
        hw.setSpd(cfg.allowedBaudSafe(lockIdx));
        spdI = lockIdx;
        sweep_.reset();
        lockOk_unlocked(lockIdx,
                        sweep_.phase() == SweepPhase::PHASE3
                            ? (isMaster ? "p3-lock" : "p3-pong")
                            : "lock");
        return false;
    }
    if (!isMaster && payload == REQ_CMD) {
        int best = bestSpd_unlocked();
        Log::log().info(
            TAG, "SWP: REQ -> best baudIdx=%u baud=%lu (slave settles)",
            (unsigned)best, (unsigned long)cfg.allowedBaudSafe(best));
        sendSweepFrame_unlocked((uint8_t)best);
        lockOk_unlocked(best, "REQ");
        return false;
    }
    if (isMaster && payload == PONG_CMD) {
        // Per-sweep-round PONG tally. Consumed by the
        // master p2/p3-fallback paths to refuse OK
        // against a peer that never answered. Reset on
        // P1 entry and reset_unlocked (start of a new
        // round/session); accumulated across P1->P2->P3
        // within a round so the baud-mismatch fallback
        // path can converge on a PONG received earlier
        // in the same round.
        sweepPongCount_++;
        SwpPhaseAction a = SwpPhaseAction::Stay;
        switch (sweep_.phase()) {
        case SweepPhase::PHASE1:
            a = decideMasterPhase1Ack();
            break;
        case SweepPhase::PHASE2:
            a = decideMasterPhase2Ack();
            break;
        case SweepPhase::PHASE3: {
            int newAcks = sweep_.phase3Acks() + 1;
            sweep_.incPhase3Acks();
            a = decideMasterPhase3Ack(newAcks, PHASE3_ACKS_NEEDED);
            break;
        }
        default:
            Log::log().info(TAG, "SWP: PONG ignored (phase=NONE)");
            return false;
        }
        Log::log().info(TAG,
                        "SWP: master got PONG at baudIdx=%d (phase=%d) "
                        "-> action=%d",
                        spdI, (int)sweep_.phase(), (int)a);
        return applyMasterSwpAction_unlocked(a);
    }
    if (!isMaster && payload == PING_CMD) {
        baudSweep.score(spdI);
        SwpPhaseAction a = SwpPhaseAction::Stay;
        switch (sweep_.phase()) {
        case SweepPhase::PHASE1:
            a = decidePongPhase1Ping();
            break;
        case SweepPhase::PHASE2:
            a = decidePongPhase2Ping();
            break;
        case SweepPhase::PHASE3: {
            int newAcks = sweep_.phase3Acks() + 1;
            sweep_.incPhase3Acks();
            a = decidePongPhase3Ping(newAcks, PHASE3_ACKS_NEEDED);
            break;
        }
        default:
            Log::log().info(TAG, "SWP: PING ignored (phase=NONE)");
            return false;
        }
        Log::log().info(TAG,
                        "SWP: slave got PING at baudIdx=%d (phase=%d) "
                        "-> action=%d",
                        spdI, (int)sweep_.phase(), (int)a);
        return applyPongSwpAction_unlocked(a);
    }
    return false;
}

bool Link::applyMasterSwpAction_unlocked(SwpPhaseAction a) {
    switch (a) {
    case SwpPhaseAction::PromoteToPhase2:
        sweep_.enterPhase2(*this);
        return false;
    case SwpPhaseAction::PromoteToPhase3:
        sweep_.enterPhase3(*this, spdI);
        return false;
    case SwpPhaseAction::Lock: {
        int baud = sweep_.phase3Baud();
        Log::log().info(TAG, "SWP: master LOCKING at baudIdx=%d baud=%lu", baud,
                        (unsigned long)cfg.allowedBaudSafe(baud));
        sweep_.reset();
        hw.setSpd(cfg.allowedBaudSafe(baud));
        spdI = baud;
        sendSweepFrame_unlocked(LOCK_CMD + (uint8_t)baud);
        lockOk_unlocked(baud, "phase3");
        return false;
    }
    case SwpPhaseAction::Stay: {
        int baud = sweep_.phase3Baud();
        sendSweepFrame_unlocked(PING_CMD);
        int rt = roundTripMs(cfg.allowedBaudSafe(baud));
        if (rt < 50)
            rt = 50;
        int acks = sweep_.phase3Acks();
        int t3 = rt * (PHASE3_ACKS_NEEDED - acks + 1) + 100;
        if (t3 < 200)
            t3 = 200;
        hw.startTimer(t3);
        // Stay is a per-RTO event but fires at most a few
        // times per session — not a flood risk. Debug, not
        // info, because the per-tick debug line in
        // onTimerSwp_unlocked already covers the dwell
        // state, and the entry-phase log in LinkSweep.cpp
        // covers the phase change itself.
        Log::log().debug(TAG,
                         "SWP: master P3 Stay (baudIdx=%d acks=%d/%d t3=%d)",
                         baud, acks, PHASE3_ACKS_NEEDED, t3);
        return false;
    }
    case SwpPhaseAction::FallbackLockSlowest:
    case SwpPhaseAction::SendPongAck:
    case SwpPhaseAction::DropToPhase1:

        return false;
    }
    return false;
}

bool Link::applyPongSwpAction_unlocked(SwpPhaseAction a) {
    switch (a) {
    case SwpPhaseAction::SendPongAck:
        sendPongAck_unlocked();
        return false;
    case SwpPhaseAction::PromoteToPhase3:
        sweep_.enterPhase3(*this, spdI);
        sendPongAck_unlocked();
        return false;
    case SwpPhaseAction::Lock: {
        int lb = sweep_.phase3Baud();
        Log::log().info(TAG, "SWP: slave LOCKING at baudIdx=%d baud=%lu", lb,
                        (unsigned long)cfg.allowedBaudSafe(lb));
        sweep_.reset();
        hw.setSpd(cfg.allowedBaudSafe(lb));
        spdI = lb;
        sendSweepFrame_unlocked(LOCK_CMD + (uint8_t)lb);
        lockOk_unlocked(lb, "p3-pong");
        return false;
    }
    case SwpPhaseAction::Stay:
        sendPongAck_unlocked();
        return false;
    case SwpPhaseAction::PromoteToPhase2:
        sendPongAck_unlocked();
        sweep_.enterPhase2(*this);
        hw.startTimer(sweep_.dwells().phase2Slave[0]);
        return false;
    case SwpPhaseAction::FallbackLockSlowest:
    case SwpPhaseAction::DropToPhase1:

        return false;
    }
    return false;
}

} // namespace autolink
