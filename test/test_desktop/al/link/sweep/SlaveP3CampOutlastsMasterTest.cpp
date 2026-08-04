// Slave P3 camp dwell must outlast the master's full
// P3 budget, including the LinkSweep::RESWEEP_PREF_MAX_ATTEMPTS
// re-PINGs. The master arms t3 per attempt and retries
// up to LinkSweep::RESWEEP_PREF_MAX_ATTEMPTS+1 times; the slave
// must keep listening for at least that whole window
// or it will abandon the camp before the master's
// final PING lands.
//
// Pin: 115200 baud at chosenBaud=2, slave enters P3.
// After the slave arms its camp timer, the arm must
// be at least t3 * (LinkSweep::RESWEEP_PREF_MAX_ATTEMPTS + 1).
// Toggle off (use phase2Slave[baud] floored at 100) ->
// red: arm too short.
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include "MockHal.h"
#    include "TestCfg.h"
#    include "NullArqCache.h"
#    include "accessors/LinkTestAccessor.h"
#    include "al/link/sweep/LinkDecision.h"
#    include "al/link/LinkWire.h"

int main() {
    using namespace autolink;
    std::cout << "=== Slave P3 camp dwell must outlast master ===\n";
    AutoLinkConfig cfg;
    testBaseCfg(cfg);
    cfg.allowedBauds[0] = 115200;
    cfg.allowedBauds[1] = 57600;
    cfg.allowedBauds[2] = 38400;
    cfg.allowedBaudsCount = 3;

    MockHal mHal, sHal;
    NullArqCache cache;
    Link pong(sHal, cache, false /*slave*/, cfg);
    pong.begin();
    LinkTestAccessor t(pong);

    int chosenBaud = 2;
    uint32_t baud = cfg.allowedBauds[chosenBaud];
    int rt = roundTripMs(baud);
    if (rt < 50)
        rt = 50;
    int t3 = rt * (PHASE3_ACKS_NEEDED + 1) + 100;
    if (t3 < 200)
        t3 = 200;
    int expectedMin = t3 * (LinkSweep::RESWEEP_PREF_MAX_ATTEMPTS + 1);

    // Set phase, then enter P3. The camp dwell must be
    // >= expectedMin.
    t.sweep().setPhase(SweepPhase::PHASE2);
    t.setSpdI(chosenBaud);
    sHal.spd = baud;
    t.sweep().enterPhase3(pong, chosenBaud);

    int actual = sHal.lastTimerMs;
    std::cout << "  expected >= " << expectedMin << " ms, actual = " << actual
              << " ms\n";
    assert(actual >= expectedMin &&
           "Slave P3 camp dwell must cover master's full P3 budget");

    std::cout << "  PASS (slave P3 camp covers master's full budget)\n";
    std::cout << "=== SlaveP3CampOutlastsMaster: PASS ===\n";
    return 0;
}

#endif
