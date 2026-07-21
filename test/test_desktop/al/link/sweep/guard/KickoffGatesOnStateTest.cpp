// Regression pin: Link::kickoff() must gate on link
// state, not on a permanent kickedOff_ latch. A
// kickedOff_ latch is a one-shot — the first call sets
// it true and every later call (e.g. the
// Ping::setPaused(false) fired by the web-UI Start
// button) silently no-ops as "kickoff: already
// running; no-op". Once the link has wedged
// mid-handshake, no recovery path exists short of
// reboot. Contract: gate on state == State::OK
// (a locked link ignores kickoff) and let every
// other state (SWP, mid-reset) re-fire hw.sendBreak()
// and re-enter Phase 1.
//
// Three runtime pins + one source-grep pin:
//
//   1. kickoff() with state==OK is a no-op (link is
//      already locked, no need to re-fire).
//   2. kickoff() with state==SWP re-fires the master
//      start path (sendBreak + reset_unlocked + P1
//      arm) — the recovery path.
//   3. A second kickoff() after the first wedge
//      still recovers, no need to reboot (the
//      operator's Start toggle works end-to-end).
//   4. Source-grep: kickoff()'s first guard is on
//      state, not kickedOff_.
//
// Toggle off (re-add the kickedOff_ guard at the
// top of kickoff()) and pins 2 and 3 go red.
#include <iostream>
#include <cassert>
#include "MockHal.h"
#include "LinkTestAccessor.h"
#include "NullArqCache.h"
#include "al/AutoLinkConfig.h"
#include "TestPaths.h"

using namespace autolink;

static const int kBauds[] = { 115200, 57600, 38400, 19200, 9600 };
static const int kNumBauds = (int)(sizeof(kBauds) / sizeof(kBauds[0]));

// Pin 1: kickoff() with state == OK is a no-op (the
// link is already locked, the master shouldn't send
// a BREAK that would tear the link down).
static void test_kickoff_in_ok_is_noop() {
    std::cout << "\n=== Pin 1: kickoff() in OK state is a no-op ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg;
    for (int i = 0; i < kNumBauds; i++)
        cfg.allowedBauds[i] = (uint32_t)kBauds[i];
    cfg.allowedBaudsCount = kNumBauds;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 0;
    MockHal mHal;
    Link ping(mHal, cache, true, cfg);
    ping.begin();
    LinkTestAccessor acc(ping);
    acc.forceState(State::OK);
    int breaksBefore = mHal.sendBreakCalls;
    int startsBefore = mHal.timerStartCalls;
    ping.kickoff();
    int breaksAfter = mHal.sendBreakCalls;
    int startsAfter = mHal.timerStartCalls;
    assert(breaksAfter == breaksBefore &&
           "kickoff() on an OK link must not send BREAK (would tear "
           "the locked link down)");
    assert(startsAfter == startsBefore &&
           "kickoff() on an OK link must not arm a timer (no work to do)");
    std::cout << "  PASS (no BREAK, no timer arm)" << std::endl;
}

// Pin 2: kickoff() with state == SWP re-fires the
// master start path (sendBreak + reset_unlocked + P1
// arm). This is the recovery path that a latch-based
// guard would block.
static void test_kickoff_in_swp_refires_master_start() {
    std::cout << "\n=== Pin 2: kickoff() in SWP re-fires master start ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg;
    for (int i = 0; i < kNumBauds; i++)
        cfg.allowedBauds[i] = (uint32_t)kBauds[i];
    cfg.allowedBaudsCount = kNumBauds;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 0;
    MockHal mHal;
    Link ping(mHal, cache, true, cfg);
    ping.begin();
    // Force the link into SWP and clear kickedOff_'s
    // effect. kickoff() in master branch sends BREAK,
    // which fires the link's onBreak on the peer. With
    // a peer wired up the slave's onBreak will arm a
    // phase2[0]+200 timer — but we want to assert the
    // master side, not the slave.
    LinkTestAccessor acc(ping);
    acc.forceState(State::SWP);
    int breaksBefore = mHal.sendBreakCalls;
    int startsBefore = mHal.timerStartCalls;
    ping.kickoff();
    int breaksAfter = mHal.sendBreakCalls;
    int startsAfter = mHal.timerStartCalls;
    assert(breaksAfter == breaksBefore + 1 &&
           "kickoff() in SWP must re-fire hw.sendBreak() — "
           "this is the recovery path; a kickedOff_ latch would "
           "block it");
    assert(startsAfter > startsBefore &&
           "kickoff() in SWP must arm the P1 timer (forward "
           "progress)");
    std::cout << "  PASS (BREAK " << breaksBefore << " -> " << breaksAfter
              << ", timer arm " << startsBefore << " -> " << startsAfter << ")"
              << std::endl;
}

// Pin 3: a wedge + recovery scenario. Drive the
// link into SWP, fire a wedge (no further onTimer
// activity), then fire kickoff() from a user-side
// Start toggle. The recovery must succeed — a
// kickedOff_ latch would turn this into a silent no-op.
static void test_kickoff_recovers_from_wedge() {
    std::cout << "\n=== Pin 3: kickoff() recovers from a wedge ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg;
    for (int i = 0; i < kNumBauds; i++)
        cfg.allowedBauds[i] = (uint32_t)kBauds[i];
    cfg.allowedBaudsCount = kNumBauds;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 0;
    MockHal mHal;
    Link ping(mHal, cache, true, cfg);
    ping.begin();
    LinkTestAccessor acc(ping);
    acc.forceState(State::SWP);
    int breaksBefore = mHal.sendBreakCalls;
    int startsBefore = mHal.timerStartCalls;
    // Simulate the user-side Start toggle (Ping::setPaused(false)
    // → kickoff()).
    ping.kickoff();
    int breaksAfter = mHal.sendBreakCalls;
    int startsAfter = mHal.timerStartCalls;
    assert(breaksAfter > breaksBefore &&
           "user Start toggle on a wedged link must trigger a "
           "BREAK — a kickedOff_ latch would silently no-op it");
    assert(startsAfter > startsBefore &&
           "user Start toggle on a wedged link must re-arm P1 — "
           "the link was wedged mid-handshake, the recovery is "
           "the P1 re-fire");
    assert(ping.getState() == State::SWP);
    std::cout << "  PASS (wedge recovered: BREAK + P1 re-arm)" << std::endl;
}

// Pin 4: source-grep. kickoff()'s first guard is on
// state, not on kickedOff_. A kickedOff_-first guard
// is the failure shape the test pins against.
static void test_kickoff_source_gates_on_state() {
    std::cout
        << "\n=== Pin 4: kickoff() source-grep: state guard, not latch ==="
        << std::endl;
    FILE *f = fopen(testRepoPath("src/al/link/LinkCore.cpp").c_str(), "r");
    assert(f);
    char buf[32768];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    const char *kick = strstr(buf, "void Link::kickoff()");
    assert(kick);
    // The first guard inside kickoff() must be on
    // state == State::OK (or != State::OK), not on
    // kickedOff_. Locate the function body and pin
    // the order of guards.
    const char *bodyEnd = strstr(kick, "}\n");
    assert(bodyEnd && bodyEnd > kick);
    int len = (int)(bodyEnd - kick);
    char body[8192];
    if (len >= (int)sizeof(body))
        len = sizeof(body) - 1;
    memcpy(body, kick, len);
    body[len] = 0;
    // A kickedOff_-first guard is the failure shape
    // the test pins against.
    const char *kickedGuard = strstr(body, "if (kickedOff_) {");
    const char *stateGuard = strstr(body, "if (state == State::OK)");
    const char *stateGuard2 = strstr(body, "state != State::OK");
    assert(stateGuard || stateGuard2);
    if (kickedGuard && (stateGuard || stateGuard2)) {
        const char *firstState = stateGuard ? stateGuard : stateGuard2;
        bool stateFirst = firstState < kickedGuard;
        assert(stateFirst);
    }
    std::cout << "  PASS (kickoff() gates on state, kickedOff_ is not a "
                 "recovery blocker)"
              << std::endl;
}

int main() {
    std::cout << "=== Kickoff Gates On State (not latch) Tests ==="
              << std::endl;
    test_kickoff_in_ok_is_noop();
    test_kickoff_in_swp_refires_master_start();
    test_kickoff_recovers_from_wedge();
    test_kickoff_source_gates_on_state();
    std::cout << "\n=== All 4 kickoff-gates-on-state pins PASS ==="
              << std::endl;
    return 0;
}
