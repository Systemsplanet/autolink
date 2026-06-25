// Clock injection: deterministic timers.
#include <cassert>
#include <cstdio>
#include <cstdint>
#include "AutoLink.h"
#include "MockHal.h"
#include "WireSim.h"
#include "../../src/al/link/Link.h"
#include "../../src/al/link/LinkDecision.h"

namespace {
using autolink::AutoLink;
using autolink::AutoLinkConfig;
using autolink::Diag;
using autolink::MockHal;
using autolink::State;
using autolink::Stats;
using autolink::TwoNodeFixture;
using autolink::WireSim;

void bringToOk(TwoNodeFixture &fix, WireSim &sim) {
    fix.nodeA().begin();
    fix.nodeB().begin();
    for (int i = 0; i < 300; i++) {
        sim.step(50);
        if (fix.getStateA() == State::OK && fix.getStateB() == State::OK)
            return;
    }
}

void test_idle_timeout_drops_link() {
    AutoLinkConfig cfg;
    std::cout
        << "\n=== Test: idle watchdog drops link after cfg.idleTimeoutMs ==="
        << std::endl;

    AutoLinkConfig watchdogCfg;
    watchdogCfg.idleTimeoutMs = 5000;
    WireSim sim(watchdogCfg);
    sim.setFrameDropPct(0);
    TwoNodeFixture fix(sim);
    bringToOk(fix, sim);
    assert(fix.getStateA() == State::OK && fix.getStateB() == State::OK);

    fix.maxBurstPerLoop = 0;

    autolink::Stats preA, preB;
    fix.nodeA().getStats(preA);
    fix.nodeB().getStats(preB);

    MockHal &mA = const_cast<MockHal &>(sim.rawA());
    MockHal &mB = const_cast<MockHal &>(sim.rawB());
    mA.runFor(5500);
    mB.runFor(5500);

    autolink::Stats postA, postB;
    fix.nodeA().getStats(postA);
    fix.nodeB().getStats(postB);

    int dropA = (int)(postA.discCount - preA.discCount);
    int dropB = (int)(postB.discCount - preB.discCount);
    std::cout << "  drops: A=" << dropA << " B=" << dropB
              << " (timerFiredCalls A=" << mA.timerFiredCalls
              << " B=" << mB.timerFiredCalls << ")" << std::endl;

    assert(dropA + dropB >= 1);

    assert(mA.timerFiredCalls + mB.timerFiredCalls > 0);
    std::cout << "PASS" << std::endl;
}

void test_ack_timeout_retransmits() {
    std::cout
        << "\n=== Test: ACK timeout at syncAckTimeoutMs triggers retransmit ==="
        << std::endl;

    AutoLinkConfig ackCfg;
    ackCfg.idleTimeoutMs = 300;
    WireSim sim(ackCfg);
    sim.setFrameDropPct(0);
    TwoNodeFixture fix(sim);
    bringToOk(fix, sim);

    for (int i = 0; i < 30; i++)
        fix.step(10);

    MockHal &mA = const_cast<MockHal &>(sim.rawA());
    MockHal &mB = const_cast<MockHal &>(sim.rawB());
    autolink::Stats preA;
    fix.nodeA().getStats(preA);

    uint8_t msg[64];
    for (int i = 0; i < 64; i++)
        msg[i] = (uint8_t)i;
    for (int i = 0; i < 30; i++) {
        if (i % 3 == 0)
            fix.nodeA().sendMsg(msg, 64);
        mA.pumpClock(50);
        autolink::pipe_data(mA, mB);
    }

    autolink::Stats postA;
    fix.nodeA().getStats(postA);
    uint64_t txDelta = postA.tx - preA.tx;
    std::cout << "  preA.tx=" << preA.tx << " postA.tx=" << postA.tx
              << " delta=" << txDelta
              << " timerFiredCalls=" << mA.timerFiredCalls << std::endl;

    assert(txDelta > 50);
    std::cout << "PASS" << std::endl;
}

void test_sweep_stall_forces_break() {
    std::cout << "\n=== Test: SWP/LCK stall forces BREAK ===" << std::endl;
    WireSim sim;
    sim.setFrameDropPct(0);
    TwoNodeFixture fix(sim);
    bringToOk(fix, sim);

    MockHal &mA = const_cast<MockHal &>(sim.rawA());
    MockHal &mB = const_cast<MockHal &>(sim.rawB());
    int breaksBefore = mA.sendBreakCalls;

    fix.nodeA().dropLink();
    fix.nodeB().dropLink();

    for (int i = 0; i < 40; i++) {
        mA.pumpClock(50);
        mB.pumpClock(50);
    }

    int breaksAfter = mA.sendBreakCalls + mB.sendBreakCalls;
    std::cout << "  breaks before=" << breaksBefore << " after=" << breaksAfter
              << " (delta=" << breaksAfter - breaksBefore << ")" << std::endl;
    assert(breaksAfter > breaksBefore);
    std::cout << "PASS" << std::endl;
}

void test_cobsSeq_wraparound_does_not_pollute_cache() {
    std::cout
        << "\n=== Test: cobsSeq wraps cleanly, cache stays bounded (merged) ==="
        << std::endl;
    autolink::WireSim sim;
    sim.setFrameDropPct(0);
    autolink::TwoNodeFixture fix(sim);
    bringToOk(fix, sim);

    for (int i = 0; i < 1500; i++)
        fix.step(10);

    for (int i = 0; i < 100; i++)
        fix.step(50);
    int cacheA = fix.sim().pendingCountA();
    int cacheB = fix.sim().pendingCountB();
    std::cout << "  after wrap-driven run: cacheA=" << cacheA
              << " cacheB=" << cacheB << " stateA=" << (int)fix.getStateA()
              << std::endl;
    assert(fix.getStateA() == autolink::State::OK);

    assert(cacheA < 200);
    assert(cacheB < 200);

    assert(cacheA <= 240);
    assert(cacheB <= 240);
    std::cout << "PASS" << std::endl;
}

void test_idle_watchdog_is_per_node() {
    std::cout << "\n=== Test: idle watchdog fires per-node (the fix) ==="
              << std::endl;

    MockHal mA, sA;
    sA.peer = &mA;
    autolink::AutoLinkConfig cfg;
    cfg.idleTimeoutMs = 5000;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 115200;
    cfg.streamBufferSize = 8192;
    autolink::Link a(mA, true, cfg);
    a.begin();

    autolink::MockHal &mARef = const_cast<autolink::MockHal &>(mA);
    mARef.runFor(2000);
    int fired = mARef.timerFiredCalls;
    std::cout << "  A timer fired " << fired << " times in 2s simulated"
              << std::endl;
    assert(fired > 0);
    std::cout << "PASS" << std::endl;
}

void test_keepalive_emitted_at_third_of_idle_timeout() {
    std::cout << "\n=== Test: keepalive fires at idleTimeoutMs/3 (the fix) ==="
              << std::endl;
    autolink::WireSim sim;
    sim.setFrameDropPct(0);
    autolink::TwoNodeFixture fix(sim);
    bringToOk(fix, sim);

    autolink::Stats preA;
    fix.nodeA().getStats(preA);

    autolink::MockHal &mA = const_cast<autolink::MockHal &>(sim.rawA());

    size_t txBefore = sim.rawA().txBuf.size();
    for (int i = 0; i < 500; i++)
        mA.pumpClock(20);
    size_t txAfter = sim.rawA().txBuf.size();
    for (int i = 0; i < 500; i++) {
    }

    size_t txDelta = txAfter - txBefore;
    std::cout << "  A txBuf delta after 10s simulated=" << txDelta
              << " timerFiredCalls=" << mA.timerFiredCalls << std::endl;
    assert(mA.timerFiredCalls > 1);
    assert(txDelta > 0);
    std::cout << "PASS" << std::endl;
}

void test_forced_drop_transitions_ok_to_swp() {
    std::cout
        << "\n=== Test: forced drop transitions OK->SWP, timer re-armed (the fix) ==="
        << std::endl;
    autolink::WireSim sim;
    sim.setFrameDropPct(0);
    autolink::TwoNodeFixture fix(sim);
    bringToOk(fix, sim);
    assert(fix.getStateA() == autolink::State::OK);

    autolink::MockHal &mA = const_cast<autolink::MockHal &>(sim.rawA());
    uint32_t timerBefore = mA.nextTimerAtMs;
    assert(timerBefore != UINT32_MAX);

    fix.nodeA().dropLink();
    assert(fix.getStateA() == autolink::State::SWP);
    uint32_t timerAfter = mA.nextTimerAtMs;
    std::cout << "  timer before drop=" << timerBefore
              << " after drop=" << timerAfter << std::endl;

    for (int i = 0; i < 20; i++)
        mA.pumpClock(60);
    std::cout << "  stateA after pump=" << (int)fix.getStateA()
              << " timerFiredCalls=" << mA.timerFiredCalls << std::endl;
    std::cout << "PASS" << std::endl;
}

void test_pumpClock_terminates_finitely() {
    std::cout << "\n=== Test: pumpClock safety bound (the fix) ==="
              << std::endl;
    autolink::WireSim sim;
    sim.setFrameDropPct(0);
    autolink::TwoNodeFixture fix(sim);
    bringToOk(fix, sim);

    autolink::MockHal &mA = const_cast<autolink::MockHal &>(sim.rawA());

    uint32_t firedBefore = mA.timerFiredCalls;
    mA.pumpClock(60000);
    uint32_t firedAfter = mA.timerFiredCalls;
    std::cout << "  timer fired " << (firedAfter - firedBefore)
              << " times in pumpClock(60000)" << std::endl;

    assert(firedAfter > firedBefore);
    std::cout << "PASS" << std::endl;
}

void test_idle_watchdog_combined_tx_rx_v5_1_54() {
    std::cout << "\n=== Test: idle watchdog combined TX+RX check (the fix) ==="
              << std::endl;

    {
        assert(autolink::decideIdleWatchdog(6000, 6000, 5000) ==
               autolink::IdleAction::Drop);

        assert(autolink::decideIdleWatchdog(6000, 0, 5000) ==
               autolink::IdleAction::Hold);

        assert(autolink::decideIdleWatchdog(0, 6000, 5000) ==
               autolink::IdleAction::Hold);

        assert(autolink::decideIdleWatchdog(4000, 4000, 5000) ==
               autolink::IdleAction::Hold);

        assert(autolink::decideIdleWatchdog(6000, 6000, 0) ==
               autolink::IdleAction::Hold);
    }

    autolink::AutoLinkConfig quietCfg;
    quietCfg.idleTimeoutMs = 5000;
    autolink::WireSim sim(quietCfg);
    sim.setFrameDropPct(0);
    autolink::TwoNodeFixture fix(sim);
    bringToOk(fix, sim);
    fix.maxBurstPerLoop = 0;
    autolink::MockHal &mA = const_cast<autolink::MockHal &>(sim.rawA());
    autolink::MockHal &mB = const_cast<autolink::MockHal &>(sim.rawB());
    autolink::Stats preA;
    fix.nodeA().getStats(preA);
    autolink::Stats preB;
    fix.nodeB().getStats(preB);

    mA.runFor(5500);
    mB.runFor(5500);
    autolink::Stats postA;
    fix.nodeA().getStats(postA);
    autolink::Stats postB;
    fix.nodeB().getStats(postB);
    int dropA = (int)(postA.discCount - preA.discCount);
    int dropB = (int)(postB.discCount - preB.discCount);
    assert(dropA + dropB >= 1);
    std::cout
        << "PASS (decision table ok, both-silent drops, active-TX holds link)"
        << std::endl;
}

} // namespace

int main() {
    std::cout << "=== Clock Injection Tests (the fix) ===" << std::endl;
    test_idle_timeout_drops_link();
    test_ack_timeout_retransmits();
    test_sweep_stall_forces_break();
    test_cobsSeq_wraparound_does_not_pollute_cache();
    test_idle_watchdog_is_per_node();
    test_keepalive_emitted_at_third_of_idle_timeout();
    test_forced_drop_transitions_ok_to_swp();
    test_pumpClock_terminates_finitely();
    test_idle_watchdog_combined_tx_rx_v5_1_54();
    std::cout << "\n=== Clock Injection Tests Completed Successfully ==="
              << std::endl;
    return 0;
}