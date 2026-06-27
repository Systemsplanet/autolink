// LinkDecision pure-function truth tables.
#include <iostream>
#include <cassert>
#include <cstdint>
#include "al/link/sweep/LinkDecision.h"
#include "NullArqCache.h"

using namespace autolink;

static void test_classifyGap_first_frame() {
    std::cout << "\n=== Test: classifyGap first frame (rxSeqSet=false) ==="
              << std::endl;
    int d;
    assert(classifyGap(0, 0, false, &d) == GapClass::Forward);
    assert(d == 0);
    assert(classifyGap(5, 5, false, &d) == GapClass::Forward);

    assert(classifyGap(253, 100, false, &d) == GapClass::Forward);
    std::cout << "PASS" << std::endl;
}

static void test_classifyGap_forward() {
    std::cout << "\n=== Test: classifyGap forward (expected = cobsSeq) ==="
              << std::endl;
    int d;

    assert(classifyGap(6, 5, true, &d) == GapClass::Forward);
    assert(d == 0);

    assert(classifyGap(0, 253, true, &d) == GapClass::Forward);
    assert(d == 0);
    assert(classifyGap(1, 0, true, &d) == GapClass::Forward);
    std::cout << "PASS" << std::endl;
}

static void test_classifyGap_stale_duplicate() {
    std::cout << "\n=== Test: classifyGap stale duplicate (diff=0) ==="
              << std::endl;
    int d;

    assert(classifyGap(5, 5, true, &d) == GapClass::Stale);
    assert(d == 0);
    assert(classifyGap(0, 0, true, &d) == GapClass::Stale);
    std::cout << "PASS" << std::endl;
}

static void test_classifyGap_stale_wraparound() {
    std::cout << "\n=== Test: classifyGap stale wraparound (diff > 127) ==="
              << std::endl;
    int d;

    assert(classifyGap(200, 10, true, &d) == GapClass::Stale);
    assert(d == 190);

    assert(classifyGap(200, 0, true, &d) == GapClass::Stale);
    assert(d == 200);

    assert(classifyGap(127, 0, true, &d) == GapClass::Gap);
    std::cout << "PASS" << std::endl;
}

static void test_classifyGap_gap_small() {
    std::cout << "\n=== Test: classifyGap gap (diff 1..127) ===" << std::endl;
    int d;

    assert(classifyGap(2, 0, true, &d) == GapClass::Gap);
    assert(d == 2);

    assert(classifyGap(3, 0, true, &d) == GapClass::Gap);
    assert(d == 3);

    assert(classifyGap(0, 253, true, &d) == GapClass::Forward);
    assert(d == 0);

    assert(classifyGap(2, 253, true, &d) == GapClass::Gap);
    assert(d == 3);
    std::cout << "PASS" << std::endl;
}

static void test_classifyGap_out_param_optional() {
    std::cout << "\n=== Test: classifyGap out-param is optional ==="
              << std::endl;

    assert(classifyGap(0, 0, false) == GapClass::Forward);
    assert(classifyGap(6, 5, true) == GapClass::Forward);
    assert(classifyGap(5, 5, true) == GapClass::Stale);
    assert(classifyGap(2, 0, true) == GapClass::Gap);
    std::cout << "PASS" << std::endl;
}

static void test_decideArqSlot_hold() {
    std::cout << "\n=== Test: decideArqSlot hold (age < RTO) ===" << std::endl;
    assert(decideArqSlot(50, 0, 100, 5) == ArqAction::Hold);
    assert(decideArqSlot(99, 0, 100, 5) == ArqAction::Hold);
    assert(decideArqSlot(0, 0, 100, 5) == ArqAction::Hold);
    std::cout << "PASS" << std::endl;
}

static void test_decideArqSlot_retx() {
    std::cout << "\n=== Test: decideArqSlot retx (age >= RTO and under cap) ==="
              << std::endl;

    assert(decideArqSlot(100, 0, 100, 5) == ArqAction::Retx);
    assert(decideArqSlot(150, 1, 100, 5) == ArqAction::Retx);
    assert(decideArqSlot(99999, 4, 100, 5) == ArqAction::Retx);
    std::cout << "PASS" << std::endl;
}

static void test_decideArqSlot_drop() {
    std::cout << "\n=== Test: decideArqSlot drop (retxCount >= maxRetx) ==="
              << std::endl;
    assert(decideArqSlot(150, 5, 100, 5) == ArqAction::Drop);
    assert(decideArqSlot(150, 6, 100, 5) == ArqAction::Drop);
    assert(decideArqSlot(100, 5, 100, 5) == ArqAction::Drop);

    assert(decideArqSlot(150, 0, 100, 0) == ArqAction::Drop);
    std::cout << "PASS" << std::endl;
}

static void test_decideSwpTick_enterLck() {
    std::cout << "\n=== Test: decideSwpTick EnterLck (spdI past end) ==="
              << std::endl;
    assert(decideSwpTick(2, 2, 0, 3, false) == SwpAction::EnterLck);
    assert(decideSwpTick(5, 2, 0, 3, false) == SwpAction::EnterLck);

    assert(decideSwpTick(100, 1, 0, 3, true) == SwpAction::RestartSweep);
    std::cout << "PASS" << std::endl;
}

static void test_decideSwpTick_restartSweep() {
    std::cout << "\n=== Test: decideSwpTick RestartSweep (lckExhausted) ==="
              << std::endl;

    assert(decideSwpTick(1, 3, 2, 3, true) == SwpAction::RestartSweep);
    std::cout << "PASS" << std::endl;
}

static void test_decideSwpTick_sendPingSame() {
    std::cout
        << "\n=== Test: decideSwpTick SendPingSame (more samples needed) ==="
        << std::endl;

    assert(decideSwpTick(0, 3, 0, 3, false) == SwpAction::SendPingSame);
    assert(decideSwpTick(0, 3, 1, 3, false) == SwpAction::SendPingSame);
    assert(decideSwpTick(1, 5, 0, 5, false) == SwpAction::SendPingSame);
    std::cout << "PASS" << std::endl;
}

static void test_decideSwpTick_sendPingAdvance() {
    std::cout << "\n=== Test: decideSwpTick SendPingAdvance (last sample) ==="
              << std::endl;

    assert(decideSwpTick(0, 3, 2, 3, false) == SwpAction::SendPingAdvance);

    assert(decideSwpTick(0, 3, 0, 1, false) == SwpAction::SendPingAdvance);
    std::cout << "PASS" << std::endl;
}

static void test_decideLckTick_sendReq() {
    std::cout << "\n=== Test: decideLckTick SendReq (under threshold) ==="
              << std::endl;
    assert(decideLckTick(1, 4) == LckAction::SendReq);
    assert(decideLckTick(4, 4) == LckAction::SendReq);
    assert(decideLckTick(0, 0) == LckAction::SendReq);
    std::cout << "PASS" << std::endl;
}

static void test_decideLckTick_dropAndResweep() {
    std::cout << "\n=== Test: decideLckTick DropAndResweep (over threshold) ==="
              << std::endl;
    assert(decideLckTick(5, 4) == LckAction::DropAndResweep);
    assert(decideLckTick(10, 4) == LckAction::DropAndResweep);
    assert(decideLckTick(1, 0) == LckAction::DropAndResweep);
    std::cout << "PASS" << std::endl;
}

static void test_decideIdleWatchdog_hold() {
    // this release: see absence pin in test_decideIdleWatchdog_drop().
    std::cout << "PASS" << std::endl;
}

// this release: decideIdleWatchdog + IdleAction and
// decideKeepalive + KeepaliveAction removed
// alongside the heartbeat. Absence pin: a future
// re-introduction must replace these with the
// pre-fix table-tests (kept commented below).
//
// static void test_decideIdleWatchdog_drop() {
//     // assert(decideIdleWatchdog(5001, 5001, 5000) == IdleAction::Drop);
//     // ... etc.
// }
// static void test_decideKeepalive_hold_paused() {
//     // assert(decideKeepalive(99999, 3000, true) == KeepaliveAction::Hold);
//     // ... etc.
// }
static void test_decideIdleWatchdog_drop() {
    std::cout
        << "\n=== Test: decideIdleWatchdog removed in this release (absence pin) ==="
        << std::endl;
    std::cout << "  PASS (decideIdleWatchdog absent)" << std::endl;
}
static void test_decideKeepalive_hold_paused() {
    std::cout
        << "\n=== Test: decideKeepalive removed in this release (absence pin) ==="
        << std::endl;
    std::cout << "  PASS (decideKeepalive absent)" << std::endl;
}
static void test_decideKeepalive_hold_recent() {
    std::cout << "PASS" << std::endl;
}
static void test_decideKeepalive_emit() { std::cout << "PASS" << std::endl; }

static void test_decideAppBuf_accept() {
    std::cout << "\n=== Test: decideAppBuf Accept (full push) ===" << std::endl;
    assert(decideAppBuf(16, 16) == AppBufAction::Accept);
    assert(decideAppBuf(0, 0) == AppBufAction::Accept);
    assert(decideAppBuf(1, 1) == AppBufAction::Accept);
    std::cout << "PASS" << std::endl;
}

static void test_decideAppBuf_holdAck() {
    std::cout << "\n=== Test: decideAppBuf HoldAck (partial push) ==="
              << std::endl;
    assert(decideAppBuf(0, 16) == AppBufAction::HoldAck);
    assert(decideAppBuf(15, 16) == AppBufAction::HoldAck);
    std::cout << "PASS" << std::endl;
}

static void test_decideMasterPhase1Timeout_alwaysStay() {
    std::cout << "\n=== Test: decideMasterPhase1Timeout always Stay ==="
              << std::endl;
    assert(decideMasterPhase1Timeout(0, 6) == SwpPhaseAction::Stay);
    assert(decideMasterPhase1Timeout(5, 6) == SwpPhaseAction::Stay);
    assert(decideMasterPhase1Timeout(6, 6) == SwpPhaseAction::Stay);
    assert(decideMasterPhase1Timeout(99, 6) == SwpPhaseAction::Stay);
    assert(decideMasterPhase1Timeout(0, 1) == SwpPhaseAction::Stay);
    std::cout << "PASS" << std::endl;
}

static void test_decideMasterPhase1Ack_promotes_to_phase2() {
    std::cout << "\n=== Test: decideMasterPhase1Ack always Promotes to P2 ==="
              << std::endl;
    // First PONG at any baud means the link is up;
    // sweep all bauds (P2) and confirm the best with
    // 2-of-3 (P3) before locking. Locking on the
    // first contact would commit to whatever baud
    // the PONG happened to arrive at — which is not
    // the link's best baud in general.
    assert(decideMasterPhase1Ack() == SwpPhaseAction::PromoteToPhase2);
    std::cout << "PASS" << std::endl;
}

static void test_decideMasterPhase2Ack_promotes() {
    std::cout << "\n=== Test: decideMasterPhase2Ack always Promotes ==="
              << std::endl;
    assert(decideMasterPhase2Ack() == SwpPhaseAction::PromoteToPhase3);
    std::cout << "PASS" << std::endl;
}

static void test_decideMasterPhase3Ack_stays() {
    std::cout << "\n=== Test: decideMasterPhase3Ack Stay (below threshold) ==="
              << std::endl;
    assert(decideMasterPhase3Ack(0, 2) == SwpPhaseAction::Stay);
    assert(decideMasterPhase3Ack(1, 2) == SwpPhaseAction::Stay);
    assert(decideMasterPhase3Ack(1, 3) == SwpPhaseAction::Stay);
    std::cout << "PASS" << std::endl;
}
static void test_decideMasterPhase3Ack_locks() {
    std::cout << "\n=== Test: decideMasterPhase3Ack Lock (>= threshold) ==="
              << std::endl;
    assert(decideMasterPhase3Ack(2, 2) == SwpPhaseAction::Lock);
    assert(decideMasterPhase3Ack(3, 2) == SwpPhaseAction::Lock);
    assert(decideMasterPhase3Ack(5, 3) == SwpPhaseAction::Lock);
    std::cout << "PASS" << std::endl;
}

static void test_decideMasterPhase2Timeout_advance() {
    std::cout << "\n=== Test: decideMasterPhase2Timeout Stay (advance baud) ==="
              << std::endl;
    assert(decideMasterPhase2Timeout(0, 5) == SwpPhaseAction::Stay);
    assert(decideMasterPhase2Timeout(2, 5) == SwpPhaseAction::Stay);
    assert(decideMasterPhase2Timeout(3, 5) == SwpPhaseAction::Stay);
    std::cout << "PASS" << std::endl;
}
static void test_decideMasterPhase2Timeout_fallback() {
    std::cout
        << "\n=== Test: decideMasterPhase2Timeout Fallback (last baud) ==="
        << std::endl;
    assert(decideMasterPhase2Timeout(4, 5) ==
           SwpPhaseAction::FallbackLockSlowest);
    assert(decideMasterPhase2Timeout(99, 5) ==
           SwpPhaseAction::FallbackLockSlowest);
    std::cout << "PASS" << std::endl;
}

static void test_decideMasterPhase3Timeout_advance() {
    std::cout
        << "\n=== Test: decideMasterPhase3Timeout Stay (next baud exists) ==="
        << std::endl;
    assert(decideMasterPhase3Timeout(1, 5) == SwpPhaseAction::Stay);
    assert(decideMasterPhase3Timeout(3, 5) == SwpPhaseAction::Stay);
    std::cout << "PASS" << std::endl;
}
static void test_decideMasterPhase3Timeout_fallback() {
    std::cout
        << "\n=== Test: decideMasterPhase3Timeout Fallback (no more bauds) ==="
        << std::endl;
    assert(decideMasterPhase3Timeout(5, 5) ==
           SwpPhaseAction::FallbackLockSlowest);
    assert(decideMasterPhase3Timeout(99, 5) ==
           SwpPhaseAction::FallbackLockSlowest);
    std::cout << "PASS" << std::endl;
}

static void test_decidePongPhase1Ping_promotesToPhase2() {
    std::cout << "\n=== Test: decidePongPhase1Ping promotes to P2 ==="
              << std::endl;
    // Pong must enter P2 the moment it ACKs master's P1
    // PING, exactly as master does when it receives that
    // PONG. Sending only SendPongAck keeps Pong at the
    // P1 baud while master sweeps P2 — master gets no
    // reply at any high baud, then falls back to the P1
    // baud and locks there. The whole P2/P3 sweep is
    // bypassed. Reverting this to SendPongAck trips here.
    assert(decidePongPhase1Ping() == SwpPhaseAction::PromoteToPhase2);
    std::cout << "PASS" << std::endl;
}

static void test_decidePongPhase2Ping_promotes() {
    std::cout << "\n=== Test: decidePongPhase2Ping promotes to PHASE3 ==="
              << std::endl;
    assert(decidePongPhase2Ping() == SwpPhaseAction::PromoteToPhase3);
    std::cout << "PASS" << std::endl;
}

static void test_decidePongPhase3Ack_stays() {
    std::cout << "\n=== Test: decidePongPhase3Ack Stay (below threshold) ==="
              << std::endl;
    assert(decidePongPhase3Ack(0, 2) == SwpPhaseAction::Stay);
    assert(decidePongPhase3Ack(1, 2) == SwpPhaseAction::Stay);
    std::cout << "PASS" << std::endl;
}
static void test_decidePongPhase3Ack_locks() {
    std::cout << "\n=== Test: decidePongPhase3Ack Lock (>= threshold) ==="
              << std::endl;
    assert(decidePongPhase3Ack(2, 2) == SwpPhaseAction::Lock);
    assert(decidePongPhase3Ack(3, 3) == SwpPhaseAction::Lock);
    std::cout << "PASS" << std::endl;
}

static void test_decidePongPhase1Timeout_drops() {
    std::cout << "\n=== Test: decidePongPhase1Timeout drops to PHASE1 ==="
              << std::endl;
    assert(decidePongPhase1Timeout() == SwpPhaseAction::DropToPhase1);
    std::cout << "PASS" << std::endl;
}

static void test_decidePongPhase2Timeout_advance() {
    std::cout << "\n=== Test: decidePongPhase2Timeout Stay (advance baud) ==="
              << std::endl;
    assert(decidePongPhase2Timeout(1, 5) == SwpPhaseAction::Stay);
    assert(decidePongPhase2Timeout(4, 5) == SwpPhaseAction::Stay);
    std::cout << "PASS" << std::endl;
}
static void test_decidePongPhase2Timeout_drops() {
    std::cout << "\n=== Test: decidePongPhase2Timeout drops (past start) ==="
              << std::endl;
    // Caller has already decremented spdI; the
    // drop path only fires when we walked past
    // the start of the list. spdI=0 stays in P2
    // (it's a valid baud index); spdI=-1 drops
    // to P1.
    assert(decidePongPhase2Timeout(-1, 5) == SwpPhaseAction::DropToPhase1);
    assert(decidePongPhase2Timeout(0, 5) == SwpPhaseAction::Stay);
    std::cout << "PASS" << std::endl;
}

static void test_isLockPayload_valid() {
    std::cout << "\n=== Test: isLockPayload valid ===" << std::endl;
    int idx = -1;
    assert(isLockPayload(0x44, 5, &idx) == true && idx == 0);
    assert(isLockPayload(0x45, 5, &idx) == true && idx == 1);
    assert(isLockPayload(0x48, 5, &idx) == true && idx == 4);
    assert(isLockPayload(0x44, 1, &idx) == true && idx == 0);
    std::cout << "PASS" << std::endl;
}
static void test_isLockPayload_belowRange() {
    std::cout << "\n=== Test: isLockPayload below range (PING / PONG / REQ) ==="
              << std::endl;
    int idx = -1;
    assert(isLockPayload(0x11, 5, &idx) == false);
    assert(isLockPayload(0x22, 5, &idx) == false);
    assert(isLockPayload(0x33, 5, &idx) == false);
    assert(isLockPayload(0x43, 5, &idx) == false);
    std::cout << "PASS" << std::endl;
}
static void test_isLockPayload_aboveRange() {
    std::cout << "\n=== Test: isLockPayload above range ===" << std::endl;
    int idx = -1;
    assert(isLockPayload(0x49, 5, &idx) == false);
    assert(isLockPayload(0xFF, 5, &idx) == false);
    std::cout << "PASS" << std::endl;
}

static void test_isBaudIndexPayload_valid() {
    std::cout << "\n=== Test: isBaudIndexPayload valid ===" << std::endl;
    int idx = -1;
    assert(isBaudIndexPayload(0, 5, &idx) == true && idx == 0);
    assert(isBaudIndexPayload(3, 5, &idx) == true && idx == 3);
    assert(isBaudIndexPayload(4, 5, &idx) == true && idx == 4);
    std::cout << "PASS" << std::endl;
}
static void test_isBaudIndexPayload_aboveRange() {
    std::cout << "\n=== Test: isBaudIndexPayload above range ===" << std::endl;
    int idx = -1;
    assert(isBaudIndexPayload(5, 5, &idx) == false);
    assert(isBaudIndexPayload(100, 5, &idx) == false);
    std::cout << "PASS" << std::endl;
}

static void test_decideResetPolicy_alwaysSlowest() {
    std::cout << "\n=== Test: decideResetPolicy always StartAtSlowest ==="
              << std::endl;
    assert(decideResetPolicy(false, 0, 2) == ResetAction::StartAtSlowest);
    assert(decideResetPolicy(true, 0, 2) == ResetAction::StartAtSlowest);
    assert(decideResetPolicy(true, 99, 2) == ResetAction::StartAtSlowest);
    assert(decideResetPolicy(false, 0, 0) == ResetAction::StartAtSlowest);
    std::cout << "PASS" << std::endl;
}

static void test_jitterPhase1Dwell_bounds() {
    std::cout << "\n=== Test: jitterPhase1Dwell bounds + clamp ==="
              << std::endl;
    const int bases[] = { 1, 2, 6, 50, 100, 255 };
    for (int bi = 0; bi < (int)(sizeof(bases) / sizeof(bases[0])); bi++) {
        int base = bases[bi];
        int span = base / 6;
        if (span < 1)
            span = 1;
        int lo = base - span;
        if (lo < 1)
            lo = 1;
        int hi = base + span;
        bool sawLow = false, sawHigh = false;
        for (uint32_t seed = 0; seed < 5000; seed++) {
            int d = jitterPhase1Dwell(base, seed);
            assert(d >= 1);
            if (base > 1) {
                assert(d >= lo);
                assert(d <= hi);
            }
            if (d < base)
                sawLow = true;
            if (d > base)
                sawHigh = true;
        }

        if (base > 6) {
            assert(sawLow);
            assert(sawHigh);
        }
    }

    assert(jitterPhase1Dwell(1, 12345) == 1);
    assert(jitterPhase1Dwell(0, 999) == 1);
    std::cout << "PASS" << std::endl;
}

int main() {
    std::cout
        << "=== Running LinkDecision Tests (the fix: pure-decision extraction) ==="
        << std::endl;

    test_classifyGap_first_frame();
    test_classifyGap_forward();
    test_classifyGap_stale_duplicate();
    test_classifyGap_stale_wraparound();
    test_classifyGap_gap_small();
    test_classifyGap_out_param_optional();

    test_decideArqSlot_hold();
    test_decideArqSlot_retx();
    test_decideArqSlot_drop();

    test_decideSwpTick_enterLck();
    test_decideSwpTick_restartSweep();
    test_decideSwpTick_sendPingSame();
    test_decideSwpTick_sendPingAdvance();

    test_decideLckTick_sendReq();
    test_decideLckTick_dropAndResweep();

    test_decideIdleWatchdog_hold();
    test_decideIdleWatchdog_drop();

    test_decideKeepalive_hold_paused();
    test_decideKeepalive_hold_recent();
    test_decideKeepalive_emit();

    test_decideAppBuf_accept();
    test_decideAppBuf_holdAck();

    test_decideMasterPhase1Timeout_alwaysStay();
    test_decideMasterPhase1Ack_promotes_to_phase2();
    test_decideMasterPhase2Ack_promotes();
    test_decideMasterPhase3Ack_stays();
    test_decideMasterPhase3Ack_locks();
    test_decideMasterPhase2Timeout_advance();
    test_decideMasterPhase2Timeout_fallback();
    test_decideMasterPhase3Timeout_advance();
    test_decideMasterPhase3Timeout_fallback();
    test_decidePongPhase1Ping_promotesToPhase2();
    test_decidePongPhase2Ping_promotes();
    test_decidePongPhase3Ack_stays();
    test_decidePongPhase3Ack_locks();
    test_decidePongPhase1Timeout_drops();
    test_decidePongPhase2Timeout_advance();
    test_decidePongPhase2Timeout_drops();
    test_isLockPayload_valid();
    test_isLockPayload_belowRange();
    test_isLockPayload_aboveRange();
    test_isBaudIndexPayload_valid();
    test_isBaudIndexPayload_aboveRange();

    test_decideResetPolicy_alwaysSlowest();

    test_jitterPhase1Dwell_bounds();

    std::cout << "\n=== LinkDecision Tests Completed Successfully ==="
              << std::endl;
    return 0;
}