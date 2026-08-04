// Pinned regression: a UART_BREAK event while OK is reported by
// the same ESP32 driver path for a genuine peer detach AND for
// a framing glitch — observed in the field under sustained
// 512000-baud ASYNC traffic with large messages. The fix
// debounces every OK-state BREAK with a short confirm window
// (NOT gated on pendingCount > 0: a burst that just drained to
// zero pending is exactly the field-log shape, and gating on
// pending silently skipped the debounce so a single UART glitch
// fired the reset path). The confirm window is cleared by a
// CRC-valid frame within BREAK_GRACE_MS of arming suspicion.
//
// BREAKs arriving within BREAK_COALESCE_MS of one another are
// treated as a single electrical event — a single glitch
// surfaces as multiple BREAK / framing-error interrupts at sub-ms
// spacing on the ESP32 UART driver, and the second-BREAK
// fast-confirm path must not be reachable from one glitch.
//
//   Pin 1: busy link, BREAK + a valid frame inside the window
//   clears suspicion, no reset, breaksSuppressedForTest() == 1.
//   Pin 2: busy link, BREAK + silence past the window confirms
//   the reset (reason=HealthWatchdog).
//   Pin 3: two BREAKs in a row outside the coalesce window
//   confirm immediately, no window wait.
//   Pin 4: idle link (no pending ARQ data) BREAK also debounces
//   — the prior `pendingCount > 0` gate is gone (it was the bug
//   class the field log surfaced). An idle-link BREAK is suppressed
//   by a valid frame inside the window OR confirms via the
//   deadline when no frame arrives. The OkKeepaliveTest contract
//   (no BREAK on a healthy idle link) is unchanged — the
//   keepalive emits a PING every idleTimeoutMs/2 that refreshes
//   the peer's lastValidRxMs and clears any pending suspicion.
//   Pin 5: two BREAKs within BREAK_COALESCE_MS (sub-ms-spaced
//   driver noise from a single electrical glitch) are coalesced
//   into one event, NOT two fast-confirming BREAKs. The
//   link must NOT reset on the second sub-ms-spaced BREAK
//   alone — that's the the prior release field-log failure mode this
//   pin exists to prevent.
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include "MockHal.h"
#    include "LinkTestAccessor.h"
#    include "NullArqCache.h"

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

static AutoLinkConfig busyCfg() {
    AutoLinkConfig cfg;
    for (int i = 0; i < kNumBauds; i++)
        cfg.allowedBauds[i] = (uint32_t)kBauds[i];
    cfg.allowedBaudsCount = kNumBauds;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 10000; // large: keepalive must not interfere
    cfg.syncAckTimeoutMs = 1000;
    cfg.maxRetx = 50;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    return cfg;
}

// Pin 1: busy link, break clears via a valid frame inside the window.
// Age past the post-lock wire-settle window
// (AUTOLINK_WIRE_SETTLE_MS = 50, added with the settle gate): a payload
// arriving inside it is dropped silently — no NAK, no noteValidFrameOk — so a
// clearing frame sent right after lock could never land within these
// pins' 100 ms budgets (the retx comes at RTO = 500 ms). Pins 1 and 4
// had been red since the settle gate landed; the PRODUCT contract (a
// valid frame clears BREAK suspicion) is intact once past settle, as
// the fieldsoak's 19 debounced BREAKs per run demonstrate.
static void agePastSettle(MockHal &mHal, MockHal &sHal) {
    for (uint32_t ts = 0; ts < 120; ts += 20) {
        mHal.pumpClock(20);
        sHal.pumpClock(20);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
    }
}

static void test_busy_break_clears_on_valid_frame() {
    std::cout << "\n=== Pin 1: busy-link BREAK clears via a valid frame "
                 "inside the confirm window ==="
              << std::endl;
    NullArqCache cacheA, cacheB;
    AutoLinkConfig cfg = busyCfg();
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link a(mHal, cacheA, /*isMaster=*/true, cfg);
    Link b(sHal, cacheB, /*isMaster=*/false, cfg);
    bringToOk(a, b, mHal, sHal);
    agePastSettle(mHal, sHal);

    // Make B "busy": plant a pending ArqCache slot directly so the
    // busy gate reads true without needing a real large transfer.
    LinkTestAccessor(b).markAckedPending(0);
    assert(LinkTestAccessor(b).arqPendingCountForTest() > 0 &&
           "precondition: B must be busy (pendingCount > 0)");

    sHal.deliver_break_to_self();
    assert(LinkTestAccessor(b).breakSuspectMsForTest() != 0 &&
           "Pin 1: a busy-link BREAK must arm suspicion, not reset "
           "immediately");
    assert(b.getState() == State::OK &&
           "Pin 1: B must still be OK right after the first BREAK — the "
           "reset is deferred to the confirm window");

    // Two valid frames within the window: A's keepalive
    // is 5s away, so send two real app messages to
    // produce two well inside 150ms. The two-frame-
    // clears contract (the current release item 4) requires the
    // first late-tail frame to be a half-clear (sees
    // the dying session's data) and the SECOND to be
    // a full clear (proves the peer is alive *now*).
    const uint8_t payload[] = { 'h', 'i' };
    bool ok = a.sendMsg(payload, sizeof payload);
    assert(ok);
    for (uint32_t t = 0; t < 30; t += 5) {
        mHal.pumpClock(5);
        sHal.pumpClock(5);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
    }
    // Half-clear: breakSuspectMs_ is still set, the
    // seen counter advanced.
    assert(LinkTestAccessor(b).breakSuspectMsForTest() != 0 &&
           "Pin 1: the first late-tail frame is a half-clear, not a full "
           "clear — the BREAK confirm window must still be armed until "
           "a SECOND frame proves the peer is alive *now*");
    ok = a.sendMsg(payload, sizeof payload);
    assert(ok);
    for (uint32_t t = 0; t < 30; t += 5) {
        mHal.pumpClock(5);
        sHal.pumpClock(5);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
    }
    assert(
        LinkTestAccessor(b).breakSuspectMsForTest() == 0 &&
        "Pin 1: two valid frames inside the confirm window must "
        "clear breakSuspectMs_ (the current release item 4: two-frame-clears)");
    assert(LinkTestAccessor(b).breaksSuppressedForTest() == 1 &&
           "Pin 1: breaksSuppressedForTest() must count the suppressed "
           "break");
    assert(b.getState() == State::OK &&
           "Pin 1: B must never have reset — the break was spurious and "
           "the valid frame proved it");
    std::cout << "  Pin 1 PASS (suspicion cleared by a valid frame, no "
                 "reset, breaksSuppressed=1)"
              << std::endl;
}

// Pin 2: busy link, break confirms via the deadline when nothing
// clears it.
static void test_busy_break_confirms_on_deadline() {
    std::cout << "\n=== Pin 2: busy-link BREAK confirms via the deadline when "
                 "no valid frame arrives ==="
              << std::endl;
    NullArqCache cacheA, cacheB;
    AutoLinkConfig cfg = busyCfg();
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link a(mHal, cacheA, /*isMaster=*/true, cfg);
    Link b(sHal, cacheB, /*isMaster=*/false, cfg);
    bringToOk(a, b, mHal, sHal);
    agePastSettle(mHal, sHal);
    LinkTestAccessor(b).markAckedPending(0);
    assert(LinkTestAccessor(b).arqPendingCountForTest() > 0);

    sHal.deliver_break_to_self();
    assert(b.getState() == State::OK &&
           "precondition: deferred, not yet reset");

    // Pump B's own timer past BREAK_CONFIRM_MS with no traffic at
    // all — nothing to clear the suspicion.
    bool confirmed = false;
    for (int i = 0; i < 20; i++) {
        sHal.pumpClock(20);
        b.onTimer();
        if (b.getState() == State::SWP) {
            confirmed = true;
            break;
        }
    }
    assert(confirmed &&
           "Pin 2: the confirm deadline must eventually reset B when "
           "nothing clears the suspicion — otherwise a genuinely dropped "
           "peer is never detected");
    assert(LinkTestAccessor(b).lastResetReasonForTest() ==
               ResetReason::HealthWatchdog &&
           "Pin 2: the deadline-confirmed reset must use HealthWatchdog, "
           "matching every other BREAK-driven reset");
    assert(LinkTestAccessor(b).breakSuspectMsForTest() == 0 &&
           "Pin 2: breakSuspectMs_ must be cleared once confirmed");
    std::cout << "  Pin 2 PASS (deadline confirmed the reset, "
                 "reason=HealthWatchdog)"
              << std::endl;
}

// Pin 3: two BREAKs in a row OUTSIDE the coalesce window confirm
// immediately (no window wait).
static void test_double_break_outside_coalesce_confirms_immediately() {
    std::cout << "\n=== Pin 3: double BREAK outside BREAK_COALESCE_MS "
                 "confirms immediately (no window wait) ==="
              << std::endl;
    NullArqCache cacheA, cacheB;
    AutoLinkConfig cfg = busyCfg();
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link a(mHal, cacheA, /*isMaster=*/true, cfg);
    Link b(sHal, cacheB, /*isMaster=*/false, cfg);
    bringToOk(a, b, mHal, sHal);
    agePastSettle(mHal, sHal);
    LinkTestAccessor(b).markAckedPending(0);

    sHal.deliver_break_to_self();
    assert(b.getState() == State::OK);
    // Pump past the coalesce window (10 ms) before
    // the second BREAK so the second event is
    // treated as a separate, independent
    // electrical event — the fast-confirm path
    // is exactly the right behavior for two
    // independent BREAKs from a real detach.
    for (uint32_t t = 0; t < 30; t += 5) {
        sHal.pumpClock(5);
    }
    sHal.deliver_break_to_self();
    assert(b.getState() == State::SWP &&
           "Pin 3: a second BREAK outside the coalesce window while one "
           "is already suspect must confirm immediately, not wait out "
           "the window");
    assert(LinkTestAccessor(b).lastResetReasonForTest() ==
           ResetReason::HealthWatchdog);
    std::cout << "  Pin 3 PASS (second BREAK confirmed immediately)"
              << std::endl;
}

// Pin 4: idle link (no pending ARQ data) BREAK also debounces —
// the prior `pendingCount > 0` gate is gone. A valid frame
// inside the window suppresses the break, otherwise the deadline
// confirms. (Contract change vs the the prior release test: the prior
// `pendingCount > 0` gate was the bug class the field log
// surfaced, so the test must reflect the new contract.)
static void test_idle_break_also_debounces() {
    std::cout << "\n=== Pin 4: idle-link BREAK also debounces (no "
                 "pendingCount>0 gate) ==="
              << std::endl;
    NullArqCache cacheA, cacheB;
    AutoLinkConfig cfg = busyCfg();
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link a(mHal, cacheA, /*isMaster=*/true, cfg);
    Link b(sHal, cacheB, /*isMaster=*/false, cfg);
    bringToOk(a, b, mHal, sHal);
    agePastSettle(mHal, sHal);
    assert(LinkTestAccessor(b).arqPendingCountForTest() == 0 &&
           "precondition: B has no pending data (idle)");

    sHal.deliver_break_to_self();
    assert(b.getState() == State::OK &&
           "Pin 4: an idle-link BREAK must still defer to the confirm "
           "window — the prior `pendingCount > 0` gate was the bug class "
           "the field log surfaced (a just-drained burst with pending=0 "
           "silently skipped the debounce and reset on a single glitch)");
    assert(LinkTestAccessor(b).breakSuspectMsForTest() != 0 &&
           "Pin 4: breakSuspectMs_ must be armed for an idle-link BREAK "
           "(no pendingCount gate around the confirm window anymore)");

    // Two valid frames within the window suppress the
    // idle-link break. the current release item 4: two-frame-clears
    // contract. A's keepalive is 5s away, so two real app
    // messages get us two valid frames well inside 150ms.
    const uint8_t payload[] = { 'h', 'i' };
    bool ok = a.sendMsg(payload, sizeof payload);
    assert(ok);
    for (uint32_t t = 0; t < 30; t += 5) {
        mHal.pumpClock(5);
        sHal.pumpClock(5);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
    }
    // First frame is the half-clear; the suspect is still
    // armed.
    assert(LinkTestAccessor(b).breakSuspectMsForTest() != 0 &&
           "Pin 4: the first late-tail frame is a half-clear, the "
           "confirm window must still be armed until a SECOND frame "
           "proves the peer is alive *now*");
    ok = a.sendMsg(payload, sizeof payload);
    assert(ok);
    for (uint32_t t = 0; t < 30; t += 5) {
        mHal.pumpClock(5);
        sHal.pumpClock(5);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
    }
    assert(LinkTestAccessor(b).breakSuspectMsForTest() == 0 &&
           "Pin 4: two valid frames inside the confirm window must "
           "clear the idle-link break suspicion");
    assert(LinkTestAccessor(b).breaksSuppressedForTest() == 1 &&
           "Pin 4: breaksSuppressedForTest() must count the suppressed "
           "idle-link break");
    assert(b.getState() == State::OK &&
           "Pin 4: B must never have reset — the idle-link break was "
           "spurious and a valid frame proved it");
    std::cout << "  Pin 4 PASS (idle-link BREAK debounced, valid frame "
                 "cleared it, breaksSuppressed=1)"
              << std::endl;
}

// Pin 5: two BREAKs within BREAK_COALESCE_MS (sub-ms-spaced
// driver noise from a single electrical glitch) are coalesced
// into ONE event — the second-BREAK fast-confirm path must not
// fire on a single glitch.
static void test_subms_spaced_breaks_are_coalesced() {
    std::cout << "\n=== Pin 5: two sub-ms-spaced BREAKs coalesce into "
                 "ONE event (no fast-confirm from a single glitch) ==="
              << std::endl;
    NullArqCache cacheA, cacheB;
    AutoLinkConfig cfg = busyCfg();
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link a(mHal, cacheA, /*isMaster=*/true, cfg);
    Link b(sHal, cacheB, /*isMaster=*/false, cfg);
    bringToOk(a, b, mHal, sHal);
    agePastSettle(mHal, sHal);
    LinkTestAccessor(b).markAckedPending(0);

    sHal.deliver_break_to_self();
    assert(b.getState() == State::OK &&
           "Pin 5 pre: first BREAK arms the confirm window");
    // Pump 5 ms — well under BREAK_COALESCE_MS (10 ms) but
    // enough to deliver the second BREAK interrupt as a
    // separate driver event from a single glitch.
    for (uint32_t t = 0; t < 5; t += 1) {
        sHal.pumpClock(1);
    }
    sHal.deliver_break_to_self();
    assert(b.getState() == State::OK &&
           "Pin 5: a second BREAK inside BREAK_COALESCE_MS must be "
           "coalesced with the first (no fast-confirm) — the the prior release "
           "field log's failure mode was two sub-ms-spaced BREAKs from "
           "one glitch fast-confirming and tearing down a healthy link");
    assert(LinkTestAccessor(b).breakSuspectMsForTest() != 0 &&
           "Pin 5: breakSuspectMs_ must still be armed (the coalesced "
           "event is still under observation)");
    // Pump B's own timer past BREAK_CONFIRM_MS with no traffic —
    // the coalesced event confirms the reset on the deadline.
    bool confirmed = false;
    for (int i = 0; i < 20; i++) {
        sHal.pumpClock(20);
        b.onTimer();
        if (b.getState() == State::SWP) {
            confirmed = true;
            break;
        }
    }
    assert(confirmed &&
           "Pin 5: the coalesced event must still confirm the reset via "
           "the deadline — coalescing is purely about not letting a "
           "single glitch fast-confirm, not about suppressing the reset");
    assert(LinkTestAccessor(b).lastResetReasonForTest() ==
           ResetReason::HealthWatchdog);
    std::cout << "  Pin 5 PASS (sub-ms BREAKs coalesced, deadline "
                 "confirmed the reset)"
              << std::endl;
}

int main() {
    std::cout << "=== BREAK Confirm-Window Tests ===" << std::endl;
    test_busy_break_clears_on_valid_frame();
    test_busy_break_confirms_on_deadline();
    test_double_break_outside_coalesce_confirms_immediately();
    test_idle_break_also_debounces();
    test_subms_spaced_breaks_are_coalesced();
    std::cout << "\n=== All 5 BREAK confirm-window pins PASS ===" << std::endl;
    return 0;
}

#endif
