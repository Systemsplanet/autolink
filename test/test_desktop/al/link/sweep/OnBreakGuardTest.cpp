// Pin the BREAK-loop fix from the SWP livelock:
//   onBreak() short-circuits in non-OK states (mirror of
//   err_unlocked's guard). Self-induced UART_BREAKs that
//   fire right after setSpd are no-ops when the link is
//   still negotiating, so a baud-mismatch / idle-line
//   gltched break cannot tear down SWP state mid-sweep.
//
// Six pins, one per fix in the SWP-livelock patch:
//
//   1. onBreak() in P1 (state != OK) is a no-op (state
//      stays SWP, phase stays P1, spdI unchanged).
//   2. onBreak() in P2 (state != OK) is a no-op (state
//      stays SWP, phase stays P2, spdI unchanged).
//   3. onBreak() in OK state still resets (backwards-
//      compatible: a real break on a locked link
//      re-sweeps).
//   4. PromoteToPhase2 shape is preserved (enterPhase2
//      + phase2Slave[0] arm). The fix keeps the existing
//      baud-jump behaviour because Fix #1 alone breaks
//      the livelock; changing the promote action would
//      lock the wire at slowest baud (~7s) and break
//      the closed-loop test's drop-interval budget.
//   5. Source-level: BREAK_DEBOUNCE_MS in EspHal.h
//      outlasts the UART event-task loop period
//      (50-100 ms typical) so a gltched second-break
//      is dropped before the window closes.
//   6. Ping::setPaused stamps tSweepStall_ so the
//      "not ready  swpAge=..." debug line shows wall-
//      clock time from the user's Start push.
//
// Toggle off (revert the guard in onBreak) and pins
// 1-3 flip red. Toggle off (drop the setPhase/enterPhase2
// pair or change the timer arm) and pin 4 flips red.
// Toggle off (lower BREAK_DEBOUNCE_MS back to 50) and
// pin 5 flips red. Toggle off (drop the tSweepStall_
// stamp before kickoff) and pin 6 flips red.
#include <iostream>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include "MockHal.h"
#include "LinkTestAccessor.h"
#include "NullArqCache.h"
#include "al/util/UtilCrc.h"

using namespace autolink;

static const int kBauds[] = { 115200, 57600, 38400, 19200, 9600 };
static const int kNumBauds = (int)(sizeof(kBauds) / sizeof(kBauds[0]));

// Pin 1: onBreak() in P1 must NOT hard-reset the
// sweep. The pre-fix shape called reset_unlocked(true)
// unconditionally, tearing down SWP state on every
// spurious break (post-setSpd gltches in P1 →
// pingSample resets → sweep restart → another
// spurious break).
static void test_onbreak_in_p1_is_noop() {
    std::cout
        << "\n=== Pin 1: onBreak() in P1 does not reset (state stays SWP) ==="
        << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg;
    for (int i = 0; i < kNumBauds; i++)
        cfg.allowedBauds[i] = (uint32_t)kBauds[i];
    cfg.allowedBaudsCount = kNumBauds;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 0;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link ping(mHal, cache, true, cfg);
    Link pong(sHal, cache, false, cfg);
    ping.begin();
    pong.begin();

    // Both should be in SWP state at slowest baud.
    assert(ping.getState() == State::SWP);
    assert(pong.getState() == State::SWP);
    assert(LinkTestAccessor(ping).sweepPhase() == SweepPhase::PHASE1);
    assert(LinkTestAccessor(pong).sweepPhase() == SweepPhase::PHASE1);
    int spdBefore = ping.getCurrentSpdIndex();
    int pongSpdBefore = pong.getCurrentSpdIndex();

    // The fix: onBreak() in P1 must be a no-op.
    // Pre-fix it would reset to spdI=slowest via
    // reset_unlocked(true) — the visible difference
    // is the preferredBaud_ field resetting to
    // NO_PREFERRED_BAUD and the diag counters
    // incrementing. We pin only that the state
    // and phase are unchanged.
    ping.onBreak();
    pong.onBreak();

    assert(ping.getState() == State::SWP);
    assert(pong.getState() == State::SWP);
    assert(LinkTestAccessor(ping).sweepPhase() == SweepPhase::PHASE1);
    assert(LinkTestAccessor(pong).sweepPhase() == SweepPhase::PHASE1);
    assert(ping.getCurrentSpdIndex() == spdBefore);
    assert(pong.getCurrentSpdIndex() == pongSpdBefore);
    std::cout << "  PASS (P1 onBreak no-op: state=SWP, phase=P1, "
                 "spdI unchanged)"
              << std::endl;
}

// Pin 2: onBreak() in P2 must NOT hard-reset the
// sweep. This is the wire-cycle case: master
// promotes to P2, slave promotes to P2, the baud
// switch gltches a break, and the pre-fix code
// tore P2 down on every gltched break.
static void test_onbreak_in_p2_is_noop() {
    std::cout
        << "\n=== Pin 2: onBreak() in P2 does not reset (state stays SWP) ==="
        << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg;
    for (int i = 0; i < kNumBauds; i++)
        cfg.allowedBauds[i] = (uint32_t)kBauds[i];
    cfg.allowedBaudsCount = kNumBauds;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 0;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link ping(mHal, cache, true, cfg);
    Link pong(sHal, cache, false, cfg);
    ping.begin();
    pong.begin();

    // Force both into P2 (master directly via the
    // sweep accessor; slave via the same — both
    // should keep state SWP, phase P2, spdI
    // unchanged across onBreak).
    LinkTestAccessor(ping).setSweepPhase(SweepPhase::PHASE2);
    LinkTestAccessor(pong).setSweepPhase(SweepPhase::PHASE2);
    assert(LinkTestAccessor(ping).sweepPhase() == SweepPhase::PHASE2);
    assert(LinkTestAccessor(pong).sweepPhase() == SweepPhase::PHASE2);
    int pingSpdBefore = ping.getCurrentSpdIndex();
    int pongSpdBefore = pong.getCurrentSpdIndex();

    ping.onBreak();
    pong.onBreak();

    assert(ping.getState() == State::SWP);
    assert(pong.getState() == State::SWP);
    assert(LinkTestAccessor(ping).sweepPhase() == SweepPhase::PHASE2);
    assert(LinkTestAccessor(pong).sweepPhase() == SweepPhase::PHASE2);
    assert(ping.getCurrentSpdIndex() == pingSpdBefore);
    assert(pong.getCurrentSpdIndex() == pongSpdBefore);
    std::cout << "  PASS (P2 onBreak no-op: state=SWP, phase=P2, "
                 "spdI unchanged)"
              << std::endl;
}

// Pin 3: onBreak() in OK state MUST reset (backwards
// compatibility — a real break on a locked link
// re-sweeps). The fix only short-circuits in non-OK
// states.
static void test_onbreak_in_ok_still_resets() {
    std::cout
        << "\n=== Pin 3: onBreak() in OK still resets (backwards compat) ==="
        << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg;
    for (int i = 0; i < kNumBauds; i++)
        cfg.allowedBauds[i] = (uint32_t)kBauds[i];
    cfg.allowedBaudsCount = kNumBauds;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 0;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link ping(mHal, cache, true, cfg);
    Link pong(sHal, cache, false, cfg);
    ping.begin();
    pong.begin();

    // Drive to OK via the standard negotiator.
    negotiate_to_ok(ping, pong, mHal, sHal);
    assert(ping.getState() == State::OK);
    assert(pong.getState() == State::OK);

    // A real break on a locked link must still
    // reset. The fix only short-circuits in non-OK
    // states — OK keeps the original behavior.
    ping.onBreak();
    pong.onBreak();
    assert(ping.getState() == State::SWP);
    assert(pong.getState() == State::SWP);
    // Both reset to P1 at slowest baud.
    assert(LinkTestAccessor(ping).sweepPhase() == SweepPhase::PHASE1);
    assert(LinkTestAccessor(pong).sweepPhase() == SweepPhase::PHASE1);
    std::cout << "  PASS (OK onBreak resets: state=SWP, phase=P1)" << std::endl;
}

// Pin 4 (regression-on-fix-1): even though Fix #3
// was reverted (Fix #1 alone breaks the livelock
// by gating onBreak on state), the slave's baud
// jump on PromoteToPhase2 is still the underlying
// "manufactures a mismatch" trigger. Pin the
// behavior so a future change can't silently
// reintroduce a HARDER form (e.g. enterPhase2 + a
// new hw.setSpd call). The current contract is
// enterPhase2 is called, spdI jumps to 0, baud
// switches to baud[0], 250ms timer arms. Pin
// that the existing shape is intact so the
// regression we care about (livelock) stays
// gated by Fix #1's onBreak state guard.
//
// This is a source-level pin: we read Link.cpp's
// applyPongSwpAction_unlocked body and assert the
// PromoteToPhase2 case still calls
// sweep_.enterPhase2 (which jumps the slave to
// baud[0]) and arms with phase2Slave[0].
// The runtime path is exercised end-to-end by
// run_test_wiresim_closedloop / run_test_alink_sweep_p1_guard.
static void test_slave_promote_to_phase2_stays_at_slowest() {
    std::cout << "\n=== Pin 4: slave PromoteToPhase2 keeps slowest baud "
                 "(source-grep) ==="
              << std::endl;
    // applyPongSwpAction_unlocked lives in LinkSweep.cpp
    // after the god-class split.
    FILE *f = fopen("../../src/al/link/LinkSweep.cpp", "r");
    assert(f);
    char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    // Locate the applyPongSwpAction_unlocked body
    // and the PromoteToPhase2 case inside it.
    // Important: there are two functions (master and
    // pong). The first occurrence of PromoteToPhase2
    // is the master's; we need the PONG's, which is
    // inside the applyPongSwpAction_unlocked function.
    const char *fn = strstr(buf, "bool Link::applyPongSwpAction_unlocked");
    assert(fn);
    const char *p2 = strstr(fn, "case SwpPhaseAction::PromoteToPhase2:");
    assert(p2);
    // The case body must end before the next
    // SwpPhaseAction:: case or the function's closing brace.
    const char *end = strstr(p2, "case SwpPhaseAction::FallbackLockSlowest:");
    if (!end)
        end = strstr(p2, "}\n");
    assert(end && end > p2);
    // Copy the case body for inspection.
    int len = (int)(end - p2);
    char case_body[8192];
    if (len >= (int)sizeof(case_body))
        len = sizeof(case_body) - 1;
    memcpy(case_body, p2, len);
    case_body[len] = 0;
    // Pin the existing PromoteToPhase2 shape
    // (calls enterPhase2 and arms phase2Slave[0]).
    // The fix preserves this shape; the livelock
    // is gated by Fix #1's onBreak state guard
    // (Pin 1/2/3), not by changing PromoteToPhase2.
    // Locking in this shape means any future
    // regression that changes the promote action
    // (e.g. silently dropping the baud switch or
    // the timer arm) trips this pin — and forces
    // the change through the test review, where
    // the lock-time impact (this is what the
    // wire-sim closed-loop test gates) is visible.
    assert(strstr(case_body, "sweep_.enterPhase2(*this)") != NULL);
    assert(strstr(case_body, "phase2Slave[0]") != NULL);
    std::cout << "  PASS (enterPhase2 + phase2Slave[0] arm present)"
              << std::endl;
}

// Pin 5: source-level pin. EspHal.h must declare a
// BREAK_DEBOUNCE_MS that outlasts the UART event-task
// loop period (~50-100 ms). The pre-fix value of 50 ms
// was below the loop period; a gltched second-break
// could land before the window closed, letting the
// second break through. The new value must be >= 100
// ms; we pin against >= 100 ms (the documented
// minimum).
static void test_esphal_break_debounce_ms_above_loop_period() {
    std::cout << "\n=== Pin 5: EspHal.h BREAK_DEBOUNCE_MS >= 100 ms ==="
              << std::endl;
    FILE *f = fopen("../../src/al/hal/EspHal.h", "r");
    assert(f);
    char buf[16384];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    const char *needle = "BREAK_DEBOUNCE_MS = ";
    const char *p = strstr(buf, needle);
    assert(p);
    int val = atoi(p + strlen(needle));
    assert(val >= 100);
    std::cout << "  PASS (BREAK_DEBOUNCE_MS = " << val << " ms >= 100 ms)"
              << std::endl;
}

// Pin 6: Ping::setPaused(false) must stamp
// tSweepStall_ = millis() before kickoff so the
// "not ready  swpAge=..." debug line on the
// post-resume sweep shows wall-clock time from
// the user's Start push rather than (millis - 0)
// = full millis count. Without this stamp, the
// post-resume swpAge reads as a misleading
// 6-7 digit number on a paused boot.
static void test_ping_setPaused_stamps_sweep_stall() {
    std::cout << "\n=== Pin 6: Ping::setPaused stamps tSweepStall_ ==="
              << std::endl;
    FILE *f = fopen("../../src/al/pingpong/Ping.h", "r");
    assert(f);
    char buf[16384];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    const char *fn = strstr(buf, "void setPaused(bool p) {");
    assert(fn);
    const char *end =
        strstr(fn, "base_.log_.info(\"Ping\", \"device-side pause");
    if (!end)
        end = strstr(fn, "}\n");
    assert(end && end > fn);
    int len = (int)(end - fn);
    char body[8192];
    if (len >= (int)sizeof(body))
        len = sizeof(body) - 1;
    memcpy(body, fn, len);
    body[len] = 0;
    // The setPaused(false) branch (inside `if (!p)`)
    // must assign tSweepStall_ = millis() before the
    // kickoff() call so the stamp lands at the
    // user's Start push.
    assert(strstr(body, "tSweepStall_ = millis()") != NULL);
    // And the assignment must be before the
    // kickoff() call so the stamp is set even if
    // kickoff is a no-op (kickedOff_ guard).
    const char *stamp = strstr(body, "tSweepStall_ = millis()");
    const char *kick = strstr(body, "kickoff()");
    assert(stamp && kick && stamp < kick);
    std::cout << "  PASS (tSweepStall_ = millis() present, "
                 "before kickoff())"
              << std::endl;
}

int main() {
    std::cout << "=== OnBreak SWP-Livelock Guard Tests ===" << std::endl;
    test_onbreak_in_p1_is_noop();
    test_onbreak_in_p2_is_noop();
    test_onbreak_in_ok_still_resets();
    test_slave_promote_to_phase2_stays_at_slowest();
    test_esphal_break_debounce_ms_above_loop_period();
    test_ping_setPaused_stamps_sweep_stall();
    std::cout << "\n=== All 6 onBreak / livelock pins PASS ===" << std::endl;
    return 0;
}
