// Baud-sweep phase machine implementation.
#include "al/link/LinkSweep.h"
#include "al/link/Link.h"
#include "al/link/LinkDecision.h"
#include "al/util/Log.h"

static constexpr const char *TAG = "AutoLink";
static constexpr int PHASE1_MAX_TRIES = 6;
static constexpr int PHASE3_ACKS_NEEDED = 2;

namespace autolink {
void LinkSweep::enterPhase1(Link &l) {
    // Never leave P1 until connected.
    phase_ = SweepPhase::PHASE1;
    int slowest = l.allowedBaudsCount() - 1;
    l.setCurrentSpdI(slowest);
    l.hwSetSpd(l.allowedBaud(slowest));
    Log::log().info(TAG, "=== P1 slowest baud[%d]=%lu ===", slowest,
                    (unsigned long)l.allowedBaud(slowest));
    if (l.masterRole()) {
        l.sendFrame_unlocked(PING_CMD);
        l.hwStartTimer(dwells_.phase1);
    } else {
        l.hwStartTimer(dwells_.phase1 * PHASE1_MAX_TRIES);
    }
}

void LinkSweep::enterPhase2(Link &l) {
    phase_ = SweepPhase::PHASE2;
    l.setCurrentSpdI(0);
    l.hwSetSpd(l.allowedBaud(0));
    Log::log().info(TAG, "=== P2 top-down sweep ===");
    if (l.masterRole()) {
        l.sendFrame_unlocked(PING_CMD);
        l.hwStartTimer(dwells_.phase2[0]);
    }
}

void LinkSweep::enterPhase3(Link &l, int chosenBaud) {
    phase_ = SweepPhase::PHASE3;
    phase3Baud_ = chosenBaud;
    phase3Acks_ = 0;
    l.hwSetSpd(l.allowedBaud(chosenBaud));
    Log::log().info(TAG, "=== P3 2-of-3 baud[%d]=%lu ===", chosenBaud,
                    (unsigned long)l.allowedBaud(chosenBaud));
    if (l.masterRole()) {
        l.sendFrame_unlocked(PING_CMD);
        int rt = (int)(2.0 * (5.0 * 10.0 / l.allowedBaud(chosenBaud) * 1000.0) +
                       0.5);
        if (rt < 5)
            rt = 5;
        int t3 = rt * (PHASE3_ACKS_NEEDED + 1) + 100;
        if (t3 < 200)
            t3 = 200;
        l.hwStartTimer(t3);
    }
}

void LinkSweep::enterResweep(Link &l) {
    phase_ = SweepPhase::PHASE2;
    int preferred = l.preferredBaudIndex();
    int retryLimit = l.baudRetryLimit();
    int baudRetries = l.baudRetries();
    if (preferred >= 0 && baudRetries < retryLimit) {
        l.setCurrentSpdI(preferred);
        l.incBaudRetries();
    } else if (preferred >= 0) {
        // Preferred baud exhausted; full sweep.
        l.setCurrentSpdI(0);
        l.clearPreferredBaud();
        l.clearBaudRetries();
    } else {
        l.setCurrentSpdI(0);
    }
    int spd = l.currentSpdI();
    l.hwSetSpd(l.allowedBaud(spd));
    int dwell = l.masterRole() ? dwells_.phase2[spd] : dwells_.phase2Slave[spd];
    l.hwStartTimer(dwell);
}

void LinkSweep::computeDwells(Link &l) {
    int N = l.allowedBaudsCount();
    int maxN = (int)(sizeof(dwells_.phase2) / sizeof(dwells_.phase2[0]));
    if (N > maxN)
        N = maxN;
    for (int i = 0; i < N; i++) {
        double rt = 2.0 * (5.0 * 10.0 / l.allowedBaud(i) * 1000.0) + 0.5;
        int d = (int)(rt * 1.5) + 1;
        if (d < 5)
            d = 5;
        dwells_.phase2[i] = d;
        dwells_.phase2Slave[i] = d;
    }
    double rt0 = 2.0 * (5.0 * 10.0 / l.allowedBaud(0) * 1000.0) + 0.5;
    dwells_.phase3 = (int)(3.0 * rt0 * 1.5) + 1;
    dwells_.phase1 = l.delayMs();
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

int LinkSweep::phase1ArmMs(Link &l) {
#ifdef AUTOLINK_HOST_TEST
    (void)l;
    return dwells_.phase1;
#else
    uint32_t seed = l.hwNowMs() ^ (l.masterRole() ? 0xA5u : 0x5Au);
    return jitterPhase1Dwell(dwells_.phase1, seed);
#endif
}

} // namespace autolink