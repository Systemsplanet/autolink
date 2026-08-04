// Fix 5: the master sent exactly one PING_CMD per P2 dwell
// (~1650 ms at the field's 6-baud config) — one corrupted/lost PING
// forfeited the entire dwell, and CRC-fail lines confirmed
// corruption at baud-transition boundaries. enterPhase2 and
// onTimerSwp_unlocked now resend PING_CMD on a ~250 ms sub-tick
// cadence (aligned to phase2Slave) for the rest of the dwell
// instead of arming the full dwell after the first PING.
//
// Pin: two real nodes, both forced directly into PHASE2 (the P1
// handshake itself is untouched by this fix and out of scope here).
// The master's first PING (sent by enterPhase2 itself) is dropped
// before it ever reaches the slave. Only the sub-tick retry at
// t=250 ms is delivered. Both sides must still promote to PHASE3
// from that single retry, well inside the ~550 ms two-baud dwell
// this config computes — neither side may fall through to a
// baud-timeout walk. Revert to arming the full dwell after the
// first PING (drop the sub-tick resend) -> red: the slave's own
// 250 ms P2 dwell times out first (no PING ever arrived) and wraps
// away from baud[0] before the master's dropped PING would have
// been retried.
#ifndef AUTOLINK_HOST_TEST
#    error "Build with -DAUTOLINK_HOST_TEST (see Makefile)"
#endif

#include <iostream>
#include <cassert>
#include "MockHal.h"
#include "NullArqCache.h"
#include "LinkTestAccessor.h"
#include "al/link/Link.h"
#include "al/link/sweep/LinkSweep.h"
#include "al/AutoLinkConfig.h"

using namespace autolink;

namespace {

AutoLinkConfig makeCfg() {
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    cfg.maxMsg = 512;
    cfg.allowedBaudsCount = 2;
    cfg.allowedBauds[0] = 115200;
    cfg.allowedBauds[1] = 57600;
    cfg.syncAckTimeoutMs = 100;
    cfg.idleTimeoutMs = 0;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    return cfg;
}

void test_dropped_first_ping_still_promotes_within_dwell() {
    std::cout << "\n=== Pin: sub-tick PING resend recovers a dropped first "
                 "PING inside the same P2 dwell ==="
              << std::endl;
    AutoLinkConfig cfg = makeCfg();
    NullArqCache cacheA, cacheB;
    MockHal mHal, sHal;
    mHal.spd = cfg.allowedBauds[0];
    sHal.spd = cfg.allowedBauds[0];
    Link master(mHal, cacheA, true, cfg);
    Link slave(sHal, cacheB, false, cfg);
    master.begin();
    slave.begin();
    LinkTestAccessor mAcc(master), sAcc(slave);

    // Skip the P1 handshake (untouched by this fix) and force both
    // sides directly into P2 at baud[0], matching real post-P1
    // state. dwells_ are already computed by begin().
    mHal.lock();
    mAcc.sweep().enterPhase2(mAcc.link());
    mHal.unlock();
    sHal.lock();
    sAcc.sweep().enterPhase2(sAcc.link());
    sHal.unlock();

    int dwell = mAcc.sweep().dwells().phase2[0];
    std::cout << "  computed P2 dwell = " << dwell << " ms" << std::endl;
    assert(dwell > 250 && "test needs a dwell longer than one sub-tick");

    // enterPhase2's own PING (the "first PING of the dwell" the
    // fix doc describes) is on the wire now — drop it: never pipe
    // it to the slave.
    assert(!mHal.txBuf.empty() && "enterPhase2 must send the first PING");
    mHal.txBuf.clear();
    mHal.txBaudPerByte.clear();

    // Advance only the master's clock to the sub-tick boundary.
    // Firing the slave's own 250 ms P2 dwell timer here (before
    // the retried PING is delivered below) would wrap it away from
    // baud[0] in this two-baud config — the race this test must
    // not allow.
    mHal.pumpClock(250);
    assert(!mHal.txBuf.empty() &&
           "the sub-tick cadence must resend PING_CMD at ~250 ms — "
           "without it the dwell stays silent until the full ~550 ms "
           "elapses. Toggle off -> red.");

    // Deliver the retried PING and its PONG reply synchronously —
    // no more clock advance needed; SWP frames are handled inline
    // off onRx, not gated on a timer tick.
    pipe_data(mHal, sHal);
    pipe_data(sHal, mHal);

    std::cout << "  master phase=" << (int)mAcc.sweepPhase()
              << " spdI=" << master.getCurrentSpdIndex()
              << " | slave phase=" << (int)sAcc.sweepPhase()
              << " spdI=" << slave.getCurrentSpdIndex() << std::endl;

    assert(mAcc.sweepPhase() == SweepPhase::PHASE3 &&
           "the master must promote to PHASE3 from the retried PING's "
           "PONG reply, recovered inside the original dwell");
    assert(sAcc.sweepPhase() == SweepPhase::PHASE3 &&
           "the slave must promote to PHASE3 from the retried PING, "
           "never having timed out its own P2 dwell waiting for a "
           "PING that only arrived on the second attempt");
    assert(master.getCurrentSpdIndex() == 0 &&
           "recovery must happen at the original baud[0] — a walk "
           "away from baud[0] means the retry did not land before "
           "some other timeout fired");
    assert(slave.getCurrentSpdIndex() == 0);
    std::cout << "  PASS (promoted to PHASE3 at baud[0] from the sub-tick "
                 "retry, well inside the "
              << dwell << " ms dwell)" << std::endl;
}

} // namespace

int main() {
    std::cout << "=== Master P2 PING sub-tick cadence (Fix 5) ===" << std::endl;
    test_dropped_first_ping_still_promotes_within_dwell();
    std::cout << "\nAll MasterPhase2PingCadence pins passed." << std::endl;
    return 0;
}
