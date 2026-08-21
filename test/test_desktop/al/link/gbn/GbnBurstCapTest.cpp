// Regression pins for the GBN whole-window retransmit burst cap.
// gbnResendWindow_unlocked replaying every pending frame verbatim on
// every RTO once the base is stuck escalates transient congestion
// into an honest maxRetx link drop. The cap
// (cfg.gbnResendBurstMax, default 8) clamps the per-RTO
// resend width via the pure decideGbnResendCap() helper.
// SYNC mode never enters the GBN path (stop-and-wait),
// so the cap is a no-op there. ASYNC SEQUENTIAL paths
// are unaffected for the common case (cap is large
// enough to cover a typical SEQUENTIAL message's chunk
// count); the cap only changes behaviour when
// pendingCount() exceeds it, which only happens under
// sustained congestion.
//
// Pins:
//   1. decideGbnResendCap math: pending<cap returns
//      pending; pending>cap returns cap; cap<=0 returns
//      0; pending<=0 returns 0.
//   2. Source pin: LinkTimers.cpp's gbnResendWindow_unlocked
//      uses decideGbnResendCap and reads cfg.gbnResendBurstMax;
//      AutoLinkConfig.h declares the field with a default of
//      8.
//   3. Runtime: a 20-pending scenario replays exactly
//      min(20, cap=8) = 8 frames per RTO — observed via
//      the post-call retx-count total.
//   4. Runtime: a 5-pending scenario (under the cap)
//      replays all 5 frames per RTO — the cap is a
//      floor, not a ceiling that ignores the actual
//      pending depth.
//   5. Runtime: cap=0 short-circuits the resend to no
//      frames (the field's documented disable semantic).
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

void test_decideGbnResendCap_math() {
    std::cout << "\n=== Pin 1: decideGbnResendCap math (pending vs cap) ==="
              << std::endl;
    // pending < cap → pending
    assert(decideGbnResendCap(1, 8) == 1);
    assert(decideGbnResendCap(5, 8) == 5);
    assert(decideGbnResendCap(7, 8) == 7);
    // pending == cap → cap
    assert(decideGbnResendCap(8, 8) == 8);
    // pending > cap → cap
    assert(decideGbnResendCap(9, 8) == 8);
    assert(decideGbnResendCap(20, 8) == 8);
    assert(decideGbnResendCap(32, 8) == 8);
    // cap == 0 → 0 (disable)
    assert(decideGbnResendCap(20, 0) == 0);
    // pending == 0 → 0
    assert(decideGbnResendCap(0, 8) == 0);
    // negative pending → 0 (defensive)
    assert(decideGbnResendCap(-3, 8) == 0);
    // negative cap → 0 (defensive)
    assert(decideGbnResendCap(20, -1) == 0);
    // cap > pending max
    assert(decideGbnResendCap(2, 100) == 2);
    std::cout << "  PASS (pending<cap, pending==cap, pending>cap, "
              << "cap==0, pending==0, negatives)" << std::endl;
}

void test_source_pin() {
    std::cout << "\n=== Pin 2: AutoLinkConfig / LinkTimers / LinkDecision "
                 "wire-up ==="
              << std::endl;
    std::string cfg_src = readFile(projectRoot() + "/src/al/AutoLinkConfig.h");
    std::string lt =
        (readFile(projectRoot() + "/src/al/link/timers/LinkTimersOk.cpp") +
         readFile(projectRoot() + "/src/al/link/timers/LinkTimersSwp.cpp") +
         readFile(projectRoot() + "/src/al/link/timers/LinkTimerBreak.cpp"));
    std::string ld =
        readFile(projectRoot() + "/src/al/link/sweep/LinkDecision.h");
    assert(!cfg_src.empty() && !lt.empty() && !ld.empty());

    // AutoLinkConfig exposes the field with the documented
    // default of 8.
    assert(cfg_src.find("gbnResendBurstMax") != std::string::npos &&
           "AutoLinkConfig.h must declare gbnResendBurstMax");
    assert(cfg_src.find("= 8;") != std::string::npos &&
           "AutoLinkConfig.h must default gbnResendBurstMax to 8");
    // The cap is consumed in gbnResendWindow_unlocked via
    // the pure helper.
    assert(lt.find("gbnResendBurstMax") != std::string::npos &&
           "LinkTimers.cpp must read cfg.gbnResendBurstMax in "
           "gbnResendWindow_unlocked");
    assert(lt.find("decideGbnResendCap") != std::string::npos &&
           "LinkTimers.cpp must call decideGbnResendCap in "
           "gbnResendWindow_unlocked");
    // The pure helper lives in LinkDecision.h.
    assert(ld.find("decideGbnResendCap") != std::string::npos &&
           "LinkDecision.h must define decideGbnResendCap");
    std::cout << "  PASS (field + default + helper wire-up all present)"
              << std::endl;
}

void seedPending(LinkTestAccessor &acc, int n) {
    // Plant n consecutive pending seqs starting at
    // gbnBase_=0. Each onSent stamps retxCount_=0 and
    // ackedPending_=true, so pendingCount()==n after
    // the loop. ARQ_CHUNK_BUDGET (64) covers n=20
    // with room to spare.
    for (int i = 0; i < n; i++) {
        acc.arq().onSent((uint8_t)i, 0xFF, 0);
    }
    acc.arq().setGbnBase(0);
    acc.arq().setGbnActive(true);
}

void test_burst_cap_clips_wide_window() {
    std::cout << "\n=== Pin 3: cap=8, pending=20 → 8 resends per RTO ==="
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
    cfg.gbnResendBurstMax = 8;

    MockHal hal;
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor acc(link);
    acc.forceState(State::OK);

    hal.lock();
    seedPending(acc, 20);
    int pendingBefore = acc.arq().pendingCount();
    int retxBefore = acc.arq().retxCountTotal();
    hal.unlock();
    assert(pendingBefore == 20);
    assert(retxBefore == 0);

    acc.gbnResendWindow(hal.now);

    hal.lock();
    int retxAfter = acc.arq().retxCountTotal();
    hal.unlock();

    // Each applyRetx(seq, now) bumps retxCount_ for exactly one
    // slot; the retxCountTotal delta is the burst width. Cap=8 on a
    // 20-pending window must bump exactly 8 slots.
    assert(retxAfter == 8 &&
           "20-pending scenario must resend exactly min(20, 8) = 8 frames "
           "per RTO under the cap");
    std::cout << "  PASS (pending=20, cap=8, post-call retxCountTotal="
              << retxAfter << ")" << std::endl;
}

void test_burst_cap_does_not_overclip_narrow_window() {
    std::cout << "\n=== Pin 4: cap=8, pending=5 → 5 resends (cap is a floor, "
                 "not a pad) ==="
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
    cfg.gbnResendBurstMax = 8;

    MockHal hal;
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor acc(link);
    acc.forceState(State::OK);

    hal.lock();
    seedPending(acc, 5);
    int pendingBefore = acc.arq().pendingCount();
    hal.unlock();
    assert(pendingBefore == 5);

    acc.gbnResendWindow(hal.now);

    hal.lock();
    int retxAfter = acc.arq().retxCountTotal();
    hal.unlock();

    // The cap is min(pending, cap): a 5-pending window resends all
    // 5 frames, not padded up to the cap's 8.
    assert(retxAfter == 5 &&
           "5-pending scenario must resend all 5 frames per RTO — the "
           "cap is a ceiling on what we resend, not a target to pad up to");
    std::cout << "  PASS (pending=5, cap=8, post-call retxCountTotal="
              << retxAfter << ")" << std::endl;
}

void test_cap_zero_disables_resend() {
    std::cout << "\n=== Pin 5: cap=0 short-circuits the resend (no frames) ==="
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
    cfg.gbnResendBurstMax = 0;

    MockHal hal;
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor acc(link);
    acc.forceState(State::OK);

    hal.lock();
    seedPending(acc, 12);
    hal.unlock();

    acc.gbnResendWindow(hal.now);

    hal.lock();
    int retxAfter = acc.arq().retxCountTotal();
    hal.unlock();

    assert(retxAfter == 0 &&
           "cap=0 must short-circuit the resend to no frames — the "
           "field's documented disable semantic");
    std::cout << "  PASS (pending=12, cap=0, post-call retxCountTotal="
              << retxAfter << ")" << std::endl;
}

} // namespace

int main() {
    std::cout << "=== GBN Whole-Window Burst Cap Regression Tests ==="
              << std::endl;
    test_decideGbnResendCap_math();
    test_source_pin();
    test_burst_cap_clips_wide_window();
    test_burst_cap_does_not_overclip_narrow_window();
    test_cap_zero_disables_resend();
    std::cout << "\nAll GBN burst-cap pins passed." << std::endl;
    return 0;
}
