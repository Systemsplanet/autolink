// Baud-sweep phase machine implementation.
#include "al/link/sweep/LinkSweep.h"
#include "al/link/LinkContext.h"
#include "al/link/sweep/LinkDecision.h"
#include "al/util/Log.h"

static constexpr const char *TAG = "AutoLink";
static constexpr int PHASE1_MAX_TRIES = 6;
static constexpr int PHASE3_ACKS_NEEDED = 2;

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
        ctx.hwStartTimer(dwells_.phase1 * PHASE1_MAX_TRIES);
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
        int rt =
            (int)(2.0 * (5.0 * 10.0 / ctx.allowedBaud(chosenBaud) * 1000.0) +
                  0.5);
        if (rt < 50)
            rt = 50;
        int t3 = rt * (PHASE3_ACKS_NEEDED + 1) + 100;
        if (t3 < 200)
            t3 = 200;
        ctx.hwStartTimer(t3);
    }
}

void LinkSweep::computeDwells(LinkContext &ctx) {
    int N = ctx.allowedBaudsCount();
    int maxN = (int)(sizeof(dwells_.phase2) / sizeof(dwells_.phase2[0]));
    if (N > maxN)
        N = maxN;
    for (int i = 0; i < N; i++) {
        double rt = 2.0 * (5.0 * 10.0 / ctx.allowedBaud(i) * 1000.0) + 0.5;
        int d = (int)(rt * 1.5) + 1;
        if (d < 5)
            d = 5;
        dwells_.phase2[i] = d;
        dwells_.phase2Slave[i] = d;
    }
    double rt0 = 2.0 * (5.0 * 10.0 / ctx.allowedBaud(0) * 1000.0) + 0.5;
    dwells_.phase3 = (int)(3.0 * rt0 * 1.5) + 1;
    dwells_.phase1 = ctx.delayMs();
    int total = 0;
    for (int i = 0; i < N; i++)
        total += dwells_.phase2[i];
    dwells_.phase2Total = total * 5 + 200;
    for (int i = 0; i < N; i++) {
        if (dwells_.phase2[i] < 5)
            dwells_.phase2[i] = 5;
        if (dwells_.phase2Slave[i] < 5)
            dwells_.phase2Slave[i] = 5;
    }
}

int LinkSweep::phase1ArmMs(LinkContext &ctx) {
#ifdef AUTOLINK_HOST_TEST
    (void)ctx;
    return dwells_.phase1;
#else
    uint32_t seed = ctx.hwNowMs() ^ (ctx.masterRole() ? 0xA5u : 0x5Au);
    return jitterPhase1Dwell(dwells_.phase1, seed);
#endif
}

} // namespace autolink