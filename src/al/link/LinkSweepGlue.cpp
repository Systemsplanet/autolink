
#include "al/link/Link.h"
#include "al/link/arq/ArqCache.h"
#include "al/link/sweep/LinkDecision.h"
#include "al/link/sweep/LinkSweep.h"
#include "al/util/Log.h"

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
    hw.setSpd(cfg.allowedBaudSafe(idx));
    spdI = idx;
    errs = 0;
    preferredBaud_ = (uint8_t)idx;
    baudRetries_ = 0;
    errWindowStartMs_ = hw.nowMs();
    errWindowCount_ = 0;
    lastRxMs = lastTxMs = hw.nowMs();
    lockedAtMs_ = hw.nowMs();
    gbnAttempts_ = 0;
    gbnBackoffMs_ = 0;
    gbnLastRetxBase_ = 0xFF;
    Log::log().info(TAG, "Locked %lu baud (%s)",
                    (unsigned long)cfg.allowedBaudSafe(idx), tag);
    changeState_unlocked(State::OK);
    wasEverOk_ = true;
    resweepPrefPending_ = false;
    if (cfg.idleTimeoutMs > 0)
        hw.startTimer(okTickMs());
}

bool Link::handleSwp_unlocked(uint8_t cobsSeq, uint8_t payload) {
    (void)cobsSeq;
    int lockIdx = -1;
    if (isLockPayload(payload, cfg.clampToMaxBauds(), &lockIdx)) {
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
        sendFrame_unlocked((uint8_t)best);
        lockOk_unlocked(best, "REQ");
        return false;
    }
    if (isMaster && payload == PONG_CMD) {
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
            return false;
        }
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
            return false;
        }
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
        sweep_.reset();
        hw.setSpd(cfg.allowedBaudSafe(baud));
        spdI = baud;
        sendFrame_unlocked(LOCK_CMD + (uint8_t)baud);
        lockOk_unlocked(baud, "phase3");
        return false;
    }
    case SwpPhaseAction::Stay: {
        int baud = sweep_.phase3Baud();
        sendFrame_unlocked(PING_CMD);
        int rt = roundTripMs(cfg.allowedBaudSafe(baud));
        if (rt < 50)
            rt = 50;
        int acks = sweep_.phase3Acks();
        int t3 = rt * (PHASE3_ACKS_NEEDED - acks + 1) + 100;
        if (t3 < 200)
            t3 = 200;
        hw.startTimer(t3);
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
        sweep_.reset();
        hw.setSpd(cfg.allowedBaudSafe(lb));
        spdI = lb;
        sendFrame_unlocked(LOCK_CMD + (uint8_t)lb);
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
