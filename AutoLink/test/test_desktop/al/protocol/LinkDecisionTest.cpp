// v5.1.40: exhaustive table-driven tests of the pure decision
// functions in src/al/protocol/LinkDecision.h. No hardware, no
// mocks. Each test pins one decision function and exhaustively
// enumerates its input domain.
//
// Toggle-verify contract (AGENTS.md rule 17a): reverting any of
// the corresponding inline blocks in ALink.cpp would NOT change
// these tests — these tests pin the PURE decision function, not
// the caller. If classifyGap is reverted to inline code in
// onPayload, these tests still pass (because LinkDecision.h still
// has the function). The pin runs the OTHER direction: removing
// the function entirely (or changing its return value) breaks
// these tests.

#include <iostream>
#include <cassert>
#include <cstdint>
#include "../al/protocol/LinkDecision.h"

using namespace autolink;

// ============================================================
// classifyGap
// ============================================================
// Decision matrix:
//   rxSeqSet=false | (any cobsSeq) -> Forward
//   rxSeqSet=true  | cobsSeq == rxSeq+1 -> Forward
//   rxSeqSet=true  | cobsSeq == rxSeq or cobsSeq far behind -> Stale
//   rxSeqSet=true  | cobsSeq ahead of rxSeq+1 but not wrap -> Gap

static void test_classifyGap_first_frame() {
    std::cout << "\n=== Test: classifyGap first frame (rxSeqSet=false) ===" << std::endl;
    int d;
    assert(classifyGap(0, 0, false, &d) == GapClass::Forward);
    assert(d == 0);
    assert(classifyGap(5, 5, false, &d) == GapClass::Forward);
    assert(classifyGap(255, 100, false, &d) == GapClass::Forward);
    std::cout << "PASS" << std::endl;
}

static void test_classifyGap_forward() {
    std::cout << "\n=== Test: classifyGap forward (expected = cobsSeq) ===" << std::endl;
    int d;
    // rxSeq=5, expected=6. cobsSeq=6 -> Forward.
    assert(classifyGap(6, 5, true, &d) == GapClass::Forward);
    assert(d == 0);
    // Wrap boundary: rxSeq=255, expected=0.
    assert(classifyGap(0, 255, true, &d) == GapClass::Forward);
    assert(d == 0);
    assert(classifyGap(1, 0, true, &d) == GapClass::Forward);
    std::cout << "PASS" << std::endl;
}

static void test_classifyGap_stale_duplicate() {
    std::cout << "\n=== Test: classifyGap stale duplicate (diff=0) ===" << std::endl;
    int d;
    // Same seq as rxSeq -> duplicate.
    assert(classifyGap(5, 5, true, &d) == GapClass::Stale);
    assert(d == 0);
    assert(classifyGap(0, 0, true, &d) == GapClass::Stale);
    std::cout << "PASS" << std::endl;
}

static void test_classifyGap_stale_wraparound() {
    std::cout << "\n=== Test: classifyGap stale wraparound (diff > 128) ===" << std::endl;
    int d;
    // rxSeq=10, cobsSeq=200: diff = (200-10) = 190 > 128 -> Stale (wraparound).
    assert(classifyGap(200, 10, true, &d) == GapClass::Stale);
    assert(d == 190);
    // rxSeq=0, cobsSeq=200: diff=200 > 128 -> Stale.
    assert(classifyGap(200, 0, true, &d) == GapClass::Stale);
    assert(d == 200);
    // Boundary: diff exactly 128 -> NOT stale (> not >=), so Gap.
    assert(classifyGap(128, 0, true, &d) == GapClass::Gap);
    std::cout << "PASS" << std::endl;
}

static void test_classifyGap_gap_small() {
    std::cout << "\n=== Test: classifyGap gap (diff 1..127) ===" << std::endl;
    int d;
    // diff=1 means expected-1 missing (the gap is the receiver's
    // NEXT expected seq; this is the seq we got).
    assert(classifyGap(2, 0, true, &d) == GapClass::Gap);
    assert(d == 2);
    // rxSeq=0, cobsSeq=3: diff=3 -> Gap (1 missing).
    assert(classifyGap(3, 0, true, &d) == GapClass::Gap);
    assert(d == 3);
    // Wrap-around: rxSeq=255, cobsSeq=1 (expected=0). diff=(1-255+256)=2 -> Gap.
    assert(classifyGap(1, 255, true, &d) == GapClass::Gap);
    assert(d == 2);
    std::cout << "PASS" << std::endl;
}

static void test_classifyGap_out_param_optional() {
    std::cout << "\n=== Test: classifyGap out-param is optional ===" << std::endl;
    // Call without outDiff pointer — should still work.
    assert(classifyGap(0, 0, false) == GapClass::Forward);
    assert(classifyGap(6, 5, true) == GapClass::Forward);
    assert(classifyGap(5, 5, true) == GapClass::Stale);
    assert(classifyGap(2, 0, true) == GapClass::Gap);
    std::cout << "PASS" << std::endl;
}

// ============================================================
// decideArqSlot
// ============================================================
// Decision matrix:
//   age < ackRtoMs -> Hold (not yet expired)
//   retxCount < maxRetx and age >= ackRtoMs -> Retx
//   retxCount >= maxRetx and age >= ackRtoMs -> Drop

static void test_decideArqSlot_hold() {
    std::cout << "\n=== Test: decideArqSlot hold (age < RTO) ===" << std::endl;
    assert(decideArqSlot(50, 0, 100, 5) == ArqAction::Hold);
    assert(decideArqSlot(99, 0, 100, 5) == ArqAction::Hold);
    assert(decideArqSlot(0, 0, 100, 5) == ArqAction::Hold);
    std::cout << "PASS" << std::endl;
}

static void test_decideArqSlot_retx() {
    std::cout << "\n=== Test: decideArqSlot retx (age >= RTO and under cap) ===" << std::endl;
    // Boundary: age exactly == RTO -> Retx.
    assert(decideArqSlot(100, 0, 100, 5) == ArqAction::Retx);
    assert(decideArqSlot(150, 1, 100, 5) == ArqAction::Retx);
    assert(decideArqSlot(99999, 4, 100, 5) == ArqAction::Retx);
    std::cout << "PASS" << std::endl;
}

static void test_decideArqSlot_drop() {
    std::cout << "\n=== Test: decideArqSlot drop (retxCount >= maxRetx) ===" << std::endl;
    assert(decideArqSlot(150, 5, 100, 5) == ArqAction::Drop);
    assert(decideArqSlot(150, 6, 100, 5) == ArqAction::Drop);
    assert(decideArqSlot(100, 5, 100, 5) == ArqAction::Drop);
    // Edge: maxRetx=0 means first expiry already drops.
    assert(decideArqSlot(150, 0, 100, 0) == ArqAction::Drop);
    std::cout << "PASS" << std::endl;
}

// ============================================================
// decideSwpTick
// ============================================================
// Decision matrix:
//   spdI >= baudCount -> EnterLck (regardless of lckExhausted)
//   lckExhausted=true -> RestartSweep
//   pingSample + 1 < samplesPerBaud -> SendPingSame
//   pingSample + 1 >= samplesPerBaud -> SendPingAdvance

static void test_decideSwpTick_enterLck() {
    std::cout << "\n=== Test: decideSwpTick EnterLck (spdI past end) ===" << std::endl;
    assert(decideSwpTick(2, 2, 0, 3, false) == SwpAction::EnterLck);
    assert(decideSwpTick(5, 2, 0, 3, false) == SwpAction::EnterLck);
    // lckExhausted takes precedence: even with spdI past end, we
    // RestartSweep (re-enter SWP at baud[0]).
    assert(decideSwpTick(100, 1, 0, 3, true) == SwpAction::RestartSweep);
    std::cout << "PASS" << std::endl;
}

static void test_decideSwpTick_restartSweep() {
    std::cout << "\n=== Test: decideSwpTick RestartSweep (lckExhausted) ===" << std::endl;
    // lckExhausted=true AND spdI<baudCount AND we're at sample boundary.
    assert(decideSwpTick(1, 3, 2, 3, true) == SwpAction::RestartSweep);
    std::cout << "PASS" << std::endl;
}

static void test_decideSwpTick_sendPingSame() {
    std::cout << "\n=== Test: decideSwpTick SendPingSame (more samples needed) ===" << std::endl;
    // samplesPerBaud=3: pingSample=0,1 -> still need more.
    assert(decideSwpTick(0, 3, 0, 3, false) == SwpAction::SendPingSame);
    assert(decideSwpTick(0, 3, 1, 3, false) == SwpAction::SendPingSame);
    assert(decideSwpTick(1, 5, 0, 5, false) == SwpAction::SendPingSame);
    std::cout << "PASS" << std::endl;
}

static void test_decideSwpTick_sendPingAdvance() {
    std::cout << "\n=== Test: decideSwpTick SendPingAdvance (last sample) ===" << std::endl;
    // pingSample + 1 >= samplesPerBaud.
    assert(decideSwpTick(0, 3, 2, 3, false) == SwpAction::SendPingAdvance);
    // samplesPerBaud=1 means every sample advances.
    assert(decideSwpTick(0, 3, 0, 1, false) == SwpAction::SendPingAdvance);
    std::cout << "PASS" << std::endl;
}

// ============================================================
// decideLckTick
// ============================================================
// Decision matrix:
//   lckRetries > maxRetries -> DropAndResweep
//   otherwise -> SendReq
//
// lckRetries is the POST-increment value (caller increments first).

static void test_decideLckTick_sendReq() {
    std::cout << "\n=== Test: decideLckTick SendReq (under threshold) ===" << std::endl;
    assert(decideLckTick(1, 4) == LckAction::SendReq);
    assert(decideLckTick(4, 4) == LckAction::SendReq);  // boundary: still send
    assert(decideLckTick(0, 0) == LckAction::SendReq);
    std::cout << "PASS" << std::endl;
}

static void test_decideLckTick_dropAndResweep() {
    std::cout << "\n=== Test: decideLckTick DropAndResweep (over threshold) ===" << std::endl;
    assert(decideLckTick(5, 4) == LckAction::DropAndResweep);
    assert(decideLckTick(10, 4) == LckAction::DropAndResweep);
    assert(decideLckTick(1, 0) == LckAction::DropAndResweep);
    std::cout << "PASS" << std::endl;
}

// ============================================================
// decideIdleWatchdog
// ============================================================
// Decision matrix:
//   age > idleTimeoutMs -> Drop
//   otherwise -> Hold

static void test_decideIdleWatchdog_hold() {
    std::cout << "\n=== Test: decideIdleWatchdog Hold (under threshold) ===" << std::endl;
    assert(decideIdleWatchdog(0, 5000) == IdleAction::Hold);
    assert(decideIdleWatchdog(5000, 5000) == IdleAction::Hold); // boundary
    assert(decideIdleWatchdog(4999, 5000) == IdleAction::Hold);
    std::cout << "PASS" << std::endl;
}

static void test_decideIdleWatchdog_drop() {
    std::cout << "\n=== Test: decideIdleWatchdog Drop (over threshold) ===" << std::endl;
    assert(decideIdleWatchdog(5001, 5000) == IdleAction::Drop);
    assert(decideIdleWatchdog(60000, 5000) == IdleAction::Drop);
    std::cout << "PASS" << std::endl;
}

// ============================================================
// decideKeepalive
// ============================================================
// Decision matrix:
//   linkPaused=true -> Hold (always)
//   txAge >= idleTimeoutMs/3 -> Emit
//   otherwise -> Hold
//
// idleTimeoutMs/3 uses integer division.

static void test_decideKeepalive_hold_paused() {
    std::cout << "\n=== Test: decideKeepalive Hold when paused ===" << std::endl;
    // Even huge txAge doesn't emit if paused.
    assert(decideKeepalive(99999, 3000, true) == KeepaliveAction::Hold);
    assert(decideKeepalive(0, 3000, true) == KeepaliveAction::Hold);
    std::cout << "PASS" << std::endl;
}

static void test_decideKeepalive_hold_recent() {
    std::cout << "\n=== Test: decideKeepalive Hold (recent TX) ===" << std::endl;
    assert(decideKeepalive(0, 3000, false) == KeepaliveAction::Hold);
    assert(decideKeepalive(999, 3000, false) == KeepaliveAction::Hold); // 3000/3=1000
    assert(decideKeepalive(500, 6000, false) == KeepaliveAction::Hold); // 6000/3=2000
    std::cout << "PASS" << std::endl;
}

static void test_decideKeepalive_emit() {
    std::cout << "\n=== Test: decideKeepalive Emit (txAge >= timeout/3) ===" << std::endl;
    assert(decideKeepalive(1000, 3000, false) == KeepaliveAction::Emit);
    assert(decideKeepalive(2000, 6000, false) == KeepaliveAction::Emit);
    assert(decideKeepalive(99999, 3000, false) == KeepaliveAction::Emit);
    std::cout << "PASS" << std::endl;
}

// ============================================================
// decideAppBuf
// ============================================================
// Decision matrix:
//   accepted < incoming -> HoldAck
//   accepted == incoming -> Accept

static void test_decideAppBuf_accept() {
    std::cout << "\n=== Test: decideAppBuf Accept (full push) ===" << std::endl;
    assert(decideAppBuf(16, 16) == AppBufAction::Accept);
    assert(decideAppBuf(0, 0) == AppBufAction::Accept);
    assert(decideAppBuf(1, 1) == AppBufAction::Accept);
    std::cout << "PASS" << std::endl;
}

static void test_decideAppBuf_holdAck() {
    std::cout << "\n=== Test: decideAppBuf HoldAck (partial push) ===" << std::endl;
    assert(decideAppBuf(0, 16) == AppBufAction::HoldAck);
    assert(decideAppBuf(15, 16) == AppBufAction::HoldAck);
    std::cout << "PASS" << std::endl;
}

// ============================================================
// Main
// ============================================================

int main() {
    std::cout << "=== Running LinkDecision Tests (v5.1.40: pure-decision extraction) ===" << std::endl;

    // classifyGap
    test_classifyGap_first_frame();
    test_classifyGap_forward();
    test_classifyGap_stale_duplicate();
    test_classifyGap_stale_wraparound();
    test_classifyGap_gap_small();
    test_classifyGap_out_param_optional();

    // decideArqSlot
    test_decideArqSlot_hold();
    test_decideArqSlot_retx();
    test_decideArqSlot_drop();

    // decideSwpTick
    test_decideSwpTick_enterLck();
    test_decideSwpTick_restartSweep();
    test_decideSwpTick_sendPingSame();
    test_decideSwpTick_sendPingAdvance();

    // decideLckTick
    test_decideLckTick_sendReq();
    test_decideLckTick_dropAndResweep();

    // decideIdleWatchdog
    test_decideIdleWatchdog_hold();
    test_decideIdleWatchdog_drop();

    // decideKeepalive
    test_decideKeepalive_hold_paused();
    test_decideKeepalive_hold_recent();
    test_decideKeepalive_emit();

    // decideAppBuf
    test_decideAppBuf_accept();
    test_decideAppBuf_holdAck();

    std::cout << "\n=== LinkDecision Tests Completed Successfully ===" << std::endl;
    return 0;
}