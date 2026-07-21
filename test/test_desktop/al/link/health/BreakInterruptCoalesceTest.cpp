// Pinned regression: a single electrical glitch surfaces on the
// ESP32 UART driver as multiple BREAK / framing-error events at
// sub-ms spacing. The the prior release onBreak() path took the second
// sub-ms-spaced BREAK as "independent corroboration" of a real
// detach and fast-confirmed the reset — tearing down a healthy
// link. The fix coalesces BREAK interrupts within
// BREAK_COALESCE_MS into one event, so the second-BREAK
// fast-confirm path is not reachable from a single glitch.
//
//  Pin 1: two sub-ms-spaced BREAKs (within BREAK_COALESCE_MS)
//  must NOT reset the link. The link stays OK, breakSuspectMs_
//  stays armed, the confirm window is extended, and the
//  deadline still confirms if nothing arrives.
//  Pin 2: source-grep on the BREAK_COALESCE_MS coalesce gate
//  inside onBreak(). The shape that triggered the bug class
//  was "second BREAK confirms at once"; the pin rejects that
//  shape and asserts the coalesce gate is present.
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include <cstring>
#    include <cstdio>
#    include "MockHal.h"
#    include "LinkTestAccessor.h"
#    include "NullArqCache.h"
#    include "TestPaths.h"

using namespace autolink;

static const int kBauds[] = { 115200 };
static const int kNumBauds = (int)(sizeof(kBauds) / sizeof(kBauds[0]));

static void bringToOk(Link &a, Link &b, MockHal &mHal, MockHal &sHal) {
    a.begin();
    b.begin();
    for (int i = 0; i < 200; i++) {
        mHal.pumpClock(50);
        sHal.pumpClock(50);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
        if (a.getState() == State::OK && b.getState() == State::OK)
            return;
    }
    assert(false && "failed to bring two single-baud nodes to OK");
}

static AutoLinkConfig cfgCommon() {
    AutoLinkConfig cfg;
    for (int i = 0; i < kNumBauds; i++)
        cfg.allowedBauds[i] = (uint32_t)kBauds[i];
    cfg.allowedBaudsCount = kNumBauds;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 10000;
    cfg.syncAckTimeoutMs = 1000;
    cfg.maxRetx = 50;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    return cfg;
}

// Pin 1: two sub-ms-spaced BREAKs do NOT reset the link. This
// is the production bug class from run 2 of the field log:
// "BREAK log and reset_unlocked log are 1 ms apart at
// 05:40:54.327/.328 — not the intended 150 ms BREAK_CONFIRM_MS
// window".
static void test_two_subms_breaks_do_not_reset() {
    std::cout << "\n=== Pin 1: two sub-ms-spaced BREAKs do NOT reset the "
                 "link (single glitch, single electrical event) ==="
              << std::endl;
    NullArqCache cacheA, cacheB;
    AutoLinkConfig cfg = cfgCommon();
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link a(mHal, cacheA, /*isMaster=*/true, cfg);
    Link b(sHal, cacheB, /*isMaster=*/false, cfg);
    bringToOk(a, b, mHal, sHal);
    // Age past the post-lock wire-settle window (50 ms, an earlier release):
    // frames inside it are dropped silently, so this pin's
    // clearing frame could never land — red since the settle gate
    // landed (the timing predates the gate). Same fix as
    // BreakConfirmTest's agePastSettle.
    for (uint32_t ts = 0; ts < 120; ts += 20) {
        mHal.pumpClock(20);
        sHal.pumpClock(20);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
    }
    LinkTestAccessor accB(b);

    // First BREAK arms the confirm window.
    sHal.deliver_break_to_self();
    assert(b.getState() == State::OK &&
           "Pin 1 pre: first BREAK arms the confirm window");
    assert(accB.breakSuspectMsForTest() != 0 &&
           "Pin 1 pre: breakSuspectMs_ must be armed after first BREAK");
    uint32_t armedAt = accB.breakSuspectMsForTest();

    // Pump 5 ms — well under BREAK_COALESCE_MS (10 ms) but
    // enough to deliver the second BREAK interrupt as a
    // separate driver event. This is the the prior release production
    // shape: 1 ms between BREAK log and reset_unlocked log.
    for (uint32_t t = 0; t < 5; t += 1) {
        sHal.pumpClock(1);
    }
    sHal.deliver_break_to_self();

    // The link must still be OK — the second BREAK is the
    // same electrical event (coalesced), not an independent
    // corroboration.
    assert(
        b.getState() == State::OK &&
        "Pin 1: a second BREAK inside BREAK_COALESCE_MS must be "
        "coalesced (no fast-confirm). The the prior release field log showed "
        "two sub-ms-spaced BREAKs from one glitch tearing down a "
        "healthy link in 1 ms — this pin prevents the regression.");
    assert(accB.breakSuspectMsForTest() == armedAt &&
           "Pin 1: breakSuspectMs_ must NOT be cleared by the coalesced "
           "second BREAK — coalescing extends the observation window, "
           "not resets it");

    // Two valid frames inside the window suppress the
    // coalesced event. the current release item 4: two-frame-clears
    // contract. A's keepalive is 5s away, so two real app
    // messages get us two valid frames well inside 150ms.
    const uint8_t payload[] = { 'h', 'i' };
    bool ok = a.sendMsg(payload, sizeof payload);
    assert(ok);
    for (uint32_t t = 0; t < 50; t += 10) {
        mHal.pumpClock(10);
        sHal.pumpClock(10);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
    }
    // First frame is the half-clear.
    assert(accB.breakSuspectMsForTest() != 0 &&
           "Pin 1: the first late-tail frame is a half-clear, the "
           "confirm window must still be armed until a SECOND frame "
           "proves the peer is alive *now*");
    ok = a.sendMsg(payload, sizeof payload);
    assert(ok);
    for (uint32_t t = 0; t < 50; t += 10) {
        mHal.pumpClock(10);
        sHal.pumpClock(10);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
    }
    assert(b.getState() == State::OK &&
           "Pin 1: a valid frame inside the extended confirm window "
           "must clear the coalesced event — the link stays OK");
    assert(accB.breakSuspectMsForTest() == 0 &&
           "Pin 1: breakSuspectMs_ must be cleared by a valid frame "
           "after the coalesced event");
    assert(accB.breaksSuppressedForTest() == 1 &&
           "Pin 1: breaksSuppressedForTest() must count the single "
           "coalesced electrical event as one suppressed break, not two");
    std::cout << " Pin 1 PASS (coalesced event, valid frame cleared "
                 "it, breaksSuppressed=1)"
              << std::endl;
}

// Pin 2: source-grep on the BREAK_COALESCE_MS gate inside
// onBreak(). The shape that triggered the bug class was
// "second BREAK while one is already suspect confirms at once";
// the pin rejects that shape and asserts the coalesce gate is
// present in onBreak().
static void test_coalesce_gate_source_grep() {
    std::cout << "\n=== Pin 2: onBreak() has the BREAK_COALESCE_MS "
                 "coalesce gate (source-grep) ==="
              << std::endl;
    FILE *f = fopen(testRepoPath("src/al/link/timers/LinkTimerBreak.cpp").c_str(), "r");
    assert(f);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    assert(buf);
    size_t got = fread(buf, 1, (size_t)sz, f);
    buf[got] = 0;
    fclose(f);

    // Locate onBreak()'s body. The header is the only
    // place that calls hw.startTimer() with the confirm
    // deadline as the only arg — narrow the search to
    // that function and pin the coalesce gate.
    const char *fn = strstr(buf, "void Link::onBreak()");
    assert(fn);
    const char *body = strchr(fn, '{');
    assert(body);
    int depth = 0;
    const char *p = body;
    const char *end = nullptr;
    while (*p) {
        if (*p == '{')
            depth++;
        else if (*p == '}') {
            depth--;
            if (depth == 0) {
                end = p + 1;
                break;
            }
        }
        p++;
    }
    assert(end);
    int len = (int)(end - fn);
    char bodybuf[8192];
    if (len >= (int)sizeof(bodybuf))
        len = sizeof(bodybuf) - 1;
    memcpy(bodybuf, fn, len);
    bodybuf[len] = 0;

    // The coalesce gate must reference BREAK_COALESCE_MS and
    // the on-break-suspect state (breakSuspectMs_ != 0).
    // The shape with the gate is the only one that closes
    // the the prior release bug class.
    assert(strstr(bodybuf, "BREAK_COALESCE_MS") != NULL &&
           "Pin 2: onBreak() must reference BREAK_COALESCE_MS — the "
           "the prior release field log showed two sub-ms-spaced BREAKs from "
           "one glitch fast-confirming the reset, and the fix is "
           "to coalesce BREAKs within BREAK_COALESCE_MS into one "
           "event. The pin rejects the prior shape that had no "
           "coalesce gate.");
    assert(strstr(bodybuf, "breakSuspectMs_") != NULL &&
           "Pin 2: onBreak() must reference breakSuspectMs_ — the "
           "coalesce gate checks `breakSuspectMs_ != 0` (a prior "
           "suspicion is in flight) before extending the window");

    // The confirm deadline arm must still be inside onBreak():
    // a coalesced event re-arms
    // hw.startTimer(breakConfirmMs_unlocked()) so the deadline
    // still fires for the coalesced event. A future change
    // that drops the timer arm leaves the link with no timer
    // scheduled and the coalesced event never confirms.
    assert(
        strstr(bodybuf, "hw.startTimer((int)breakConfirmMs_unlocked(*this))") !=
            NULL &&
        "Pin 2: onBreak() must re-arm "
        "hw.startTimer(breakConfirmMs_unlocked()) for the "
        "coalesced event — without the re-arm, the "
        "previously-scheduled timer (e.g. the next idle-keepalive "
        "tick, seconds out under a large idleTimeoutMs) leaves the "
        "confirm check unevaluated for far longer than the "
        "confirm deadline");

    std::cout << " Pin 2 PASS (BREAK_COALESCE_MS gate present, "
                 "breakSuspectMs_ checked, timer re-armed)"
              << std::endl;
}

int main() {
    std::cout << "=== BREAK Interrupt Coalesce Tests ===" << std::endl;
    test_two_subms_breaks_do_not_reset();
    test_coalesce_gate_source_grep();
    std::cout << "\n=== All 2 BREAK interrupt-coalesce pins PASS ==="
              << std::endl;
    return 0;
}

#endif
