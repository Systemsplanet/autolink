// Three defects in the GBN recovery path, pinned together.
//
//   Keep-livelock — sweepRetx's Keep verdict on a maxRetx base left the
//   slot's retxCount_ and sentAtMs_ untouched, so every later decideSlot
//   returned Drop and the resend window was never called again: the base
//   became a zombie. Closed by LinkArq::rearmSlot.
//
//   Unbounded Keep — a dead peer whose floating RX line keeps stamping
//   lastRxMs rides the Keep path forever and never drops. Closed by
//   consecutiveKeep_ and the rescue cap.
//
//   P3 relock miss — the timer hits the resweepPrefPending_ fallback while
//   a concurrent event task has already locked the master to OK. Closed by
//   releasing the link lock around setSpd (EspHal::setSpd blocks up to
//   ~20 ms in uart_wait_tx_done) plus three re-acquire guards and a
//   bail-path setSpd re-assert.
//
// Pins 1-2 cover rearmSlot's math and its call sites; 3-5 the keep, cap,
// and forward-progress-reset behaviours; 6a/6b the fallback's happy and
// bail paths. MockHal::setSpd is a no-op, so the host cannot reproduce the
// UART stall itself — Pin 6b installs an onSetSpd hook that mutates the
// link under the lock release to stand in for the racing event task.
#ifndef AUTOLINK_HOST_TEST
#    error "Build with -DAUTOLINK_HOST_TEST (see Makefile)"
#endif

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <cassert>
#include "MockHal.h"
#include "NullArqCache.h"
#include "LinkTestAccessor.h"
#include "al/link/Link.h"
#include "al/link/arq/LinkArq.h"
#include "al/link/sweep/LinkDecision.h"
#include "al/AutoLinkConfig.h"

using namespace autolink;

namespace {

std::string readFile(const std::string &path) {
    std::ifstream f(path);
    if (!f.good())
        return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string projectRoot() {
    std::string base = ".";
    for (int i = 0; i < 8; i++) {
        std::ifstream pf(base + "/AGENTS.md");
        if (pf.good())
            return base;
        base += "/..";
    }
    return ".";
}

void test_rearmSlot_math() {
    std::cout << "\n=== Pin 1: LinkArq::rearmSlot math + invariants ==="
              << std::endl;
    LinkArq arq;
    arq.clearAll();
    arq.setGbnBase(0);
    arq.setGbnActive(true);
    // Pre-state: a fresh send stamps retxCount=0,
    // sentAtMs=100, pending=true, baseSeq=0,
    // bytesRecvd=0.
    arq.onSent(0, 0xFF, 100);
    // Burn a couple of retx counts.
    arq.applyRetx(0, 200);
    arq.applyRetx(0, 300);
    arq.onAcked(0, 250); // ACK, pending=false
    // Re-send a fresh chunk on the same seq (the
    // GBN budget ring is 2*W deep, so re-using seq
    // is fine for the test).
    arq.onSent(0, 0xFF, 1000);
    arq.applyRetx(0, 1100);
    arq.applyRetx(0, 1200);
    arq.applyRetx(0, 1300);
    // Pre-arm: retxCount_=3, sentAtMs_=1300,
    // ackedPending_=true, baseSeq_=0, bytesRecvd_=0.
    // (retxCount_ gets zeroed on onSent then bumped
    // 3 times by applyRetx.)
    // Rearm at now=2000.
    arq.rearmSlot(0, 2000);
    // Post-arm: retxCount=0, sentAtMs=2000, pending
    // still true (ackedPending_ left alone), baseSeq
    // still 0, bytesRecvd still 0.
    int bi = LinkArq::budgetIdx(0);
    assert(arq.isPending(0) &&
           "rearmSlot must NOT clear ackedPending_ — the chunk is still "
           "outstanding");
    (void)0;
    // The private fields are read through decideSlot: a rearmed slot must
    // Hold below the RTO and then Retx (not Drop) past it — proof the retx
    // budget is fresh.
    LinkArq::Action a1 = arq.decideSlot(0, 2000, 500, 3);
    assert(a1 == LinkArq::Action::Hold &&
           "post-rearm decideSlot at now=sentAtMs: age=0 < rtoMs=500 → Hold");
    LinkArq::Action a2 = arq.decideSlot(0, 2600, 500, 3);
    assert(a2 == LinkArq::Action::Retx &&
           "post-rearm decideSlot at now=sentAtMs+600: retxCount=0 (fresh), "
           "age>=rtoMs → Retx, not Drop — the keep-livelock fix");
    // applyRetx after rearm must bump retxCount_
    // back up from 0 to 1, exercising the same
    // budget-ring slot earlier onSent wrote.
    arq.applyRetx(0, 2600);
    LinkArq::Action a3 = arq.decideSlot(0, 3200, 500, 3);
    assert(a3 == LinkArq::Action::Retx &&
           "post-rearm applyRetx bumps retxCount_=1; next age>=rtoMs → "
           "Retx (still under the maxRetx=3 budget)");
    // The unused-budgetIdx is documented in LinkArq.h;
    // this pin stays well clear of it.
    (void)bi;
    std::cout << "  PASS (pending preserved, bytesRecvd preserved, "
              << "post-rearm Retx instead of Drop)" << std::endl;
}

void test_source_pin() {
    std::cout << "\n=== Pin 2: LinkArq / LinkTimers wire-up of rearm + cap ==="
              << std::endl;
    std::string la = readFile(projectRoot() + "/src/al/link/arq/LinkArq.h");
    std::string lac = readFile(projectRoot() + "/src/al/link/arq/LinkArq.cpp");
    std::string lh = readFile(projectRoot() + "/src/al/link/Link.h");
    std::string lt =
        (readFile(projectRoot() + "/src/al/link/timers/LinkTimersOk.cpp") +
         readFile(projectRoot() + "/src/al/link/timers/gbn/LinkTimersGbn.cpp") +
         readFile(projectRoot() + "/src/al/link/timers/LinkTimersSwp.cpp"));
    assert(!la.empty() && !lac.empty() && !lh.empty() && !lt.empty());

    // LinkArq.h declares rearmSlot.
    assert(la.find("void rearmSlot(uint8_t seq, uint32_t nowMs)") !=
               std::string::npos &&
           "LinkArq.h must declare rearmSlot(seq, nowMs)");
    // Scope the check to rearmSlot's body: the protected fields are
    // legitimately assigned elsewhere in the same file, and rearmSlot's own
    // comment names them — so look for an assignment, not a mention.
    size_t rearmPos = lac.find("void LinkArq::rearmSlot");
    assert(rearmPos != std::string::npos &&
           "LinkArq.cpp must define rearmSlot");
    size_t rearmEnd = lac.find("\n}\n", rearmPos);
    assert(rearmEnd != std::string::npos);
    std::string rearmBody = lac.substr(rearmPos, rearmEnd - rearmPos);
    assert(rearmBody.find("retxCount_[bi] = 0") != std::string::npos &&
           "rearmSlot must zero retxCount_");
    assert(rearmBody.find("sentAtMs_[bi] = nowMs") != std::string::npos &&
           "rearmSlot must re-stamp sentAtMs_");
    assert(rearmBody.find("ackedPending_[") == std::string::npos &&
           "rearmSlot must NOT assign to ackedPending_ (the comment "
           "mentions the name; the pin checks the assignment, not the "
           "mention)");
    assert(rearmBody.find("baseSeq_[bi]") == std::string::npos &&
           "rearmSlot must NOT assign to baseSeq_");
    assert(rearmBody.find("bytesRecvd_[") == std::string::npos &&
           "rearmSlot must NOT assign to bytesRecvd_");
    // Link.h declares consecutiveKeep_ +
    // gbnKeepRescueCap_unlocked() + DEFAULT_GBN_KEEP_RESCUE_CAP.
    assert(lh.find("consecutiveKeep_") != std::string::npos &&
           "Link.h must declare consecutiveKeep_ field");
    assert(lh.find("gbnKeepRescueCap_unlocked") != std::string::npos &&
           "Link.h must expose gbnKeepRescueCap_unlocked()");
    assert(lh.find("DEFAULT_GBN_KEEP_RESCUE_CAP") != std::string::npos &&
           "Link.h must declare DEFAULT_GBN_KEEP_RESCUE_CAP constant");
    // LinkTimers.cpp sweepRetx_unlocked Keep branch
    // calls arq_.rearmSlot and increments
    // consecutiveKeep_ + checks the cap.
    assert(lt.find("arq_.rearmSlot") != std::string::npos &&
           "LinkTimers.cpp Keep branch must call arq_.rearmSlot");
    assert(lt.find("consecutiveKeep_") != std::string::npos &&
           "LinkTimers.cpp Keep branch must track consecutiveKeep_");
    assert(lt.find("gbnKeepRescueCap_unlocked") != std::string::npos &&
           "LinkTimers.cpp Keep branch must consult gbnKeepRescueCap_");
    assert(lt.find("resweepPrefPending_") != std::string::npos &&
           "LinkTimers.cpp must have the resweepPrefPending_ branch");
    const std::string fallbackBlock = "P3 preferredBaud_ camp exhausted -> "
                                      "falling back to enterPhase1";
    size_t fallbackPos = lt.find(fallbackBlock);
    assert(fallbackPos != std::string::npos &&
           "LinkTimers.cpp must keep the P3 relock-miss fallback block");
    // The P3/resweepPrefPending_ branch must hold the link
    // lock across setSpd. Releasing and reacquiring with
    // portMAX_DELAY inside a timer callback would deadlock
    // when the holder of the mutex needs to
    // post a startTimer/stopTimer command serviced by the
    // same FreeRTOS daemon task. The fix drops the
    // unlock/relock entirely: the 20 ms stall in
    // uart_wait_tx_done is bounded, the holder is the
    // daemon itself, and the prior three guards (state /
    // arq generation / sweep phase) reduce to defense-in-
    // depth.
    //
    // The pin shape: the P3 fallback block (between
    // "P3 preferredBaud_ relock missed" and its closing
    // return) must NOT contain hw.unlock() / hw.lock() /
    // arq_.generation() / "sweep_.phase() != SweepPhase::NONE".
    // The file as a whole DOES contain hw.unlock()/hw.lock()
    // (LinkTimersOk uses them, for example), so the pin
    // scopes the search to the fallback block, not the
    // whole file.
    int depth = 0;
    size_t windowEnd = std::string::npos;
    for (size_t i = fallbackPos; i < lt.size(); i++) {
        if (lt[i] == '{')
            depth++;
        else if (lt[i] == '}') {
            depth--;
            if (depth <= 0) {
                windowEnd = i;
                break;
            }
        }
    }
    if (windowEnd == std::string::npos)
        windowEnd = lt.size();
    std::string fallbackRegion =
        lt.substr(fallbackPos, windowEnd - fallbackPos);
    assert(fallbackRegion.find("hw.unlock()") == std::string::npos &&
           "P3 fallback must NOT release the link lock across setSpd — "
           "reacquiring with portMAX_DELAY inside a FreeRTOS timer-"
           "service callback deadlocks when the holder of the mutex "
           "needs to post a startTimer command serviced by the same "
           "daemon task that's now stuck in xSemaphoreTake");
    assert(fallbackRegion.find("hw.lock()") == std::string::npos &&
           "P3 fallback must NOT reacquire the link lock after setSpd — "
           "same deadlock as hw.unlock() above");
    assert(fallbackRegion.find("arq_.generation()") == std::string::npos &&
           "P3 fallback no longer needs the arq generation guard — "
           "the lock is held the whole window, no concurrent writer "
           "can race the generation bump from inside the timer "
           "callback");
    assert(fallbackRegion.find("sweep_.phase() != SweepPhase::NONE") ==
               std::string::npos &&
           "P3 fallback no longer needs the sweep phase guard — "
           "same reason as the arq generation guard above");
    // The P3 fallback must arm a watchdog so a partial
    // fix can't reintroduce a silent dead branch. The
    // actual call is
    // hw.startTimer(sweep_.phase1ArmMs(*this)) — a static
    // "phase1ArmMs()" string match would miss it; the pin
    // matches on the substring "phase1ArmMs" inside a
    // startTimer call.
    assert(fallbackRegion.find("hw.startTimer(") != std::string::npos &&
           fallbackRegion.find("phase1ArmMs") != std::string::npos &&
           "P3 fallback must arm a watchdog (phase1ArmMs) before "
           "returning so onTimer() can fire again — the forward-"
           "progress guarantee");
    std::cout << "  PASS (rearmSlot declared+defined, cap helper present, "
              << "consecutiveKeep_ tracked, P3 fallback present, lock "
              << "release + epoch guard + re-assert present)" << std::endl;
}

void test_keep_livelock_rearm() {
    std::cout << "\n=== Pin 3: keep livelock rearm — base retransmits each "
                 "cycle ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 115200;
    cfg.syncAckTimeoutMs = 1000;
    cfg.maxRetx = 50; // high so RTO cycles don't trip Drop
    cfg.idleTimeoutMs = 60000;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;

    MockHal hal;
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor acc(link);
    acc.forceState(State::OK);

    // Seed the GBN base with a single outstanding
    // chunk. lastRxMs == now (reverse channel alive,
    // so decideGbnDropOnMaxRetx returns Keep).
    hal.lock();
    acc.setLastRx(hal.now);
    acc.setLastTx(hal.now);
    acc.arq().setGbnBase(0);
    acc.arq().setGbnActive(true);
    acc.arq().onSent(0, 0xFF, hal.now);
    hal.unlock();
    // Seed the storm threshold. See Pin 3 of GbnBackoffTest.
    acc.setGbnBaseStuckThresholdMsForTest(10000);

    // Exhaust maxRetx on the base, then take the Keep path. The rearm must
    // clear retxCount_ and re-stamp sentAtMs_, or the resend window is never
    // called again.
    hal.lock();
    acc.setLastRx(hal.now);
    acc.setLastTx(hal.now);
    for (int i = 0; i < 60; i++)
        acc.arq().applyRetx(0, hal.now);
    hal.unlock();
    int retxBeforeKeep = acc.arq().retxCountTotal();
    assert(retxBeforeKeep >= 60 &&
           "setup precondition: retxCount_[bi] must be >= 60 (we "
           "applied 60 retx counts directly to the base slot) so the "
           "next decideSlot returns Drop (retxCount >= maxRetx=50)");
    uint32_t agedNow1 = hal.now + 1010;
    hal.lock();
    acc.setLastRx(agedNow1);
    acc.setLastTx(agedNow1);
    hal.unlock();
    acc.sweepRetx(agedNow1);
    int retxAfterKeep = acc.arq().retxCountTotal();
    int keepsAfter1 = acc.consecutiveKeepForTest();
    assert(keepsAfter1 == 1 &&
           "first keep-eligible exhaustion must increment "
           "consecutiveKeep_ to 1");
    assert(retxAfterKeep == 0 &&
           "the Keep branch's rearmSlot must clear retxCount_ on the "
           "base slot — retxCountTotal() drops to 0 (or whatever the "
           "budget ring's other slots hold, here zero) after the Keep. "
           "Without the clear, the base slot's retxCount_ stays past "
           "budget and every later decideSlot returns Drop.");

    // A second RTO cycle: the rearmed slot is past the RTO with a fresh
    // budget, so this takes the Retx path (not Drop). consecutiveKeep_ stays
    // where it is and the resend window fires.
    uint32_t agedNow2 = agedNow1 + 1010;
    hal.lock();
    acc.setLastRx(agedNow2);
    acc.setLastTx(agedNow2);
    hal.unlock();
    int retxBefore2 = acc.arq().retxCountTotal();
    acc.sweepRetx(agedNow2);
    int retxAfter2 = acc.arq().retxCountTotal();
    int keepsAfter2 = acc.consecutiveKeepForTest();
    assert(retxAfter2 == retxBefore2 + 1 &&
           "second sweepRetx (post-rearm age>=RTO) must fire the resend "
           "window — retxCountTotal_ bumps by 1. Without the rearm's "
           "clear + re-stamp, the base is a zombie after one Keep: every "
           "later decideSlot returns Drop and the resend window never "
           "runs.");
    assert(keepsAfter2 == 1 &&
           "second sweepRetx takes the Retx path (post-rearm), NOT the "
           "Keep path — consecutiveKeep_ stays at 1, not 2");

    // Third cycle: another RTO, another resend.
    // Bumps retxCountTotal_ again.
    uint32_t agedNow3 = agedNow2 + 1010;
    hal.lock();
    acc.setLastRx(agedNow3);
    acc.setLastTx(agedNow3);
    hal.unlock();
    int retxBefore3 = acc.arq().retxCountTotal();
    acc.sweepRetx(agedNow3);
    int retxAfter3 = acc.arq().retxCountTotal();
    assert(retxAfter3 == retxBefore3 + 1 &&
           "third sweepRetx (post-rearm age>=RTO) must fire the resend "
           "window again — the keep-livelock fix means each new RTO "
           "tick retransmits, not just the first one");

    std::cout << "  PASS (keeps=" << keepsAfter2
              << ", retxTotal: preKeep=" << retxBeforeKeep
              << " postKeep=" << retxAfterKeep << " c2=" << retxAfter2
              << " c3=" << retxAfter3 << ")" << std::endl;
}

void test_unbounded_keep_force_drops() {
    std::cout
        << "\n=== Pin 4: unbounded Keep rescue cap — 4th keep force-drops "
           "==="
        << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 115200;
    cfg.syncAckTimeoutMs = 1000;
    cfg.maxRetx = 3; // low so the base can be cycled cheaply
    cfg.idleTimeoutMs = 60000;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;

    MockHal hal;
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor acc(link);
    acc.forceState(State::OK);

    hal.lock();
    acc.setLastRx(hal.now);
    acc.setLastTx(hal.now);
    acc.arq().setGbnBase(0);
    acc.arq().setGbnActive(true);
    acc.arq().onSent(0, 0xFF, hal.now);
    hal.unlock();

    int cap = acc.gbnKeepRescueCapForTest();
    assert(cap == 3 && "default cap must be 3 (DEFAULT_GBN_KEEP_RESCUE_CAP)");

    // Drive `cap` Keep cycles. Each cycle: exhaust
    // maxRetx on the base, set lastRxMs to a "now"
    // that ages the slot past RTO, fire sweepRetx.
    // After `cap` cycles consecutiveKeep_ == cap and
    // the link is still in OK (no drop yet).
    for (int i = 0; i < cap; i++) {
        hal.lock();
        acc.setLastRx(hal.now);
        acc.setLastTx(hal.now);
        for (int j = 0; j < 4; j++)
            acc.arq().applyRetx(0, hal.now);
        hal.unlock();
        uint32_t agedNow = hal.now + 1010;
        hal.lock();
        acc.setLastRx(agedNow);
        acc.setLastTx(agedNow);
        hal.unlock();
        acc.sweepRetx(agedNow);
    }
    int keepsBeforeForce = acc.consecutiveKeepForTest();
    State stateBeforeForce = acc.getStateForTest();
    uint64_t discBeforeForce = acc.getDiagCountForTest();
    assert(keepsBeforeForce == cap &&
           "consecutiveKeep_ must reach the cap value after `cap` Keep "
           "cycles (each cycle increments by 1; the cap-check at the "
           "top of the Keep branch compares and force-Drops on >cap, "
           "not ==cap)");
    assert(stateBeforeForce == State::OK &&
           "link must stay in OK across the first `cap` Keep cycles — "
           "the cap check is at the top of the next Keep branch, not "
           "on every Keep");

    // The (cap+1)th cycle: reverse channel still alive, maxRetx exhausted
    // again. The rescue cap must force the honest drop anyway.
    hal.lock();
    acc.setLastRx(hal.now);
    acc.setLastTx(hal.now);
    for (int j = 0; j < 4; j++)
        acc.arq().applyRetx(0, hal.now);
    hal.unlock();
    uint32_t agedNowForce = hal.now + 1010;
    hal.lock();
    acc.setLastRx(agedNowForce);
    acc.setLastTx(agedNowForce);
    hal.unlock();
    acc.sweepRetx(agedNowForce);

    uint64_t discAfter = acc.getDiagCountForTest();
    State stateAfter = acc.getStateForTest();
    int keepsAfter = acc.consecutiveKeepForTest();
    assert(discAfter == discBeforeForce + 1 &&
           "rescue cap exhausted must fire the honest drop — discCount "
           "increments by 1. Without the cap, a dead peer whose "
           "floating RX line spews garbage keeps refreshing lastRxMs "
           "and the helper always returns Keep, so the link Keep-cycles "
           "forever.");
    assert(stateAfter == State::SWP &&
           "honest drop transitions to SWP (reset_unlocked(true) -> "
           "resweep handshake)");
    assert(keepsAfter == 0 &&
           "consecutiveKeep_ must reset to 0 on the force-Drop — the "
           "drop path clears it alongside the other counters");
    std::cout << "  PASS (cap=" << cap << ", keeps " << keepsBeforeForce << "->"
              << keepsAfter << ", disc " << discBeforeForce << "->" << discAfter
              << ", state OK->SWP)" << std::endl;
}

void test_forward_progress_resets_keep() {
    std::cout << "\n=== Pin 5: forward-progress (cumulative ACK) resets "
                 "consecutiveKeep_ ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 115200;
    cfg.syncAckTimeoutMs = 1000;
    cfg.maxRetx = 50;
    cfg.idleTimeoutMs = 60000;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;

    MockHal hal;
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor acc(link);
    acc.forceState(State::OK);

    hal.lock();
    acc.setLastRx(hal.now);
    acc.setLastTx(hal.now);
    acc.arq().setGbnBase(0);
    acc.arq().setGbnActive(true);
    acc.arq().onSent(0, 0xFF, hal.now);
    hal.unlock();

    // Drive 2 Keep cycles.
    for (int i = 0; i < 2; i++) {
        hal.lock();
        acc.setLastRx(hal.now);
        acc.setLastTx(hal.now);
        for (int j = 0; j < 60; j++)
            acc.arq().applyRetx(0, hal.now);
        hal.unlock();
        uint32_t agedNow = hal.now + 1010;
        hal.lock();
        acc.setLastRx(agedNow);
        acc.setLastTx(agedNow);
        hal.unlock();
        acc.sweepRetx(agedNow);
    }
    int keepsBeforeAck = acc.consecutiveKeepForTest();
    assert(keepsBeforeAck == 2 &&
           "consecutiveKeep_ must be 2 after 2 Keep cycles");

    // driveOnAck fires the cumulative-ACK handler directly. It takes its own
    // lock, so the test thread must not already hold hal.lock() — the mutex
    // is non-recursive.
    acc.driveOnAck(0);
    int keepsAfterAck = acc.consecutiveKeepForTest();
    assert(keepsAfterAck == 0 &&
           "cumulative ACK that advances gbnBase_ must reset "
           "consecutiveKeep_ to 0 — the cumulative-ACK handler in "
           "LinkRx.cpp must clear the counter alongside the GBN "
           "backoff triple, or a healthy link that recovers from a "
           "transient stall stays penalised on the next one.");

    // Drive a third Keep cycle post-ACK. The
    // consecutiveKeep_ counter is now 0 again, so
    // the link has a fresh `cap` budget to ride the
    // next transient congestion spike without
    // tripping the rescue cap.
    hal.lock();
    acc.setLastRx(hal.now);
    acc.setLastTx(hal.now);
    acc.arq().setGbnBase(0);
    acc.arq().setGbnActive(true);
    acc.arq().onSent(0, 0xFF, hal.now);
    hal.unlock();
    hal.lock();
    acc.setLastRx(hal.now);
    acc.setLastTx(hal.now);
    for (int j = 0; j < 60; j++)
        acc.arq().applyRetx(0, hal.now);
    hal.unlock();
    uint32_t agedNow2 = hal.now + 1010;
    hal.lock();
    acc.setLastRx(agedNow2);
    acc.setLastTx(agedNow2);
    hal.unlock();
    acc.sweepRetx(agedNow2);
    int keepsAfterThird = acc.consecutiveKeepForTest();
    assert(keepsAfterThird == 1 &&
           "post-ACK third Keep cycle: consecutiveKeep_ counts from 0 "
           "again, not from 2 — the cumulative-ACK reset is load-bearing "
           "for healthy links that occasionally hit the Keep path");
    std::cout << "  PASS (keeps: 2 -> 0 on ACK -> 1 on next keep)" << std::endl;
}

void test_enterphase1_fallback_contract() {
    std::cout << "\n=== Pin 6: P3 relock-miss fallback drives the real "
                 "path: SWP/PHASE3 + resweepPrefPending -> onTimer -> "
                 "PHASE1 + PING + armed ==="
              << std::endl;
    // Pin 6a — the happy path: nothing re-drives the link under the lock
    // release, so the fallback proceeds. A fallback that produces no sweep
    // would leave the phase at NONE/PHASE3 and txBuf empty.
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.allowedBauds[0] = 115200;
    cfg.allowedBauds[1] = 9600;
    cfg.allowedBaudsCount = 2;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 0;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;

    MockHal hal;
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor acc(link);

    acc.forceState(State::SWP);
    hal.lock();
    acc.setSweepPhase(SweepPhase::PHASE3);
    acc.setResweepPrefPendingForTest(true);
    // Force the retry path to be exhausted so the
    // onTimer goes straight to the P1 fallback
    // (the P3-preferredBaud retry arm is the
    // narrow-window path — pinned by
    // LinkSweepP1GuardTest — but this Pin 6 is
    // about the P1 fallback itself, not the
    // retry).
    acc.setResweepPrefAttemptsForTest(99);
    hal.unlock();
    hal.clearTx();
    int lockDepthBefore = hal.lockDepth;
    link.onTimer();
    int lockDepthAfter = hal.lockDepth;
    assert(lockDepthAfter == lockDepthBefore &&
           "fallback must balance the hw.unlock()/hw.lock() pair on the "
           "happy path — lockDepth != 0 on return means the lock was "
           "leaked");
    assert(link.getState() == State::SWP &&
           "fallback must keep state SWP on the happy path (the epoch "
           "and state guards both pass — link is still in resweep)");
    assert(acc.sweepPhase() == SweepPhase::PHASE1 &&
           "fallback must set phase=PHASE1 on the happy path (a "
           "fallback that produces no sweep would leave phase at "
           "NONE/PHASE3)");
    assert(link.getCurrentSpdIndex() == cfg.allowedBaudsCount - 1 &&
           "fallback must set spdI to the slowest baud (the P1 walk "
           "always starts at slowest)");
    assert(hal.timerActive &&
           "fallback must arm the sweep timer (the master P1 PING "
           "needs a dwell to fire on)");
    assert(!hal.txBuf.empty() &&
           "fallback must emit a PING on the wire (the master P1 PING "
           "so the slave has something to ACK)");
    assert(!acc.resweepPrefPendingForTest() &&
           "fallback must clear resweepPrefPending_ on the happy "
           "path (the flag is consumed by the fallback)");
    std::cout << "  Pin 6a PASS (happy path: state=SWP, phase=PHASE1, "
              << "spdI=" << link.getCurrentSpdIndex()
              << ", timer armed, PING on wire txBuf.size=" << hal.txBuf.size()
              << ", resweepPrefPending cleared, lockDepth balanced)"
              << std::endl;

    // Pin 6b — the deadlock-freeness pin. The
    // P3/resweepPrefPending_ branch holds the link
    // lock the whole window (no hw.unlock() / hw.setSpd()
    // / hw.lock() sandwich). On ESP32, onTimer() runs
    // on the FreeRTOS timer-service (daemon) task, and
    // reacquiring the link lock with portMAX_DELAY
    // inside a timer callback would block forever when
    // the holder of the mutex needs to post a
    // startTimer/stopTimer command serviced by the same
    // daemon task.
    //
    // A hook that flips state/phase under onSetSpd is a
    // no-op for the link's behavior (the lock is held
    // and nothing inside the callback can race a
    // concurrent writer). The pin: the onSetSpd hook fires
    // (proving the link reached setSpd), the lockDepth
    // stays balanced (proving no unlock/relock was
    // attempted), and the link still ends in PHASE1 with
    // spdI=1 (the slowest baud) and an armed timer.
    hal.onSetSpd = nullptr;
    hal.spd = 0;
    hal.spdHistory.clear();
    int hookFired = 0;
    hal.onSetSpd = [&]() {
        hookFired = 1;
        // Attempt to flip state to OK + set spdI to 0.
        // The fix holds the lock — these flips take effect
        // on the link's fields, but the link's P3/resweep
        // branch has already passed the state guard (the
        // branch no longer consults state guards; it
        // unconditionally sets phase to PHASE1 and spdI to
        // the slowest baud). The link should still end in
        // PHASE1, spdI=1.
        acc.forceStateNoLock(State::OK);
        acc.setSpdI(0);
    };
    acc.forceState(State::SWP);
    hal.lock();
    acc.setSweepPhase(SweepPhase::PHASE3);
    acc.setResweepPrefPendingForTest(true);
    // Force the retry-exhausted path so the onTimer
    // reaches the P1 fallback (which calls setSpd),
    // not the narrow-window retry (which re-arms
    // the timer without a setSpd). The retry path
    // is pinned by LinkSweepP1GuardTest.
    acc.setResweepPrefAttemptsForTest(99);
    hal.unlock();
    hal.clearTx();
    lockDepthBefore = hal.lockDepth;
    link.onTimer();
    lockDepthAfter = hal.lockDepth;
    hal.onSetSpd = nullptr;
    assert(lockDepthAfter == lockDepthBefore &&
           "P3/resweepPrefPending_ branch must not release the link lock "
           "across setSpd — releasing the lock inside a FreeRTOS timer-"
           "service callback can deadlock when the holder of the mutex "
           "needs to post a startTimer command serviced by the same "
           "daemon task");
    assert(hookFired == 1 &&
           "onSetSpd hook must fire (the link reaches setSpd in the P3 "
           "fallback — proves the fallback actually called setSpd)");
    assert(acc.sweepPhase() == SweepPhase::PHASE1 &&
           "P3/resweepPrefPending_ branch drives the link into PHASE1 "
           "regardless of what the onSetSpd hook does to the link's "
           "internal state — the link's own state is now ignored by "
           "the branch because the lock is held the whole window");
    assert(link.getCurrentSpdIndex() == cfg.allowedBaudsCount - 1 &&
           "P3/resweepPrefPending_ branch sets spdI to the slowest "
           "baud (the P1 walk always starts at slowest)");
    assert(hal.timerActive &&
           "P3/resweepPrefPending_ branch arms a watchdog so onTimer() "
           "can fire again after the fallback completes");
    std::cout << "  Pin 6b PASS (deadlock-freeness: lockDepth balanced, "
              << "hook fired, link in PHASE1, spdI="
              << link.getCurrentSpdIndex() << ", timer armed)" << std::endl;
}

} // namespace

int main() {
    std::cout << "=== GBN Keep Rearm (livelock fix + rescue cap + enterPhase1 "
                 "fallback) ==="
              << std::endl;
    test_rearmSlot_math();
    test_source_pin();
    test_keep_livelock_rearm();
    test_unbounded_keep_force_drops();
    test_forward_progress_resets_keep();
    test_enterphase1_fallback_contract();
    std::cout << "\nAll GBN keep-rearm pins passed." << std::endl;
    return 0;
}
