
#include "al/link/sweep/LinkSweep.h"
#include "al/link/ISweepCtx.h"
#include "al/link/LinkWire.h"
#include "al/link/arq/ArqCache.h"
#include "al/link/sweep/LinkDecision.h"
#include "al/util/Log.h"

static constexpr const char *TAG = "AutoLink";

namespace autolink {
void LinkSweep::enterPhase1(ISweepCtx &ctx) {
    phase_ = SweepPhase::PHASE1;
    // Per-round PONG tally reset. The master p2/p3
    // fallback paths consume this counter to refuse OK
    // against a peer that never answered (a PONG-less
    // fallback is a 10 s retx-storm wait with no
    // recovery path). Pinned by FallbackRequiresPongTest.
    ctx.resetSweepPongCount();
    int slowest = ctx.allowedBaudsCount() - 1;
    ctx.setCurrentSpdI(slowest);
    ctx.hwSetSpd(ctx.allowedBaud(slowest));
    Log::log().info(TAG, "=== P1 slowest baud[%d]=%lu dwellMs=%d ===", slowest,
                    (unsigned long)ctx.allowedBaud(slowest),
                    ctx.masterRole() ? dwells_.phase1
                                     : dwells_.phase2[0] + 200);
    if (ctx.masterRole()) {
        ctx.sendSweepFrame(PING_CMD);
        ctx.hwStartTimer(dwells_.phase1);
    } else {
        ctx.hwStartTimer(dwells_.phase2[0] + 200);
    }
}

void LinkSweep::enterPhase2(ISweepCtx &ctx) {
    phase_ = SweepPhase::PHASE2;
    // Note: sweepPongCount_ is NOT reset on P2/P3
    // entry. The master p2-fallback path checks the
    // counter to refuse OK against a peer that never
    // answered; the P1 PONG is evidence the peer is
    // alive at some baud and the P2-fallback at the
    // slowest baud needs that evidence to converge in
    // the baud-mismatch case. Resetting here would
    // deny the P2 fallback in exactly the shape
    // alink_io's readme-usage path exercises.
    // The counter is reset in P1 entry (start of a
    // new sweep round) and in reset_unlocked (start
    // of a new session) — both are the right
    // boundaries for "fresh round".
    ctx.setCurrentSpdI(0);
    ctx.hwSetSpd(ctx.allowedBaud(0));
    Log::log().info(TAG, "=== P2 top-down sweep baud[0]=%lu dwellMs[0]=%d ===",
                    (unsigned long)ctx.allowedBaud(0), dwells_.phase2[0]);
    if (ctx.masterRole()) {
        ctx.sendSweepFrame(PING_CMD);
        phase2ElapsedMs_ = 0;
        // Sub-tick cadence (~250 ms, aligned to phase2Slave)
        // instead of arming the full ~1650 ms dwell — see the
        // fuller rationale in LinkTimersSwp.cpp's PHASE2
        // handling. Pinned by MasterPhase2PingCadenceTest.
        int subTick = dwells_.phase2Slave[0];
        if (subTick <= 0 || subTick > dwells_.phase2[0])
            subTick = dwells_.phase2[0];
        ctx.hwStartTimer(subTick);
    } else {
        // Slave enters P2 at spdI=0 (the proven baud). The
        // slave-P2 dwell is per-baud (phase2Slave[spdI]); the
        // first entry starts at the proven baud's dwell.
        ctx.hwStartTimer(dwells_.phase2Slave[0]);
    }
}

void LinkSweep::enterPhase3(ISweepCtx &ctx, int chosenBaud) {
    phase_ = SweepPhase::PHASE3;
    phase3Baud_ = chosenBaud;
    phase3Acks_ = 0;
    // Same rationale as enterPhase2: P3 falls back to
    // P1 on timeout if the peer never answered. The
    // P1+P2 PONGs are evidence the peer is alive; the
    // P3-fallback check consumes the same counter.
    ctx.hwSetSpd(ctx.allowedBaud(chosenBaud));
    Log::log().info(TAG, "=== P3 2-of-3 baud[%d]=%lu ===", chosenBaud,
                    (unsigned long)ctx.allowedBaud(chosenBaud));
    if (ctx.masterRole()) {
        ctx.sendSweepFrame(PING_CMD);
        int rt = roundTripMs(ctx.allowedBaud(chosenBaud));
        if (rt < 50)
            rt = 50;
        int t3 = rt * (PHASE3_ACKS_NEEDED + 1) + 100;
        if (t3 < 200)
            t3 = 200;
        ctx.hwStartTimer(t3);
        Log::log().info(TAG, "P3 master: t3=%d ms (rt=%d acksNeeded=%d)", t3,
                        rt, PHASE3_ACKS_NEEDED);
    } else {
        // Slave camp bound: must outlast the master's
        // full P3 budget, including the
        // RESWEEP_PREF_MAX_ATTEMPTS re-PINGs. The master
        // arms `t3` per attempt and retries up to
        // RESWEEP_PREF_MAX_ATTEMPTS+1 times before
        // falling through to a P1 walk, so the slave
        // needs to keep listening for at least that
        // whole window or it will abandon the camp
        // before the master's final PING lands.
        int rt = roundTripMs(ctx.allowedBaud(chosenBaud));
        if (rt < 50)
            rt = 50;
        int t3 = rt * (PHASE3_ACKS_NEEDED + 1) + 100;
        if (t3 < 200)
            t3 = 200;
        // (RESWEEP_PREF_MAX_ATTEMPTS + 1) attempts:
        // the initial P3 + the retry attempts.
        int t3s = t3 * (LinkSweep::RESWEEP_PREF_MAX_ATTEMPTS + 1);
        ctx.hwStartTimer(t3s);
        Log::log().info(TAG,
                        "P3 slave: camping %d ms at baud[%d]=%lu (waiting "
                        "for LOCK_CMD)",
                        t3s, chosenBaud,
                        (unsigned long)ctx.allowedBaud(chosenBaud));
    }
}

void LinkSweep::computeDwells(ISweepCtx &ctx) {
    int N = (int)(sizeof(dwells_.phase2) / sizeof(dwells_.phase2[0]));
    int pongSweepMs = 250 * ctx.allowedBaudsCount();
    int masterDwell = (int)(pongSweepMs * 1.1f);
    for (int i = 0; i < N; i++) {
        dwells_.phase2[i] = masterDwell;
        dwells_.phase2Slave[i] = 250;
    }
    int rt0 = roundTripMs(ctx.allowedBaud(0));
    dwells_.phase3 = (int)(3.0 * rt0 * 1.5) + 1;
    dwells_.phase1 = ctx.delayMs();
    int total = 0;
    for (int i = 0; i < N; i++)
        total += dwells_.phase2Slave[i];
    dwells_.phase2Total = total * 5 + 200;
    // Sweep dwell budget: dump the computed values once at
    // boot so an operator can see whether the dwell table
    // is the cause when the sweep never locks (e.g.
    // pongSweepMs=4000 + delayMs=5000 + phase3 = 8.4s, well
    // past idleTimeoutMs / 2 = 5s keepalive). Cheap (one
    // log line per boot) and gated to a real sweep call.
    Log::log().info(TAG,
                    "computeDwells: phase1=%d phase2[0]=%d phase2Slave[0]=%d "
                    "phase3=%d phase2Total=%d (baudCount=%d rt0=%d)",
                    dwells_.phase1, dwells_.phase2[0], dwells_.phase2Slave[0],
                    dwells_.phase3, dwells_.phase2Total,
                    ctx.allowedBaudsCount(), rt0);
}

int LinkSweep::phase1ArmMs(ISweepCtx &ctx) {
    if (!ctx.masterRole())
        return dwells_.phase2[0] + 200;
#ifdef AUTOLINK_HOST_TEST
    (void)ctx;
    return dwells_.phase1;
#else
    uint32_t seed = ctx.hwNowMs() ^ (ctx.masterRole() ? 0xA5u : 0x5Au);
    return jitterPhase1Dwell(dwells_.phase1, seed);
#endif
}

} // namespace autolink
