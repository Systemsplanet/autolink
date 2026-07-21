// Shared unit-test fixture: baseline config + OK-lock helper.
// Tests set per-pin fields (mode, timeouts) after testBaseCfg.
#pragma once
#include "MockHal.h"
#include "al/AutoLinkConfig.h"

namespace autolink {

inline void testBaseCfg(AutoLinkConfig &cfg) {
    static const uint32_t kBauds[] = { 115200, 57600, 38400, 19200, 9600 };
    for (int i = 0; i < 5; i++)
        cfg.allowedBauds[i] = kBauds[i];
    cfg.allowedBaudsCount = 5;
    cfg.pingSamplesPerBaud = 1;
    cfg.streamBufferSize = 131072;
    cfg.maxMsg = 2048;
    cfg.postLockQuietMs = 0;
}

// Pump both clocks + pipe both ways until both links are OK.
inline void lockPair(Link &ping, Link &pong, MockHal &mHal, MockHal &sHal,
                     int maxIters = 100, uint32_t stepMs = 50) {
    for (int i = 0; i < maxIters &&
         (ping.getState() != State::OK || pong.getState() != State::OK);
         i++) {
        mHal.pumpClock(stepMs);
        sHal.pumpClock(stepMs);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
    }
}

} // namespace autolink
