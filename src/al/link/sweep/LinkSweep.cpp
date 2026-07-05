// Baud-sweep phase machine implementation.
#include "al/link/sweep/LinkSweep.h"
#include "al/link/LinkContext.h"
#include "al/link/arq/ArqCache.h"
#include "al/link/sweep/LinkDecision.h"
#include "al/util/Log.h"

static constexpr const char *TAG = "AutoLink";

namespace autolink {
void LinkSweep::enterPhase1(LinkContext &ctx) {
    // Never leave P1 until connected.
    phase_ = SweepPhase::PHASE1;
    int slowest = ctx.allowedBaudsCount() - 1;
    ctx.setCurrentSpdI(slowest);
    ctx.hwSetSpd(ctx.allowedBaud(slowest));
    Log::log().info(TAG, "=== P1 slowest baud[%d]=%lu ===", slowest,
                    (unsigned long)ctx.allowedBaud(slowest));
    if (ctx.masterRole()) {
        ctx.sendFrame(PING_CMD);
        ctx.hwStartTimer(dwells_.phase1);
    } else {
        // Slave must outlast the master's worst-case P2 dwell
        // (one master P2 slot = 250 * N * 1.1 ms) plus a
        // scheduling slack. Tying to phase2[0] means the
        // guard scales with the dwell table, not a magic
        // constant that drifts when the baud count changes.
        ctx.hwStartTimer(dwells_.phase2[0] + 200);
    }
}

void LinkSweep::enterPhase2(LinkContext &ctx) {
    phase_ = SweepPhase::PHASE2;
    ctx.setCurrentSpdI(0);
    ctx.hwSetSpd(ctx.allowedBaud(0));
    Log::log().info(TAG, "=== P2 top-down sweep ===");
    if (ctx.masterRole()) {
        ctx.sendFrame(PING_CMD);
        ctx.hwStartTimer(dwells_.phase2[0]);
    }
}

void LinkSweep::enterPhase3(LinkContext &ctx, int chosenBaud) {
    phase_ = SweepPhase::PHASE3;
    phase3Baud_ = chosenBaud;
    phase3Acks_ = 0;
    ctx.hwSetSpd(ctx.allowedBaud(chosenBaud));
    Log::log().info(TAG, "=== P3 2-of-3 baud[%d]=%lu ===", chosenBaud,
                    (unsigned long)ctx.allowedBaud(chosenBaud));
    if (ctx.masterRole()) {
        ctx.sendFrame(PING_CMD);
        int rt = roundTripMs(ctx.allowedBaud(chosenBaud));
        if (rt < 50)
            rt = 50;
        int t3 = rt * (PHASE3_ACKS_NEEDED + 1) + 100;
        if (t3 < 200)
            t3 = 200;
        ctx.hwStartTimer(t3);
    }
}

// P2 master dwell = 1.1 × the slave's full sweep time. The
// slave sweeps all bauds at 250 ms each; the master must
// hold each baud long enough that a slave whose sweep
// started at any point still lands back on the master's
// current baud within the window. Per-baud round-trip
// (~1 ms at 115200) is dominated by FreeRTOS tick + UART
// event-task scheduling slack on real hardware.
void LinkSweep::computeDwells(LinkContext &ctx) {
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
}

int LinkSweep::phase1ArmMs(LinkContext &ctx) {
    // Slave P1 arm must outlast one master P2 dwell (phase2[0])
    // so the slave's listen window covers the master's PING
    // without racing the master's P2 timer. Master keeps the
    // short phase1 dwell — it's in charge of PINGing and a
    // short arm just sends another PING sooner.
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