
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
    int slowest = ctx.allowedBaudsCount() - 1;
    ctx.setCurrentSpdI(slowest);
    ctx.hwSetSpd(ctx.allowedBaud(slowest));
    Log::log().info(TAG, "=== P1 slowest baud[%d]=%lu ===", slowest,
                    (unsigned long)ctx.allowedBaud(slowest));
    if (ctx.masterRole()) {
        ctx.sendFrame(PING_CMD);
        ctx.hwStartTimer(dwells_.phase1);
    } else {
        ctx.hwStartTimer(dwells_.phase2[0] + 200);
    }
}

void LinkSweep::enterPhase2(ISweepCtx &ctx) {
    phase_ = SweepPhase::PHASE2;
    ctx.setCurrentSpdI(0);
    ctx.hwSetSpd(ctx.allowedBaud(0));
    Log::log().info(TAG, "=== P2 top-down sweep ===");
    if (ctx.masterRole()) {
        ctx.sendFrame(PING_CMD);
        ctx.hwStartTimer(dwells_.phase2[0]);
    }
}

void LinkSweep::enterPhase3(ISweepCtx &ctx, int chosenBaud) {
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
