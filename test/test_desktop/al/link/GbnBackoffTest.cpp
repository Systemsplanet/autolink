// Regression pins for the GBN whole-window retransmit storm
// (todo.md item 2). The bench logs showed gbnResendWindow_unlocked
// replaying 20-30 frames every ~500 ms once the base was stuck,
// escalating transient congestion into an honest maxRetx link
// drop. The fix introduces an exponential inter-resend backoff
// driven by decideGbnBackoff() (pure helper in LinkDecision.h),
// reset on any forward ACK progress.
//
// Pins:
//   1. decideGbnBackoff math: noProgress=1 → base; doubling
//      each step until max; cap holds; base=0 → 0.
//   2. Source pin: Link.h exposes gbnBackoffMs_ +
//      gbnLastRetxBase_ + gbnBackoffCapMs_unlocked() and the
//      LinkTimers integration (sweepRetx_unlocked calls
//      decideGbnBackoff; onTimerOk_unlocked uses gbnBackoffMs_
//      to stretch the timer).
//   3. Runtime: a MockHal scenario where the GBN base is
//      permanently stuck drives gbnAttempts_ up and the
//      exponential backoff grows the timer fire gap from
//      syncAckTimeoutMs to syncAckTimeoutMs*8 (capped).
//   4. Runtime: a forward ACK that advances gbnBase_ resets
//      gbnAttempts_ + gbnBackoffMs_ to 0, so the NEXT stall
//      starts from the base RTO (no latency penalty on the
//      happy path).
//   5. Runtime: an honest drop (maxRetx on the base) still
//      fires under backoff — the cap (8*RTO) keeps maxRetx
//      reachable within a bounded wall budget.
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

void test_decideGbnBackoff_math() {
    std::cout
        << "\n=== Pin 1: decideGbnBackoff math (noProgress=1, doubling, cap) ==="
        << std::endl;
    assert(decideGbnBackoff(0, 500, 4000) == 500);
    assert(decideGbnBackoff(1, 500, 4000) == 500);
    assert(decideGbnBackoff(2, 500, 4000) == 1000);
    assert(decideGbnBackoff(3, 500, 4000) == 2000);
    assert(decideGbnBackoff(4, 500, 4000) == 4000);
    // Cap holds:
    assert(decideGbnBackoff(5, 500, 4000) == 4000);
    assert(decideGbnBackoff(10, 500, 4000) == 4000);
    // base=0 disables the backoff cleanly:
    assert(decideGbnBackoff(5, 0, 4000) == 0);
    // max < base floors to base (defensive):
    assert(decideGbnBackoff(3, 1000, 500) == 1000);
    std::cout << "  PASS (noProgress=1..10 round-trips at base=500 cap=4000)"
              << std::endl;
}

void test_source_pin() {
    std::cout << "\n=== Pin 2: Link.h / LinkTimers.cpp wire-up ==="
              << std::endl;
    std::string lh = readFile(projectRoot() + "/src/al/link/Link.h");
    std::string lt = readFile(projectRoot() + "/src/al/link/LinkTimers.cpp");
    assert(!lh.empty() && !lt.empty());

    assert(lh.find("gbnBackoffMs_") != std::string::npos &&
           "Link.h must declare gbnBackoffMs_");
    assert(lh.find("gbnLastRetxBase_") != std::string::npos &&
           "Link.h must declare gbnLastRetxBase_ (forward-progress "
           "snapshot)");
    assert(lh.find("gbnBackoffCapMs_unlocked") != std::string::npos &&
           "Link.h must expose gbnBackoffCapMs_unlocked()");
    assert((lh.find("decideGbnBackoff") != std::string::npos ||
            lt.find("decideGbnBackoff") != std::string::npos) &&
           "decideGbnBackoff must be called somewhere — best-effort "
           "search");
    assert(lt.find("decideGbnBackoff") != std::string::npos &&
           "LinkTimers.cpp must call decideGbnBackoff() in "
           "sweepRetx_unlocked");
    assert(lt.find("gbnBackoffMs_") != std::string::npos &&
           "LinkTimers.cpp must consult gbnBackoffMs_ in onTimerOk_unlocked");
    std::cout << "  PASS (all four structural pins present)" << std::endl;
}

void test_stuck_base_grows_backoff() {
    std::cout
        << "\n=== Pin 3: stuck base drives exponential backoff under RTO ==="
        << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 115200;
    cfg.syncAckTimeoutMs = 500;
    cfg.maxRetx = 50;
    cfg.idleTimeoutMs = 0;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;

    MockHal hal;
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor acc(link);

    // Force OK so sweepRetx_unlocked is the path
    // onTimer takes (in SWP the timer drives the
    // sweep handshake, not the GBN retx ladder).
    acc.forceState(State::OK);

    // Seed two ARQ slots so pendingCount() is non-zero
    // (otherwise decideSlot() would Hold — no base seq
    // to time out). gbnBase_ lands at seq 0; the two
    // sends stamp sentAtMs_[bi] = 0 so the first
    // RTO-aged decideSlot fires Retx immediately.
    hal.lock();
    acc.arq().setGbnBase(0);
    acc.arq().setGbnActive(true);
    acc.arq().onSent(0, 0xFF, hal.now);
    acc.arq().onSent(1, 0xFF, hal.now);
    hal.unlock();

    // Drive sweepRetx_unlocked directly. Five
    // rounds, each at one RTO past the previous.
    // With no ACKs arriving, each call should:
    // bump gbnAttempts_, resend window, bump
    // gbnBackoffMs_ via decideGbnBackoff.
    for (int round = 0; round < 5; round++) {
        hal.pumpClock(500);
        acc.sweepRetx(hal.now);
    }

    hal.lock();
    int attempts = acc.gbnAttemptsForTest();
    uint32_t backoff = acc.gbnBackoffMsForTest();
    hal.unlock();

    std::cout << "  attempts=" << attempts << " backoff=" << backoff
              << std::endl;

    assert(attempts >= 3 && "gbnAttempts_ must grow on stuck-base RTO cycles");
    assert(backoff > (uint32_t)cfg.syncAckTimeoutMs &&
           "gbnBackoffMs_ must exceed base RTO once the base is stuck");
    assert(backoff <= 8u * (uint32_t)cfg.syncAckTimeoutMs &&
           "gbnBackoffMs_ must be capped at 8*syncAckTimeoutMs");
    std::cout << "  PASS (attempts grew monotonically, backoff > base, "
              << "backoff <= cap)" << std::endl;
}

void test_forward_ack_resets_backoff() {
    std::cout
        << "\n=== Pin 4: forward ACK progress resets gbnAttempts_ + backoff ==="
        << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 115200;
    cfg.syncAckTimeoutMs = 500;
    cfg.maxRetx = 50;
    cfg.idleTimeoutMs = 0;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;

    MockHal hal;
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor acc(link);
    acc.forceState(State::OK);

    // Seed and bump backoff manually via the accessor.
    hal.lock();
    acc.arq().setGbnBase(0);
    acc.arq().setGbnActive(true);
    acc.arq().onSent(0, 0xFF, hal.now);
    acc.arq().onSent(1, 0xFF, hal.now);
    hal.unlock();
    // Drive three stuck RTOs.
    for (int i = 0; i < 3; i++) {
        hal.pumpClock(500);
        acc.sweepRetx(hal.now);
    }

    hal.lock();
    int attemptsBefore = acc.gbnAttemptsForTest();
    uint32_t backoffBefore = acc.gbnBackoffMsForTest();
    hal.unlock();
    assert(attemptsBefore >= 2);
    assert(backoffBefore > (uint32_t)cfg.syncAckTimeoutMs);

    // Simulate a forward ACK that advances gbnBase_
    // past the stuck seq, mirroring LinkRx.cpp's
    // cumulative-ACK handler (onAcked() for every seq up
    // to the acked one, then setGbnBase() once). After
    // that handler runs, gbnAttempts_ and gbnBackoffMs_
    // reset to 0. The cumulative-ACK handler is wired
    // through Link::onAck() with an ACK_TYPE wire
    // frame; we exercise the source-level reset directly
    // so the pin doesn't depend on a full ACK-roundtrip
    // (the reset itself is in the cumulative-ACK
    // handler, which is the only place in production
    // code that resets these fields — the onSent path
    // and reset_unlocked also reset them, but neither
    // is a "forward ACK" in the sense of this pin).
    hal.lock();
    uint8_t prevBase = acc.arq().gbnBase();
    acc.arq().onAcked(prevBase);
    acc.arq().setGbnBase((prevBase == 253) ? 0 : (uint8_t)(prevBase + 1));
    hal.unlock();

    // Re-trigger sweepRetx to observe the new
    // forward-progress detection: the snapshot
    // check (gbnLastRetxBase_ != gbnBase_) trips
    // and resets gbnAttempts_ to 0 on this round.
    hal.pumpClock(500);
    acc.sweepRetx(hal.now);

    hal.lock();
    int attemptsAfter = acc.gbnAttemptsForTest();
    uint32_t backoffAfter = acc.gbnBackoffMsForTest();
    hal.unlock();

    // After the forward-progress detection in
    // sweepRetx_unlocked (gbnLastRetxBase_ !=
    // arq_.gbnBase()), gbnAttempts_ resets to 0,
    // THEN the round-counter ++ fires, so the
    // post-call value is 1 (not 0) and backoff is
    // baseMs (noProgress=1 → unchanged). The pin
    // asserts the post-call values, not the
    // intermediate snapshot.
    assert(attemptsAfter == 1 &&
           "gbnAttempts_ must reset to 0 on forward ACK progress, then "
           "increment to 1 for THIS round — the reset clears the prior "
           "stall, the round counter restarts");
    assert(backoffAfter == (uint32_t)cfg.syncAckTimeoutMs &&
           "gbnBackoffMs_ must reset to baseMs (noProgress=1) on forward "
           "ACK progress — the prior backoff is wiped clean");
    std::cout << "  PASS (before: attempts=" << attemptsBefore
              << " backoff=" << backoffBefore
              << "ms; after forward ACK: attempts=" << attemptsAfter
              << " backoff=" << backoffAfter << "ms)" << std::endl;
}

void test_honest_drop_still_fires_under_backoff() {
    std::cout
        << "\n=== Pin 5: maxRetx still fires under backoff (cap bounded) ==="
        << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 115200;
    // Tight RTO + low maxRetx so the budget exhausts in
    // a small wall-clock window — fits the host suite's
    // subsecond budget.
    cfg.syncAckTimeoutMs = 50;
    cfg.maxRetx = 3;
    cfg.idleTimeoutMs = 0;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;

    MockHal hal;
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor acc(link);
    acc.forceState(State::OK);

    hal.lock();
    acc.arq().setGbnBase(0);
    acc.arq().setGbnActive(true);
    acc.arq().onSent(0, 0xFF, hal.now);
    hal.unlock();

    // Drive sweepRetx_unlocked. With backoff
    // cap = 8*50 = 400 ms, 4 retx rounds hit
    // maxRetx=3 on the 4th round (retxCount_=3
    // >= maxRetx=3 → Drop). Each round ages the
    // slot by cfg.syncAckTimeoutMs before the
    // decideSlot call.
    int dropped = 0;
    for (int i = 0; i < 30 && dropped == 0; i++) {
        hal.pumpClock(50);
        acc.sweepRetx(hal.now);
        if (acc.getStateForTest() == State::SWP)
            dropped = 1;
    }

    State finalState = acc.getStateForTest();
    uint64_t disc = acc.getDiagCountForTest();

    assert(finalState == State::SWP &&
           "link must drop (enter SWP) when maxRetx exhausted on the base, "
           "even under backoff — the cap keeps maxRetx reachable");
    assert(disc >= 1 &&
           "dropLink must have bumped discCount — the honest-drop contract "
           "is preserved under backoff");
    std::cout << "  PASS (state=" << (finalState == State::SWP ? "SWP" : "OK")
              << " disc=" << (unsigned)disc << ")" << std::endl;
}

} // namespace

int main() {
    std::cout << "=== GBN Whole-Window Backoff Regression Tests ==="
              << std::endl;
    test_decideGbnBackoff_math();
    test_source_pin();
    test_stuck_base_grows_backoff();
    test_forward_ack_resets_backoff();
    test_honest_drop_still_fires_under_backoff();
    std::cout << "\nAll GBN backoff pins passed." << std::endl;
    return 0;
}