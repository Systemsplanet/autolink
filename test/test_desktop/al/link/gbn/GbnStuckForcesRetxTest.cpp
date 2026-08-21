// sweepRetx_unlocked's storm-stuck branch tested
// `if (a == Drop || baseStormStuck)` — so once the base-storm-stuck
// clock tripped, a genuine LinkArq::Action::Retx verdict from
// decideSlot (a real RTO, retxCount(base)=0) was routed into the
// honest-drop evaluation instead of being retransmitted. At 512000
// baud with no baud locked, ackRtoMs and gbnBaseStuckThresholdMs_
// both floor to syncAckTimeoutMs, so the first RTO and the stuck
// verdict land on the same tick — every one of three field
// disconnects (seq=170/63/181) fired this way, all with
// retxCount(base)=0.
//
// Pin 1: a coincident Retx must retransmit and rearm the stuck
// clock, not honest-drop. Pin 2: the new baseStormStuck+Hold+
// retxCount<2 branch also retransmits rather than dropping. Pin 3:
// the >=2-real-retx gate still lets the honest drop through once
// real failures accumulate and silence continues — the fix doesn't
// weaken GbnDropPolicyTest's existing peer-gone contract.
#ifndef AUTOLINK_HOST_TEST
#    error "Build with -DAUTOLINK_HOST_TEST (see Makefile)"
#endif

#include <iostream>
#include <cassert>
#include "MockHal.h"
#include "NullArqCache.h"
#include "LinkTestAccessor.h"
#include "al/link/Link.h"
#include "al/link/arq/LinkArq.h"
#include "al/AutoLinkConfig.h"

using namespace autolink;

namespace {

AutoLinkConfig makeCfg() {
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 512000;
    cfg.syncAckTimeoutMs = 500;
    cfg.maxRetx = 5;
    cfg.idleTimeoutMs = 100000;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    return cfg;
}

void test_real_retx_wins_over_coincident_stuck_verdict() {
    std::cout << "\n=== Pin 1: a real Retx verdict must retransmit, not "
                 "honest-drop, even when baseStormStuck fires on the same "
                 "tick ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg = makeCfg();
    MockHal hal;
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor acc(link);
    acc.forceState(State::OK);

    hal.lock();
    acc.arq().setGbnBase(170);
    acc.arq().setGbnActive(true);
    hal.unlock();
    hal.now += 100000;
    hal.now += 600;
    hal.lock();
    // Sent 600 ms ago, zero prior retransmits — the exact field
    // signature (retxCount(base)=0, base-change->drop gap ~800 ms).
    acc.arq().onSent(170, 170, hal.now - 600);
    hal.unlock();

    // No baud locked (spdI invalid) -> baudAwareRtoMs_unlocked() and
    // baudAwareStuckThresholdMs_unlocked() both floor to
    // syncAckTimeoutMs, same as the 512000 field collapse.
    acc.setGbnBaseStuckThresholdMsForTest((uint32_t)cfg.syncAckTimeoutMs);
    acc.gbnBaseStuckTrackedSeq_set_for_test(170);
    acc.gbnBaseStuckSinceMs_set_for_test(hal.now - 10000);

    bool brk = acc.sweepRetx(hal.now);

    std::cout << "  brk=" << (brk ? "true" : "false")
              << " retxCount=" << (int)acc.arq().retxCountFor(170) << " state="
              << (acc.getStateForTest() == State::OK ? "OK" : "SWP")
              << " gbnAttempts=" << acc.gbnAttemptsForTest() << std::endl;

    assert(!brk &&
           "a coincident Retx+baseStormStuck verdict must not request a "
           "BREAK/reset");
    assert(acc.getStateForTest() == State::OK &&
           "the link must stay OK — this is a progress-seeking "
           "retransmit, not an honest drop");
    assert(acc.arq().retxCountFor(170) == 1 &&
           "the base's Retx verdict must actually go on the wire "
           "(applyRetx) — swallowing it into the honest-drop branch "
           "leaves retxCount at 0, the exact field signature. Toggle "
           "the branch order off -> red.");
    assert(acc.gbnAttemptsForTest() == 1 &&
           "gbnRetxBaseAndRearm_unlocked's bookkeeping must run on the "
           "Retx path, not just applyRetx in isolation");
    assert(acc.gbnLastRetxBaseForTest() == 170);
    assert(acc.gbnBaseStuckSinceMs_for_test() == hal.now &&
           "the stuck clock must restart behind the retransmit — a "
           "stale clock re-fires baseStormStuck on the very next tick");
    assert(acc.consecutiveKeepForTest() == 0 &&
           "the honest-drop Keep/Drop evaluation must not have run at "
           "all on a real Retx verdict");
    std::cout << "  PASS (real Retx wins over the coincident storm-stuck "
                 "verdict)"
              << std::endl;
}

void test_stuck_hold_forces_retx_until_two_real_failures() {
    std::cout << "\n=== Pin 2+3: baseStormStuck+Hold forces one retx while "
                 "retxCount<2, honest-drops once real failures reach 2 with "
                 "continued silence ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg = makeCfg();
    MockHal hal;
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor acc(link);
    acc.forceState(State::OK);

    hal.lock();
    acc.arq().setGbnBase(63);
    acc.arq().setGbnActive(true);
    hal.unlock();
    acc.setGbnBaseStuckThresholdMsForTest((uint32_t)cfg.syncAckTimeoutMs);
    acc.gbnBaseStuckTrackedSeq_set_for_test(63);

    // Round A: sentAtMs_ fresh (age < ackRtoMs -> Hold), but the
    // storm-immune clock already reads stuck. Pin 2: this must
    // force one retransmit, not evaluate the honest drop
    // (retxCount(0) < 2).
    hal.now += 100000;
    hal.now += 600;
    hal.lock();
    acc.arq().onSent(63, 63, hal.now);
    hal.unlock();
    acc.gbnBaseStuckSinceMs_set_for_test(hal.now - 10000);
    bool brkA = acc.sweepRetx(hal.now);
    std::cout << "  round A: brk=" << (brkA ? "true" : "false")
              << " retxCount=" << (int)acc.arq().retxCountFor(63) << std::endl;
    assert(!brkA && "Pin 2: retxCount<2 must force a retransmit, not drop");
    assert(acc.getStateForTest() == State::OK);
    assert(acc.arq().retxCountFor(63) == 1);

    // Round B: only 100 ms later (still < ackRtoMs -> Hold), stage
    // the storm clock stuck again — a second real failure without
    // the per-slot RTO ever legitimately elapsing (the NAK-storm
    // livelock shape GbnBaseStuckLivelockTest also covers). Still
    // retxCount(1) < 2 -> must still retransmit, not drop.
    hal.now += 100;
    acc.gbnBaseStuckSinceMs_set_for_test(hal.now - 10000);
    bool brkB = acc.sweepRetx(hal.now);
    std::cout << "  round B: brk=" << (brkB ? "true" : "false")
              << " retxCount=" << (int)acc.arq().retxCountFor(63) << std::endl;
    assert(!brkB && "Pin 2: retxCount<2 must still force a retransmit");
    assert(acc.getStateForTest() == State::OK);
    assert(acc.arq().retxCountFor(63) == 2);

    // Round C: retxCount is now 2 with continued silence (lastRxMs
    // never stamped -> decideGbnDropOnMaxRetx must see peer-gone).
    // Pin 3: the >=2 gate must now let the honest drop through.
    hal.now += 100;
    acc.gbnBaseStuckSinceMs_set_for_test(hal.now - 10000);
    bool brkC = acc.sweepRetx(hal.now);
    std::cout << "  round C: brk=" << (brkC ? "true" : "false") << " state="
              << (acc.getStateForTest() == State::OK ? "OK" : "SWP")
              << std::endl;
    assert(acc.getStateForTest() == State::SWP &&
           "Pin 3: once real retx count reaches 2 with continued "
           "silence, the honest drop must still fire — the fix must "
           "not weaken earlier peer-gone contract");
    std::cout << "  PASS (forced retx while count<2, honest-drop once "
                 "count>=2 with silence)"
              << std::endl;
}

} // namespace

int main() {
    std::cout << "=== GBN stuck-base forces retx (not honest-drop) ==="
              << std::endl;
    test_real_retx_wins_over_coincident_stuck_verdict();
    test_stuck_hold_forces_retx_until_two_real_failures();
    std::cout << "\nAll GbnStuckForcesRetx pins passed." << std::endl;
    return 0;
}
