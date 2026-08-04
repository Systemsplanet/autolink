
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

void Link::onTimerSwp_unlocked() {
    if (isMaster) {
        if (sweep_.phase() == SweepPhase::PHASE1) {
            sendSweepFrame_unlocked(PING_CMD);
            pingSample++;
            hw.startTimer(phase1ArmMs());
            return;
        }
        if (sweep_.phase() == SweepPhase::PHASE2) {
            int dwell = sweep_.dwells().phase2[spdI];
            int subTick = sweep_.dwells().phase2Slave[spdI];
            if (subTick <= 0 || subTick > dwell)
                subTick = dwell;
            sweep_.addPhase2ElapsedMs(subTick);
            if (sweep_.phase2ElapsedMs() < dwell) {
                // Still inside the current baud's ~1650 ms dwell
                // (250 x baudCount x 1.1) — resend PING_CMD on the
                // ~250 ms sub-tick cadence (aligned to
                // phase2Slave) rather than wait out the whole
                // dwell on a single PING. CRC-fail lines at
                // baud-transition boundaries showed one corrupted
                // PING was forfeiting the entire dwell. The baud
                // hasn't changed, so no setSpd here — that's a
                // real (~20 ms blocking) hardware op, reserved for
                // the baud-advance branch below. Pinned by
                // MasterPhase2PingCadenceTest.
                sendSweepFrame_unlocked(PING_CMD);
                hw.startTimer(subTick);
                return;
            }
            sweep_.setPhase2ElapsedMs(0);
            spdI++;
            SwpPhaseAction a =
                decideMasterPhase2Timeout(spdI, cfg.clampToMaxBauds());
            if (a == SwpPhaseAction::FallbackLockSlowest) {
                int lb = cfg.clampToMaxBauds() - 1;
                // p2-fallback: declaring OK against a peer
                // that never answered is a 10 s retx-storm
                // wait with no recovery path (master
                // sends LOCK_CMD + lockOk, peer is wedged
                // listening at a different baud, neither
                // side converges). Require at least one
                // PONG at the fallback baud across the
                // sweep round. If we got none, fall back
                // to a P1 walk with the existing backoff
                // — the master will try again next round
                // from a fresh slowest-baud position.
                // Pinned by FallbackRequiresPongTest.
                if (sweepPongCount_ < 1) {
                    Log::log().warning(TAG,
                                       "p2-fallback denied: no PONG received "
                                       "this round (count=%d) — falling back "
                                       "to P1 walk",
                                       sweepPongCount_);
                    int slowest = cfg.clampToMaxBauds() - 1;
                    spdI = slowest;
                    hw.setSpd(cfg.allowedBaudSafe(slowest));
                    sweep_.enterPhase1(*this);
                    return;
                }
                sweep_.reset();
                hw.setSpd(cfg.allowedBaudSafe(lb));
                spdI = lb;
                sendSweepFrame_unlocked(LOCK_CMD + (uint8_t)lb);
                lockOk_unlocked(lb, "p2-fallback");
                return;
            }
            hw.setSpd(cfg.allowedBaudSafe(spdI));
            sendSweepFrame_unlocked(PING_CMD);
            int nextDwell = sweep_.dwells().phase2[spdI];
            int nextSubTick = sweep_.dwells().phase2Slave[spdI];
            if (nextSubTick <= 0 || nextSubTick > nextDwell)
                nextSubTick = nextDwell;
            hw.startTimer(nextSubTick);
            return;
        }
        if (sweep_.phase() == SweepPhase::PHASE3) {
            // The proven baud didn't re-lock: walk P1 from the
            // slowest rather than let the sweep advance to the
            // next baud, which would drift off the proven
            // sequence on every drop.
            //
            // setSpd holds the link lock the whole window
            // (EspHal::setSpd blocks up to ~20 ms in
            // uart_wait_tx_done). Releasing the lock
            // across setSpd would let concurrent writers
            // mutate state, but onTimer() runs on the FreeRTOS
            // timer-service (daemon) task — reacquiring with
            // portMAX_DELAY inside a timer callback deadlocks
            // when the holder itself needs to post a
            // startTimer/stopTimer command, which has to be
            // serviced by the same daemon that's now stuck in
            // xSemaphoreTake. The fix is to keep the lock
            // across setSpd: the 20 ms stall is bounded, the
            // holder is the daemon itself (no one to wait on
            // for itself), and the prior three guards
            // (state / arq generation / sweep phase) reduce to
            // defense-in-depth — no other writer can race the
            // link lock from inside the timer callback.
            //
            // Every exit from this branch re-arms a watchdog
            // (phase1ArmMs) so a partial fix to the prior
            // shape can't reintroduce a silent dead branch
            // where onTimer() never fires again. Pinned by
            // GbnKeepRearmTest Pin 6a/6b and P3BailWatchdogTest.
            if (resweepPrefPending_) {
                // fix: on the FIRST P3 miss at the
                // preserved baud, listen briefly at the
                // preserved baud again before falling through
                // to a full P1 walk. The peer may be on a
                // sweep-phase skew (slave took the preserved-
                // baud fast path in the same window, but the
                // master's P3 timeout fired before the
                // peer's PONG arrived). One more PING at the
                // same baud lets the two sides converge
                // without dropping to 9600. Pinned by
                // WireSimReConvergeTest.
                // Camp for a wall-clock budget rather than a fixed
                // number of t3-sized attempts, so a peer that needs
                // seconds to come back is still met at the proven
                // baud instead of being abandoned to a P1 walk.
                bool campOpen =
                    resweepPrefDeadlineMs_ != 0 &&
                    (int32_t)(resweepPrefDeadlineMs_ - hw.nowMs()) > 0 &&
                    resweepPrefAttempts_ < RESWEEP_PREF_ATTEMPT_CEILING;
                if (campOpen) {
                    resweepPrefAttempts_++;
                    Log::log().info(
                        TAG,
                        "P3 preferredBaud_ relock missed (attempt "
                        "%d, %lu ms of camp left) — re-sending PING at "
                        "preserved baud",
                        resweepPrefAttempts_,
                        (unsigned long)(resweepPrefDeadlineMs_ - hw.nowMs()));
                    // Re-send the PING frame so the peer has a
                    // fresh chance to lock. A pure timer re-arm
                    // is a no-op if the peer missed the original
                    // PING outright (e.g. wire noise) — we'd just
                    // wait the same dwell and time out again. The
                    // first PING may also have been masked by a
                    // RESET the peer was in the middle of, in
                    // which case a literal re-PING lets both
                    // sides converge without a full P1 walk. The
                    // short P3 timer on the second attempt
                    // (PHASE3_ACKS_NEEDED - 1) gives the retry
                    // a tighter budget so a stuck peer is
                    // recognized before the 8 s P1 fallback.
                    sendSweepFrame_unlocked(PING_CMD);
                    int rt =
                        roundTripMs(cfg.allowedBaudSafe(sweep_.phase3Baud()));
                    if (rt < 50)
                        rt = 50;
                    int acks = sweep_.phase3Acks();
                    int t3 = rt * (PHASE3_ACKS_NEEDED - acks + 1) + 100;
                    if (t3 < 200)
                        t3 = 200;
                    hw.startTimer(t3);
                    return;
                }
                resweepPrefAttempts_ = 0;
                Log::log().info(
                    TAG,
                    "P3 preferredBaud_ camp exhausted -> falling back to enterPhase1");
                resweepPrefPending_ = false;
                resweepPrefDeadlineMs_ = 0;
                sweep_.reset();
                int slowest = cfg.clampToMaxBauds() - 1;
                uint32_t baud = cfg.allowedBaudSafe(slowest);
                hw.setSpd(baud);
                spdI = slowest;
                sweep_.setPhase(SweepPhase::PHASE1);
                Log::log().info(TAG, "=== P1 slowest baud[%d]=%lu ===", slowest,
                                (unsigned long)baud);
                sendSweepFrame_unlocked(PING_CMD);
                hw.startTimer(sweep_.phase1ArmMs(*this));
                return;
            }
            int next = sweep_.phase3Baud() + 1;
            SwpPhaseAction a =
                decideMasterPhase3Timeout(next, cfg.clampToMaxBauds());
            if (a == SwpPhaseAction::FallbackLockSlowest) {
                int lb = cfg.clampToMaxBauds() - 1;
                // Same rule as p2-fallback: refuse OK
                // against a peer that never answered. P3
                // is a more constrained path (the proven
                // baud already passed P2 once), so a
                // PONG-less P3 timeout is a stronger
                // signal of peer wedge.
                if (sweepPongCount_ < 1) {
                    Log::log().warning(TAG,
                                       "p3-fallback denied: no PONG received "
                                       "this round (count=%d) — falling back "
                                       "to P1 walk",
                                       sweepPongCount_);
                    int slowest = cfg.clampToMaxBauds() - 1;
                    spdI = slowest;
                    hw.setSpd(cfg.allowedBaudSafe(slowest));
                    sweep_.enterPhase1(*this);
                    return;
                }
                sweep_.reset();
                spdI = lb;
                hw.setSpd(cfg.allowedBaudSafe(lb));
                lockOk_unlocked(lb, "p3-fallback");
                return;
            }
            sweep_.reset();
            spdI = next;
            hw.setSpd(cfg.allowedBaudSafe(spdI));
            sendSweepFrame_unlocked(PING_CMD);
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
        hw.startTimer(phase1ArmMs());
        return;
    }
    if (sweep_.phase() == SweepPhase::PHASE2) {
        // The slave enters P2 at spdI=0 (the proven
        // baud). Decrementing from 0 to -1 fires
        // DropToPhase1 on every P2 timeout. In a
        // single-baud config the slave P1 <-> P2 cycles
        // every 250 ms and never reaches P3. Stay in P2
        // at the proven baud and let the master PING
        // (every idleTimeoutMs/2) promote the slave to P3.
        // Multi-baud config: spdI==0 is the proven
        // baud only when allowedBaudsCount()==1; in a
        // multi-baud config the slave must wrap
        // spdI to clampToMaxBauds()-1 and keep
        // walking, otherwise the slave camps at 512000
        // while the master walks all the way to 9600.
        // Pinned by SlaveP2SpdI0WalkTest.
        if (spdI <= 0) {
            if (cfg.allowedBaudsCount <= 1) {
                hw.startTimer(sweep_.dwells().phase2Slave[0]);
                return;
            }
            // Wrap to slowest (N-1), listen at N-1 first
            // (RX baud matches the master's P1/fallback
            // seat), then decrement for the next iteration.
            // Listening at N-2 here would miss the baud
            // the master is parked on.
            spdI = cfg.clampToMaxBauds() - 1;
            hw.setSpd(cfg.allowedBaudSafe(spdI));
            hw.startTimer(sweep_.dwells().phase2Slave[spdI]);
            return;
        }
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
    // Slave PHASE3 camp. Re-arming the preserved-baud
    // camp forever (when the master has fallen to P1
    // from a HealthWatchdog reset) leaves the slave
    // listening at the proven baud while the master
    // walks P1; neither side ever overlaps at the
    // preserved baud inside any bounded window. After
    // RESWEEP_PREF_MAX_ATTEMPTS re-arms with no lock,
    // clear preferredBaud_ and re-enter P1 the master
    // can actually find. The re-arm dwell is
    // idleTimeoutMs/2 (5 s default) so two PING
    // arrivals (PHASE3_ACKS_NEEDED=2) at the master's
    // keepalive cadence are reachable across the
    // budget. Pinned by WireSimReConvergeTest.
    if (resweepPrefAttempts_ < RESWEEP_PREF_MAX_ATTEMPTS) {
        resweepPrefAttempts_++;
        Log::log().info(TAG,
                        "P3 slave camp re-arming at preserved baud "
                        "(attempt %d/%d)",
                        resweepPrefAttempts_, RESWEEP_PREF_MAX_ATTEMPTS);
        int rearmMs = cfg.idleTimeoutMs / 2;
        if (rearmMs < 1000)
            rearmMs = 1000;
        hw.startTimer(rearmMs);
        return;
    }
    resweepPrefAttempts_ = 0;
    Log::log().info(TAG,
                    "P3 slave camp exhausted -> enterPhase1 "
                    "(cleared preferredBaud_=%u)",
                    (unsigned)preferredBaud_);
    preferredBaud_ = NO_PREFERRED_BAUD;
    resweepPrefPending_ = false;
    sweep_.enterPhase1(*this);
}

} // namespace autolink
