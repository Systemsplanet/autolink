// Regression pins for the peer-gone vs reverse-congested
// drop policy. The fix distinguishes two shapes of
// "maxRetx exhausted on the GBN base":
//
//   Peer-gone — no reverse traffic in the RTO window.
//   The peer is genuinely unreachable. Honest drop is
//   the only correct action: keep resending and the
//   base will never advance.
//
//   Reverse-congested — the peer is alive and its ACK
//   path is the bottleneck. We just resend-stormed the
//   uplink (or a cap'd prefix) on every RTO, the peer's
//   RX-FIFO drains slowly, ACKs trickle in late. maxRetx
//   on the base is a side-effect of our own resend storm
//   starving the reverse channel — the death-spiral the
//   inter-RTO backoff + live-window clamp + burst cap
//   made rarer but not impossible. Dropping here
//   tears down a live link on self-inflicted congestion,
//   and the resweep/re-lock cycle takes seconds — far
//   longer than the gap that a little patience would
//   have ridden out.
//
// The disambiguator is the reverse channel itself:
// has any inbound frame arrived within the RTO window?
// lastRxMs is the single timestamp the link already
// stamps on every inbound frame. If (now - lastRxMs)
// < rtoMs, the peer is on the wire and the base is
// just slow — Keep, reset the retx counter, let the
// backoff + cap keep the next round gentler. The
// RTO-exact boundary (now - lastRxMs) == rtoMs is
// "RTO elapsed exactly" → Drop, matching the
// decideGbnDropOnMaxRetx helper's strict-less-than
// comparison. Otherwise the peer is silent and the
// base will never advance —
// Drop, the honest link drop.
//
// Pins:
//   1. decideGbnDropOnMaxRetx math across the full
//      boundary grid: silent (now-lastRxMs > rtoMs) →
//      Drop; live (now-lastRxMs <= rtoMs) → Keep;
//      lastRxMs==0 → Drop; rtoMs==0 → Drop (test
//      escape hatch).
//   2. Source pin: LinkDecision.h defines
//      decideGbnDropOnMaxRetx + GbnDropDecision; the
//      LinkTimers.cpp sweepRetx_unlocked Drop branch
//      reads Link::lastRxMs and calls
//      decideGbnDropOnMaxRetx, and the Keep branch
//      resets gbnAttempts_ + gbnBackoffMs_ +
//      gbnLastRetxBase_ to 0 so the next stall starts
//      from base RTO.
//   3. Runtime: peer-gone scenario (lastRxMs far in
//      the past, base stuck) → sweepRetx trips the
//      honest drop; discCount increments.
//   4. Runtime: reverse-congested scenario (lastRxMs
//      fresh, base stuck) → sweepRetx returns false
//      (no sendBreak), discCount stays put,
//      gbnAttempts_ + gbnBackoffMs_ + gbnLastRetxBase_
//      reset to 0.
#ifndef AUTOLINK_HOST_TEST
#    error "Build with -DAUTOLINK_HOST_TEST (see Makefile)"
#endif

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <cassert>
#include <vector>
#include "MockHal.h"
#include "NullArqCache.h"
#include "LinkTestAccessor.h"
#include "al/link/Link.h"
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

void test_decideGbnDropOnMaxRetx_math() {
    std::cout
        << "\n=== Pin 1: decideGbnDropOnMaxRetx math (now/lastRxMs/rto) ==="
        << std::endl;
    // Silent (peer-gone): now - lastRxMs > rtoMs → Drop.
    assert(decideGbnDropOnMaxRetx(/*now=*/2000, /*lastRxMs=*/1000,
                                  /*rtoMs=*/500) == GbnDropDecision::Drop &&
           "now-lastRxMs (1000ms) > rtoMs (500ms) must Drop — peer silent");
    // Just-silent boundary: now - lastRxMs == rtoMs → Drop (<=, not <, so
    // equality is "silence" — the RTO has elapsed).
    assert(decideGbnDropOnMaxRetx(/*now=*/1500, /*lastRxMs=*/1000,
                                  /*rtoMs=*/500) == GbnDropDecision::Drop &&
           "now-lastRxMs == rtoMs must Drop (RTO elapsed exactly)");
    // Just-live boundary: now - lastRxMs < rtoMs → Keep.
    assert(decideGbnDropOnMaxRetx(/*now=*/1499, /*lastRxMs=*/1000,
                                  /*rtoMs=*/500) == GbnDropDecision::Keep &&
           "now-lastRxMs (499ms) < rtoMs (500ms) must Keep — peer alive");
    // Heavily live: ACKs flooding in → Keep.
    assert(decideGbnDropOnMaxRetx(/*now=*/1100, /*lastRxMs=*/1000,
                                  /*rtoMs=*/500) == GbnDropDecision::Keep &&
           "now-lastRxMs (100ms) << rtoMs (500ms) must Keep — peer very alive");
    // lastRxMs==0 (link never received a frame since reset) → Drop.
    assert(decideGbnDropOnMaxRetx(/*now=*/10000, /*lastRxMs=*/0,
                                  /*rtoMs=*/500) == GbnDropDecision::Drop &&
           "lastRxMs==0 must Drop — fresh link, no inbound ever, maxRetx "
           "on a base with zero reverse traffic is the textbook peer-gone");
    // rtoMs==0 (test escape hatch) → Drop, the legacy always-drop shape.
    assert(decideGbnDropOnMaxRetx(/*now=*/1000, /*lastRxMs=*/999,
                                  /*rtoMs=*/0) == GbnDropDecision::Drop &&
           "rtoMs==0 must Drop — test escape hatch for the legacy "
           "always-drop behavior");
    // Wraparound-safe: now < lastRxMs (clock wrap or test fixture) →
    // the difference is unsigned-arithmetic negative, which reads as
    // a huge positive; the helper should still call it Drop. (The
    // MockHal clock can't wrap in practice — it only counts up — but
    // the boundary test pins the defensive path.)
    assert(decideGbnDropOnMaxRetx(/*now=*/100, /*lastRxMs=*/200,
                                  /*rtoMs=*/500) == GbnDropDecision::Drop &&
           "now < lastRxMs (clock wrap) must Drop — the unsigned diff "
           "is huge, way past the RTO window");
    std::cout << "  PASS (silent / boundary=Drop / live / lastRxMs=0 / "
              << "rtoMs=0 / wraparound)" << std::endl;
}

void test_source_pin() {
    std::cout
        << "\n=== Pin 2: LinkDecision / LinkTimers wire-up of drop policy ==="
        << std::endl;
    std::string ld =
        readFile(projectRoot() + "/src/al/link/sweep/LinkDecision.h");
    std::string lt =
        (readFile(projectRoot() + "/src/al/link/timers/LinkTimersOk.cpp") +
         readFile(projectRoot() + "/src/al/link/timers/gbn/LinkTimersGbn.cpp") +
         readFile(projectRoot() + "/src/al/link/timers/LinkTimersSwp.cpp"));
    assert(!ld.empty() && !lt.empty());

    // The pure helper + enum live in LinkDecision.h.
    assert(ld.find("decideGbnDropOnMaxRetx") != std::string::npos &&
           "LinkDecision.h must define decideGbnDropOnMaxRetx");
    assert(ld.find("GbnDropDecision") != std::string::npos &&
           "LinkDecision.h must declare GbnDropDecision enum");
    // The Drop branch in sweepRetx_unlocked consults lastRxMs and the
    // helper; the Keep branch resets the backoff triple.
    assert(lt.find("decideGbnDropOnMaxRetx") != std::string::npos &&
           "LinkTimers.cpp sweepRetx_unlocked must call "
           "decideGbnDropOnMaxRetx on the Drop branch");
    assert(lt.find("GbnDropDecision::Keep") != std::string::npos &&
           "LinkTimers.cpp sweepRetx_unlocked must handle the Keep case");
    assert(lt.find("lastRxMs") != std::string::npos &&
           "LinkTimers.cpp sweepRetx_unlocked must read Link::lastRxMs "
           "to distinguish peer-gone from reverse-congested");
    std::cout << "  PASS (helper + enum + LinkTimers wire-up all present)"
              << std::endl;
}

void test_peer_gone_drops_honestly() {
    std::cout << "\n=== Pin 3: silent base (peer-gone) -> honest link drop ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 115200;
    cfg.syncAckTimeoutMs = 50;
    cfg.maxRetx = 3;
    // Long idle so the link's idle-timeout watchdog
    // (applyHealth) doesn't fire during the test — the
    // pin cares only about sweepRetx_unlocked's
    // maxRetx-on-base path.
    cfg.idleTimeoutMs = 60000;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;

    MockHal hal;
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor acc(link);
    acc.forceState(State::OK);

    // Stamp lastRxMs as "never" (0). The peer has been
    // silent forever. The base is stuck (maxRetx
    // exhausted). sweepRetx must trip the honest drop.
    hal.lock();
    acc.setLastRx(0); // lastRxMs==0: never received a frame
    acc.setLastTx(hal.now);
    acc.arq().setGbnBase(0);
    acc.arq().setGbnActive(true);
    acc.arq().onSent(0, 0xFF, hal.now);
    for (int i = 0; i < 10; i++)
        acc.arq().applyRetx(0, hal.now);
    hal.unlock();
    // Seed the storm threshold (forceState bypasses
    // lockOk). See BaudAwareStuckThresholdTest.
    acc.setGbnBaseStuckThresholdMsForTest(10000);
    uint64_t discBefore = acc.getDiagCountForTest();

    // Pump the RTO to age the slot past decideSlot()'s
    // Hold threshold (age < ackRtoMs → Hold), then call
    // sweepRetx directly so the test isn't at the mercy
    // of the timer arm. setLastTx(hal.now) right before
    // the pump keeps applyHealth's txAge check satisfied
    // (txAge is small → DropIdle doesn't fire).
    hal.lock();
    acc.setLastTx(hal.now);
    hal.unlock();
    hal.pumpClock(cfg.syncAckTimeoutMs + 10);
    hal.lock();
    acc.setLastTx(hal.now);
    hal.unlock();
    bool brk = acc.sweepRetx(hal.now);

    uint64_t discAfter = acc.getDiagCountForTest();
    State st = acc.getStateForTest();

    // Peer-gone: discCount must increment and the link
    // must leave OK (reset_unlocked(true) is the honest
    // drop path — it increments discCount and resweeps
    // to SWP if a baud sweep is configured). The test
    // doesn't care which post-drop state it lands in, only
    // that discCount went up.
    assert(discAfter == discBefore + 1 &&
           "peer-gone (lastRxMs==0) maxRetx-on-base must trip the honest "
           "drop — discCount must increment by 1");
    assert(st != State::OK &&
           "peer-gone maxRetx-on-base must leave the OK state (reset "
           "transitions to SWP for the resweep handshake)");
    std::cout << "  PASS (disc: " << discBefore << "->" << discAfter
              << ", state: OK->" << (st == State::SWP ? "SWP" : "OK")
              << ", brk=" << brk << ")" << std::endl;
}

void test_reverse_congested_keeps_and_resets() {
    std::cout << "\n=== Pin 4: live-ACK base (reverse-congested) -> keep + "
                 "reset backoff triple ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 115200;
    cfg.syncAckTimeoutMs = 1000;
    cfg.maxRetx = 50; // high so the RTO cycles don't trip Drop
    // Long idle so the link's idle-timeout watchdog
    // (applyHealth) doesn't fire during the test — the
    // pin cares only about sweepRetx_unlocked's
    // maxRetx-on-base path.
    cfg.idleTimeoutMs = 60000;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;

    MockHal hal;
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor acc(link);
    acc.forceState(State::OK);

    // Stamp lastRxMs and lastTxMs as "now" so the link's
    // idle-timeout watchdog (applyHealth) doesn't fire
    // during the setup — the test cares only about
    // sweepRetx_unlocked's maxRetx-on-base path, not
    // the idle watchdog.
    hal.lock();
    acc.setLastRx(hal.now);
    acc.setLastTx(hal.now);
    acc.arq().setGbnBase(0);
    acc.arq().setGbnActive(true);
    acc.arq().onSent(0, 0xFF, hal.now);
    hal.unlock();
    // Seed the storm threshold. See Pin 3.
    acc.setGbnBaseStuckThresholdMsForTest(10000);
    // Drive three stuck RTOs so gbnAttempts_ grows past
    // 1 and gbnBackoffMs_ is bumped above baseMs. The
    // RTO cycles don't trip the Drop branch
    // (maxRetx=50, we apply <50 retx counts). Set
    // lastRxMs/lastTxMs to hal.now BEFORE each pumpClock
    // so the timer arm's applyHealth + sweepRetx see a
    // live reverse channel (the helper's Keep path
    // returns Keep rather than Drop, and applyHealth's
    // DropIdle needs both rxAge>idle and txAge>idle,
    // which is false here).
    for (int i = 0; i < 3; i++) {
        hal.lock();
        acc.setLastRx(hal.now);
        acc.setLastTx(hal.now);
        hal.unlock();
        hal.pumpClock(cfg.syncAckTimeoutMs + 10);
        acc.sweepRetx(hal.now);
    }
    int attemptsBefore = acc.gbnAttemptsForTest();
    uint32_t backoffBefore = acc.gbnBackoffMsForTest();
    assert(attemptsBefore >= 2 &&
           "setup precondition: gbnAttempts_ must grow on stuck RTOs");
    assert(backoffBefore > (uint32_t)cfg.syncAckTimeoutMs &&
           "setup precondition: gbnBackoffMs_ must exceed base RTO");

    // Now exhaust maxRetx on the base so the NEXT
    // sweepRetx trips the Drop branch. Keep the reverse
    // channel alive (lastRxMs == now) so the keep path
    // fires instead of the honest drop. The RTO is 1000
    // ms; we pass sweepRetx a "now" that's > sentAtMs_ +
    // ackRtoMs so decideSlot sees the slot as aged past
    // RTO (and trips Drop), and we set lastRxMs to the
    // same "now" so the helper sees now-lastRxMs = 0
    // < rtoMs (and returns Keep). No pumpClock needed —
    // the slot's age is the difference between the
    // timestamp we pass and the timestamp applyRetx
    // stamped, not the MockHal's now.
    hal.lock();
    acc.setLastRx(hal.now);
    acc.setLastTx(hal.now);
    for (int i = 0; i < 60; i++)
        acc.arq().applyRetx(0, hal.now);
    hal.unlock();
    // Age the slot past RTO by passing a "now" 1010ms
    // ahead. lastRxMs is set to this same "now" so the
    // helper sees now-lastRxMs = 0 and returns Keep.
    uint32_t agedNow = hal.now + 1010;
    hal.lock();
    acc.setLastRx(agedNow);
    acc.setLastTx(agedNow);
    hal.unlock();
    bool brk = acc.sweepRetx(agedNow);

    uint64_t discAfter = acc.getDiagCountForTest();
    State st = acc.getStateForTest();
    int attemptsAfter = acc.gbnAttemptsForTest();
    uint32_t backoffAfter = acc.gbnBackoffMsForTest();
    uint8_t lastRetxBaseAfter = acc.gbnLastRetxBaseForTest();

    // Reverse-congested: discCount must NOT increment,
    // state must stay OK, and the backoff triple must
    // be reset to (0, 0, 0xFF) so the next stall starts
    // fresh from base RTO. brk must be false (the
    // function's return is the "sendBreak" flag, which
    // is only set on the SWP-state path; the keep path
    // returns false to mean "no break needed").
    assert(discAfter == 0 &&
           "reverse-congested (lastRxMs==now) maxRetx-on-base must NOT "
           "drop — discCount must stay 0");
    assert(st == State::OK &&
           "reverse-congested maxRetx-on-base must stay in OK state — the "
           "honest drop is skipped");
    assert(attemptsAfter == 0 &&
           "reverse-congested path must reset gbnAttempts_ to 0 — the "
           "next stall starts fresh from base RTO, not from a backed-off "
           "ratcheted value (pre-call value was >=2, so the reset path "
           "actually fired)");
    assert(backoffAfter == 0 &&
           "reverse-congested path must reset gbnBackoffMs_ to 0 — the "
           "exponential cadence is wiped clean (pre-call value was > "
           "baseMs, so the reset path actually fired)");
    assert(lastRetxBaseAfter == 0xFF &&
           "reverse-congested path must reset gbnLastRetxBase_ to 0xFF — "
           "the forward-progress sentinel, so the next sweepRetx round "
           "doesn't see a stale snapshot");
    assert(brk == false &&
           "reverse-congested path must return false (no sendBreak) — the "
           "drop didn't fire");
    std::cout << "  PASS (disc stays 0, state OK, attempts " << attemptsBefore
              << "->" << attemptsAfter << ", backoff " << backoffBefore
              << "ms->" << backoffAfter << "ms, lastRetxBase=0xFF, brk=false)"
              << std::endl;
}

} // namespace

int main() {
    std::cout << "=== GBN Drop Policy (peer-gone vs reverse-congested) "
                 "Regression Tests ==="
              << std::endl;
    test_decideGbnDropOnMaxRetx_math();
    test_source_pin();
    test_peer_gone_drops_honestly();
    test_reverse_congested_keeps_and_resets();
    std::cout << "\nAll GBN drop-policy pins passed." << std::endl;
    return 0;
}
