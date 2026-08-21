// Slave P2 walk must visit every baud, including the
// slowest (N-1). The P2 walk steps spdI in the correct
// order so the slave listens at the slowest baud — the
// exact baud the master's P1/fallback parks on. A
// baud-mismatch storm would otherwise result: master
// walks to the slowest, slave camped at N-2.
//
// Pin: 5-baud config, slave enters P2 at spdI==0,
// drives the P2 timeout fire. After the wrap, the
// RX baud must be allowedBauds[N-1] (the slowest),
// not allowedBauds[N-2]. Toggle off (revert to
// spdI=N-1 then spdI-- before setSpd) -> red.
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include "MockHal.h"
#    include "TestCfg.h"
#    include "NullArqCache.h"
#    include "accessors/LinkTestAccessor.h"

int main() {
    using namespace autolink;
    std::cout << "=== Slave P2 wrap must visit slowest baud ===\n";
    AutoLinkConfig cfg;
    testBaseCfg(cfg);
    cfg.allowedBauds[0] = 115200;
    cfg.allowedBauds[1] = 57600;
    cfg.allowedBauds[2] = 38400;
    cfg.allowedBauds[3] = 19200;
    cfg.allowedBauds[4] = 9600;
    cfg.allowedBaudsCount = 5;
    cfg.idleTimeoutMs = 0;

    MockHal mHal, sHal;
    NullArqCache cache;
    Link pong(sHal, cache, false /*slave*/, cfg);
    pong.begin();
    LinkTestAccessor t(pong);

    // Pin slave to P2 at spdI=0.
    t.sweep().setPhase(SweepPhase::PHASE2);
    t.setSpdI(0);
    sHal.spd = cfg.allowedBauds[0];
    t.sweep().dwells().phase2Slave[0] = 50;

    // Arm the P2 timer and pump it to expiry.
    mHal.now = 0;
    sHal.now = 0;
    sHal.startTimer(50);
    for (int i = 0; i < 30; i++) {
        mHal.pumpClock(2);
        sHal.pumpClock(2);
    }

    // After the wrap: the slave must have set its RX
    // baud to allowedBauds[N-1] = 9600 at least once
    // during the walk. The onTimerSwp_unlocked branch
    // either continues the walk or wraps to slowest;
    // the wrap branch must setSpd at the slowest index
    // BEFORE decrementing.
    int spdI = pong.getCurrentSpdIndex();
    assert(spdI == cfg.allowedBaudsCount - 1 &&
           "Slave P2 wrap must land on slowest baud, not N-2");
    // RX baud must be the slowest, not the second-slowest.
    assert(sHal.spd == cfg.allowedBauds[cfg.allowedBaudsCount - 1] &&
           "RX baud must be slowest (N-1) after wrap, not N-2");

    std::cout << "  PASS (slave P2 wrap visited slowest baud "
              << cfg.allowedBauds[cfg.allowedBaudsCount - 1] << ")\n";
    std::cout << "=== SlaveP2WalkAllBauds: PASS ===\n";
    return 0;
}

#endif
