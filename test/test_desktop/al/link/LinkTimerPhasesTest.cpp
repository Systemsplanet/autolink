// Per-phase pins for the decomposed OK-tick: health verdict
// application and the retx sweep are independently drivable.
// Re-merging them into one blob (or reordering the sequence)
// turns a specific pin red here instead of only sagging a
// 15 s itest floor.
//
// The GBN rewrite removed the reorder-expiry phase (there's
// no held-frame pool left to expire) and changed the sweep
// phase's maxRetx verdict from "retire the slot" to "honest
// link drop" — the ArqCache pipeline window IS the cache now,
// so there's no cache-miss/fake-ACK path to fall back to.
//
// The dead-link backstop (a mutually-quiet link with an
// in-flight op) outranks the SYNC short-circuit so SYNC
// also gets a recovery path; the test's "symmetric idle +
// pending" verdict is now DropDeadLink instead of DropIdle.
#include <iostream>
#include <cassert>
#include <cstring>
#include "MockHal.h"
#include "TestCfg.h"
#include "LinkTestAccessor.h"
#include "al/link/arq/ArqCache.h"
#include "al/link/LinkHealth.h"

using namespace autolink;

struct Pair {
    AutoLinkConfig cfg;
    ArqCache a{ AUTOLINK_ARQ_PIPELINE_WINDOW };
    ArqCache b{ AUTOLINK_ARQ_PIPELINE_WINDOW };
    MockHal mHal, sHal;
    Link ping, pong;
    Pair(AutoLinkConfig c)
        : cfg(c), ping(mHal, a, true, cfg), pong(sHal, b, false, cfg) {
        mHal.peer = &sHal;
        sHal.peer = &mHal;
        ping.begin();
        pong.begin();
        lockPair(ping, pong, mHal, sHal);
        assert(ping.getState() == State::OK);
    }
};

static AutoLinkConfig mk() {
    AutoLinkConfig c;
    testBaseCfg(c);
    return c;
}

static void test_health_phase() {
    std::cout << "\n=== Pin 1: applyHealth Keeps a healthy link, drops a "
                 "symmetric-idle one ==="
              << std::endl;
    Pair p(mk());
    LinkTestAccessor t(p.ping);
    assert(t.applyHealth(p.mHal.now) == (int)HealthAction::Keep);
    assert(p.ping.getState() == State::OK);

    // Symmetric idle: both ages past idleTimeoutMs with a pending slot.
    // The dead-link backstop catches this before the SYNC short-circuit
    // would otherwise skip every mutual-quiet check, so the verdict is
    // DropDeadLink (not DropIdle — DropIdle is ASYNC-only and accepts
    // frameErrs as an alternative signal).
    t.arq().seedGbn(5);
    t.arq().onSent(5, 0xFF, p.mHal.now);
    uint32_t old = p.mHal.now;
    p.mHal.now += (uint32_t)p.cfg.idleTimeoutMs + 1000;
    t.setLastRx(old);
    t.setLastTx(old);
    Stats s0, s1;
    p.ping.getStats(s0);
    assert(t.applyHealth(p.mHal.now) == (int)HealthAction::DropDeadLink);
    p.ping.getStats(s1);
    assert(p.ping.getState() != State::OK && "verdict must reset the link");
    assert(s1.discCount == s0.discCount + 1);
    std::cout << "  PASS" << std::endl;
}

static void test_sweep_phase_retransmits() {
    std::cout << "\n=== Pin 2: sweepRetx resends the GBN base on RTO, no "
                 "drop yet ==="
              << std::endl;
    Pair p(mk());
    LinkTestAccessor t(p.ping);
    uint32_t t0 = p.mHal.now;
    uint8_t chunk[4] = { 1, 2, 3, 4 };
    p.a.insert(7, chunk, 4);
    t.setGbnBase(7);
    t.arq().onSent(7, 0xFF, t0);
    p.mHal.txBuf.clear();
    Stats s0, s1;
    p.ping.getStats(s0);
    bool brk = t.sweepRetx(t0 + (uint32_t)p.cfg.syncAckTimeoutMs + 1);
    p.ping.getStats(s1);
    assert(!brk && "one RTO round must not drop the link");
    assert(t.arq().isPending(7) &&
           "base stays pending — it's a resend, not "
           "an ACK");
    assert(!p.mHal.txBuf.empty() &&
           "the RTO must put a retransmit on the "
           "wire");
    assert(p.ping.getState() == State::OK);
    assert(s1.discCount == s0.discCount);
    std::cout << "  PASS" << std::endl;
}

static void test_sweep_phase_maxretx_drops_link() {
    std::cout << "\n=== Pin 3: sweepRetx maxRetx on the GBN base is an "
                 "honest link drop ==="
              << std::endl;
    Pair p(mk());
    LinkTestAccessor t(p.ping);
    uint32_t t0 = p.mHal.now;
    t.setGbnBase(7);
    t.arq().onSent(7, 0xFF, t0);
    for (int i = 0; i < p.cfg.maxRetx; i++)
        t.arq().applyRetx(7, t0);
    Stats s0, s1;
    p.ping.getStats(s0);
    bool brk = t.sweepRetx(t0 + (uint32_t)p.cfg.syncAckTimeoutMs + 1);
    p.ping.getStats(s1);
    assert(p.ping.getState() != State::OK &&
           "maxRetx on the base must drop the link, not retire the slot");
    assert(s1.discCount == s0.discCount + 1);
    (void)brk;
    std::cout << "  PASS" << std::endl;
}

int main() {
    Log::log().setLevel(Log::NONE);
    test_health_phase();
    test_sweep_phase_retransmits();
    test_sweep_phase_maxretx_drops_link();
    std::cout << "\nLinkTimerPhasesTest: all pins passed" << std::endl;
    return 0;
}
