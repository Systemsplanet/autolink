// GBN sweep-retx half split out of LinkTimersOk.cpp to keep the
// per-file 15 KB cap — the same reason LinkTimerBreak.cpp was
// split out earlier. onTimerOk_unlocked and the rest of the
// OK-tick machinery stay in LinkTimersOk.cpp; this file owns only
// the GBN retransmit/stuck-base decision path it drives.
#include "al/link/Link.h"
#include "al/link/arq/LinkArq.h"
#include "al/link/sweep/LinkDecision.h"
#include "al/util/Log.h"

namespace autolink {

namespace {
constexpr const char *TAG = "AutoLink";
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
    // gbnBackoffMs_ lengthens the next OK-tick (see
    // onTimerOk_unlocked) but nothing lengthens
    // gbnBaseStuckThresholdMs_ itself, so a backed-off retx round
    // always lands after the stuck window has already expired.
    // Clamp the *effective* threshold used below (never the field
    // — other readers still see the real baud-aware value) to
    // cover the backoff plus one more RTO. Pinned by
    // GbnBackoffTest.
    uint32_t effectiveStuckThresholdMs = gbnBaseStuckThresholdMs_;
    if (gbnBackoffMs_ + ackRtoMs > effectiveStuckThresholdMs)
        effectiveStuckThresholdMs = gbnBackoffMs_ + ackRtoMs;
    bool baseStormStuck =
        (uint32_t)(now - gbnBaseStuckSinceMs_) >= effectiveStuckThresholdMs;
    LinkArq::Action a =
        arq_.decideSlot(arq_.gbnBase(), now, ackRtoMs, cfg.maxRetx);
    // A real RTO expiry is progress-seeking and must never be
    // swallowed by the storm-stuck verdict just because both land
    // on the same tick — at 512000 baud ackRtoMs and
    // gbnBaseStuckThresholdMs_ both collapse to syncAckTimeoutMs,
    // so the first RTO and the stuck verdict coincide. Restart the
    // stuck clock behind the retransmit instead of skipping it.
    // Pinned by GbnStuckForcesRetxTest.
    if (a == LinkArq::Action::Retx) {
        gbnRetxBaseAndRearm_unlocked(now);
        return false;
    }
    if (a == LinkArq::Action::Hold && !baseStormStuck)
        return false;
    if (a == LinkArq::Action::Drop ||
        (baseStormStuck && arq_.retxCountFor(arq_.gbnBase()) >= 2)) {
        if (baseStormStuck && a != LinkArq::Action::Drop)
            Log::log().warning(
                TAG,
                "seq=%u base storm-stuck %lu ms with %u real retx and "
                "no progress -> forcing honest-drop evaluation",
                (unsigned)arq_.gbnBase(),
                (unsigned long)(now - gbnBaseStuckSinceMs_),
                (unsigned)arq_.retxCountFor(arq_.gbnBase()));
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
    // Remaining case: baseStormStuck with a real retx count still
    // under 2 — the per-slot RTO clock (decideSlot) hasn't caught
    // up yet, but the storm-immune clock has. Force one base
    // retransmit rather than wait; the >= 2 gate above still
    // requires real failed attempts before an honest drop.
    gbnRetxBaseAndRearm_unlocked(now);
    return false;
}

void Link::gbnRetxBaseAndRearm_unlocked(uint32_t now) {
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
    // A retransmit is progress-seeking; the stuck clock restarts
    // behind it regardless of what triggered this call. Pinned by
    // GbnStuckForcesRetxTest.
    gbnBaseStuckSinceMs_ = now;
}

} // namespace autolink
