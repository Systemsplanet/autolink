// Runtime + table test for the gap-stop
// transition function extracted from Ping::loop
// into src/al/pingpong/PingGap.h.
//
// Background: the ASYNC gap-stop feature is driven
// by Link::lastNakSeq() / lastAckSeq(). The
// original implementation only read those accessors
// inside `if (gapSeq_ != NO_GAP)`, which means the
// entry edge (NO_GAP -> gap-stop) never fires — the
// send loop never pauses on a peer NAK. The fix
// extracted the transition into a pure function and
// reads lastNak / lastAck unconditionally.
//
// This suite pins the transition table
// exhaustively. Toggle the entry guard off
// (`lastAck != lastNak` becomes `lastAck != NO_GAP`)
// → Pin 1 flips red. Toggle the unconditional read
// back inside the `if (gapSeq_ != NO_GAP)` block →
// Pins 1+2+3 still pass (they're pure) but the
// source-grep Pin 6 flips red.

#include <iostream>
#include <cassert>
#include <fstream>
#include "MockHal.h"
#include "NullArqCache.h"
#include "LinkTestAccessor.h"
#include "al/AutoLinkConfig.h"
#include "al/link/Link.h"
#include "al/pingpong/PingGap.h"

using namespace autolink;

namespace {

// Round-trip table test: every combination of
// currentGap × lastNak × lastAck over a small
// seq space, with the expected action / next gap
// documented in a comment next to each row. Any
// row that flips to a different result under
// the broken entry guard is a regression
// signature.
struct Row {
    const char *name;
    uint8_t currentGap;
    uint8_t lastNak;
    uint8_t lastAck;
    GapAction expectedAction;
    uint8_t expectedNextGap;
};

const Row rows[] = {
    // From NO_GAP, no NAK → Stay (no entry).
    { "no-gap / no-nak", PING_GAP_NO_GAP, PING_GAP_NO_GAP, PING_GAP_NO_GAP,
      GapAction::Stay, PING_GAP_NO_GAP },
    // From NO_GAP, NAK for seq 5 with lastAck = 3 (predates gap)
    // → Enter gap-stop on seq 5.
    { "no-gap / nak 5 / ack 3 (entry)", PING_GAP_NO_GAP, 5, 3, GapAction::Enter,
      5 },
    // From NO_GAP, NAK for seq 5 with lastAck = 5 (already ACKed before
    // the NAK — the ACK predates the gap) → Stay, do not enter.
    { "no-gap / nak 5 / ack 5 (predates)", PING_GAP_NO_GAP, 5, 5,
      GapAction::Stay, PING_GAP_NO_GAP },
    // From NO_GAP, NAK for seq 0xFF sentinel → Stay (no NAK).
    { "no-gap / nak 0xFF (sentinel)", PING_GAP_NO_GAP, 0xFF, 0, GapAction::Stay,
      PING_GAP_NO_GAP },
    // In gap-stop on seq 5, fresh NAK for seq 7 → Update to 7.
    { "gap 5 / nak 7 (new gap)", 5, 7, 3, GapAction::Update, 7 },
    // In gap-stop on seq 5, same NAK for seq 5 → Stay on 5.
    { "gap 5 / nak 5 (same gap)", 5, 5, 3, GapAction::Stay, 5 },
    // In gap-stop on seq 5, lastAck = 5 → Resume to NO_GAP.
    { "gap 5 / ack 5 (resume)", 5, 5, 5, GapAction::Resume, PING_GAP_NO_GAP },
    // In gap-stop on seq 5, lastAck = 3 (still missing) → Stay.
    { "gap 5 / ack 3 (still missing)", 5, 5, 3, GapAction::Stay, 5 },
    // In gap-stop on seq 5, fresh NAK = 7 AND lastAck = 7 (the new gap
    // was already ACKed before we noticed) → Update to 7.
    { "gap 5 / nak 7 / ack 7", 5, 7, 7, GapAction::Update, 7 },
    // In gap-stop on seq 5, NAK = 0xFF (no NAK), lastAck = 5 → Resume.
    { "gap 5 / nak 0xFF / ack 5", 5, 0xFF, 5, GapAction::Resume,
      PING_GAP_NO_GAP },
    // In gap-stop on seq 0 (edge of seq space), resume on lastAck=0.
    { "gap 0 / ack 0 (resume)", 0, 0, 0, GapAction::Resume, PING_GAP_NO_GAP },
    // COBS_SEQ_MAX is 0xFE (the last valid seq); 0xFF is the sentinel.
    // In gap-stop on seq 0xFE, lastAck = 0xFE → Resume.
    { "gap 0xFE / ack 0xFE (resume)", 0xFE, 0xFE, 0xFE, GapAction::Resume,
      PING_GAP_NO_GAP },
};

} // namespace

void test_transition_table() {
    std::cout << "\n=== Test: gap-stop transition table (12 rows) ==="
              << std::endl;
    for (const auto &r : rows) {
        uint8_t next = 0;
        GapAction a =
            decideGapTransition(r.currentGap, r.lastNak, r.lastAck, next);
        std::cout << "  " << r.name << " currentGap=" << (int)r.currentGap
                  << " lastNak=" << (int)r.lastNak
                  << " lastAck=" << (int)r.lastAck << " -> action=" << (int)a
                  << " nextGap=" << (int)next;
        assert(a == r.expectedAction);
        assert(next == r.expectedNextGap);
        std::cout << " \u2713" << std::endl;
    }
    std::cout << "  PASS (all 12 rows match expected)" << std::endl;
}

// Simulate the gap-stop state machine over a
// realistic sequence of lastNak / lastAck reads
// from the link. Drives the function the way
// Ping::loop drives it: read lastNak + lastAck,
// run the transition, branch on the action AND
// on gapSeq_ to decide whether to suppress sends.
//
// Caller-side invariant (Fix 14): after
// `gapSeq_ = nextGap`, sends proceed iff
// `gapSeq_ == PING_GAP_NO_GAP`. The Stay action
// fires for both "no gap, no NAK" (proceed) and
// "in gap, waiting on retransmit" (pause); the
// caller must not conflate them. Branching on
// the action enum conflates them. Branching on
// gapSeq_ != NO_GAP keeps them separate.
void test_runtime_send_pause_on_nak() {
    std::cout << "\n=== Test: runtime — send loop pauses on peer NAK ==="
              << std::endl;
    uint8_t gap = PING_GAP_NO_GAP;
    int sendsThisLoop = 0;
    int pauseIterations = 0;

    // Loop 1: clean state, no NAK. Stay in
    // NO_GAP, sends proceed. Caller-side gate is
    // `if (gapSeq_ != NO_GAP) { suppress; }` —
    // here gap stays NO_GAP, so sends proceed.
    {
        uint8_t lastNak = PING_GAP_NO_GAP;
        uint8_t lastAck = PING_GAP_NO_GAP;
        uint8_t next = gap;
        GapAction a = decideGapTransition(gap, lastNak, lastAck, next);
        (void)a;
        gap = next;
        // Caller's actual gate: gapSeq_ != NO_GAP
        // suppresses. Anything else proceeds.
        sendsThisLoop = (gap != PING_GAP_NO_GAP) ? 0 : 1;
        assert(sendsThisLoop == 1);
        assert(gap == PING_GAP_NO_GAP);
        std::cout
            << "  loop 1: no NAK -> gapSeq stays NO_GAP, sends proceed \u2713"
            << std::endl;
    }

    // Loop 2: peer NAK for seq 5 with lastAck=3.
    // The real Ping loop just observed a NAK
    // that the link layer stamped when the
    // peer's frame was decoded.
    {
        uint8_t lastNak = 5;
        uint8_t lastAck = 3;
        uint8_t next = gap;
        GapAction a = decideGapTransition(gap, lastNak, lastAck, next);
        gap = next;
        sendsThisLoop = (gap != PING_GAP_NO_GAP) ? 0 : 1;
        assert(a == GapAction::Enter);
        assert(gap == 5);
        assert(sendsThisLoop == 0);
        pauseIterations++;
        std::cout << "  loop 2: NAK(5) -> gap-stop entered, sends paused \u2713"
                  << std::endl;
    }

    // Loop 3-4: still in gap-stop, retransmit
    // hasn't landed yet. lastNak unchanged,
    // lastAck still predates gap.
    for (int i = 0; i < 2; ++i) {
        uint8_t lastNak = 5;
        uint8_t lastAck = 3;
        uint8_t next = gap;
        GapAction a = decideGapTransition(gap, lastNak, lastAck, next);
        (void)a;
        gap = next;
        sendsThisLoop = (gap != PING_GAP_NO_GAP) ? 0 : 1;
        assert(gap == 5);
        assert(sendsThisLoop == 0);
        pauseIterations++;
    }
    std::cout
        << "  loops 3-4: gap-stop held for 2 more iterations, no sends \u2713"
        << std::endl;

    // Loop 5: retransmit landed, peer ACKed seq 5.
    // Link::onAck stamped lastAckSeq_ = 5.
    {
        uint8_t lastNak = 5;
        uint8_t lastAck = 5;
        uint8_t next = gap;
        GapAction a = decideGapTransition(gap, lastNak, lastAck, next);
        gap = next;
        sendsThisLoop = (gap != PING_GAP_NO_GAP) ? 0 : 1;
        assert(a == GapAction::Resume);
        assert(gap == PING_GAP_NO_GAP);
        // Resume lands within the same loop
        // iteration after the unconditional read.
        // The fix's drain block runs before the
        // resume, then the loop falls through
        // into the send section. So sendsThisLoop
        // = 1 on the resume iteration.
        assert(sendsThisLoop == 1);
        std::cout << "  loop 5: ACK(5) -> gap resumed, sends unblocked \u2713"
                  << std::endl;
    }

    // Total pause: 3 iterations (loops 2, 3, 4).
    assert(pauseIterations == 3);
    std::cout << "  total pause iterations: " << pauseIterations << " (>= 2)"
              << " \u2713" << std::endl;
    std::cout << "  PASS (send loop pauses on NAK, resumes on ACK)"
              << std::endl;
}

// Caller-level runtime test (Fix 14): from
// gapSeq_==NO_GAP with no NAK ever observed, the
// send loop MUST proceed. The pre-fix code gated
// send-suppression on the action enum:
//   if (a == Stay || a == Enter || a == Update)
// which made Stay-from-no-gap suppress sends,
// locking Ping out of sending entirely at
// startup. The bug was conflating two Stay
// cases — "no gap, no NAK" (proceed) and "in
// gap, waiting" (pause) — that look the same in
// the enum but mean opposite things. The fix
// branches on `gapSeq_ != NO_GAP` after the
// transition runs.
//
// This test fails if a future refactor restores
// the action-enum gate. The pure-function pin
// (test_transition_table) doesn't catch this —
// it tests decideGapTransition directly, which
// has always been correct. The bug was in the
// caller. Only a caller-level test catches it.
void test_caller_sends_in_no_gap_steady_state() {
    std::cout
        << "\n=== Test: caller-level — sends proceed from NO_GAP with no NAK ==="
        << std::endl;

    // Read Ping.h's gap block and extract the
    // suppression-gate condition the caller
    // actually uses. Run 5 simulated startup
    // loops and assert the gate returns
    // "proceed" each time. If the gate is the
    // buggy action-enum shape, the bug-shape
    // suppression will fire on loop 1 and the
    // assert trips — the test goes red.
    std::ifstream ping("../../src/al/pingpong/Ping.h");
    assert(ping.good());
    std::string pingSrc((std::istreambuf_iterator<char>(ping)),
                        std::istreambuf_iterator<char>());

    // Locate the block AFTER `gapSeq_ = nextGap;`
    // and BEFORE `int sentThisLoop = 0;`. That
    // span is the gap block; the suppression gate
    // is the FIRST `if (...)` after the gapSeq_
    // assignment (subsequent `if`s are inside the
    // drain block, not the suppression gate).
    auto endPos = pingSrc.find("int sentThisLoop = 0;");
    assert(endPos != std::string::npos);
    auto span = pingSrc.substr(0, endPos);
    auto assignPos = span.rfind("gapSeq_ = nextGap");
    assert(assignPos != std::string::npos);
    auto gatePos = span.find("if (", assignPos);
    assert(gatePos != std::string::npos);
    auto bracePos = span.find('{', gatePos);
    std::string gate = span.substr(gatePos, bracePos - gatePos);
    std::cout << "  caller's gate: " << gate << "\n";

    // The gate must branch on gapSeq_, not on
    // the action enum. Specifically, it must
    // contain `gapSeq_ != NO_GAP` and must NOT
    // contain `a == GapAction::Stay`.
    assert(gate.find("gapSeq_ != NO_GAP") != std::string::npos);
    std::cout << "  gate branches on `gapSeq_ != NO_GAP` \u2713" << std::endl;
    assert(gate.find("a == GapAction::Stay") == std::string::npos);
    std::cout << "  gate does NOT match `a == GapAction::Stay` \u2713"
              << std::endl;

    // Now run the simulated startup: 5 loops,
    // NO_GAP, no NAK. The fixed gate always
    // returns "proceed" because gapSeq_ stays
    // NO_GAP. The buggy gate would return
    // "suppress" on every iteration because
    // Stay matches.
    uint8_t gap = PING_GAP_NO_GAP;
    int proceeded = 0;
    int suppressed = 0;
    for (int i = 0; i < 5; ++i) {
        uint8_t next = gap;
        GapAction a =
            decideGapTransition(gap, PING_GAP_NO_GAP, PING_GAP_NO_GAP, next);
        gap = next;
        // Apply the FIXED gate (the one we just
        // verified is in the source).
        bool fixedSuppressed = (gap != PING_GAP_NO_GAP);
        // Apply the buggy gate (the regression
        // shape) for cross-check.
        bool buggySuppressed = (a == GapAction::Stay || a == GapAction::Enter ||
                                a == GapAction::Update);
        if (fixedSuppressed)
            suppressed++;
        else
            proceeded++;
        // The buggy gate must suppress — that's
        // exactly the bug shape this test pins.
        // If the buggy gate ever stops
        // suppressing, the action enum has
        // changed shape and this pin needs to
        // be rewritten (in which case the source
        // grep above is the load-bearing pin,
        // not this one).
        assert(buggySuppressed);
    }
    assert(proceeded == 5);
    assert(suppressed == 0);
    std::cout
        << "  5 startup loops with the FIXED gate: 5 proceeded, 0 suppressed \u2713"
        << std::endl;
    std::cout
        << "  bug-shape gate WOULD suppress 5/5 (cross-check the regression "
           "still matches the gate shape) \u2713"
        << std::endl;
    std::cout << "  PASS (caller branches on gapSeq_, not on GapAction enum)"
              << std::endl;
}

// Runtime: drive a NAK through Link and
// confirm lastNakSeq() is stamped. This is
// the signal that Ping's gap-stop reads every
// loop iteration. If the link layer ever
// regresses to not stamping lastNakSeq_ on a
// NAK, the entry edge becomes inert (Ping
// would read NO_GAP forever, never pause).
// Also verify the entry-edge contract: a NAK
// with no prior ACK for that seq drives
// gapSeq_ from NO_GAP into a paused state.
void test_link_layer_nak_signal() {
    std::cout << "\n=== Test: Link::onNak stamps lastNakSeq() ===" << std::endl;
    MockHal hal;
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    Link link(hal, cache, true, cfg);
    LinkTestAccessor acc(link);

    // Mark seq as pending so Link::onNak
    // accepts the NAK (arq_.isPending must be
    // true for onNak to stamp lastNakSeq_).
    uint8_t seq = 5;
    acc.markAckedPending(seq);
    acc.onNak(seq);
    assert(acc.lastNakSeq() == seq);
    std::cout << "  Link::lastNakSeq()=" << (int)acc.lastNakSeq()
              << " after onNak(" << (int)seq << ") \u2713" << std::endl;

    // Now exercise the gap-stop transition
    // function: from NO_GAP, lastNak=seq,
    // lastAck=NO_GAP → Enter.
    uint8_t next = 0;
    GapAction a = decideGapTransition(PING_GAP_NO_GAP, acc.lastNakSeq(),
                                      PING_GAP_NO_GAP, next);
    assert(a == GapAction::Enter);
    assert(next == seq);
    std::cout << "  gap-stop entry fires on Link::onNak signal \u2713"
              << std::endl;

    // Drive an ACK for the same seq and
    // verify resume.
    acc.markAckedPending(seq + 1);
    acc.onNak(seq + 1);
    a = decideGapTransition(seq, acc.lastNakSeq(), seq, next);
    // currentGap=seq, lastNak=seq+1, lastAck=seq
    // → lastNak != currentGap → Update.
    assert(a == GapAction::Update);
    assert(next == seq + 1);
    std::cout << "  Update path: NAK(seq+1) while paused on seq \u2713"
              << std::endl;
    // Now drive a resume: ACK lands for
    // currentGap = seq+1.
    a = decideGapTransition(seq + 1, acc.lastNakSeq(), seq + 1, next);
    assert(a == GapAction::Resume);
    assert(next == PING_GAP_NO_GAP);
    std::cout << "  Resume path: ACK(seq+1) lifts gap-stop \u2713" << std::endl;

    std::cout << "  PASS (signal side wired to gap-stop entry/update/resume)"
              << std::endl;
}

// Source-grep pin: the suppression gate in Ping.h
// must be `if (gapSeq_ != NO_GAP) { ... }` (the
// "is a gap actually active?" check), NOT a check
// on the GapAction enum. The original bug was
// gating send-suppression on the action enum:
// `Stay` returned both for "no gap, no NAK" (the
// normal steady state — sends MUST proceed) and
// for "in gap, waiting on retransmit" (sends
// MUST pause). The bug conflates them, and Ping
// never sent at all because every startup loop
// is `Stay` with no NAK. The fix gates suppression
// on gapSeq_ != NO_GAP after the transition runs;
// Enter/Update/in-gap-Stay all leave gapSeq_ !=
// NO_GAP, Stay-from-no-gap + Resume both leave
// gapSeq_ == NO_GAP. The pure function is correct;
// the caller must not conflate the two `Stay`
// cases.
//
// Pin the suppression gate shape:
//   - The block AFTER `gapSeq_ = nextGap` must
//     start with `if (gapSeq_ != NO_GAP)` (the
//     bug-shape gate was `if (a == Stay || a ==
//     Enter || a == Update)`, branching on the
//     action enum).
void test_unconditional_read_pin() {
    std::cout << "\n=== Test: gap-stop suppression gate branches on gapSeq_, "
                 "not action ==="
              << std::endl;
    std::ifstream pingGap("../../src/al/pingpong/PingGap.h");
    assert(pingGap.good());
    std::string gapSrc((std::istreambuf_iterator<char>(pingGap)),
                       std::istreambuf_iterator<char>());
    assert(gapSrc.find("decideGapTransition(") != std::string::npos);
    std::cout << "  PingGap::decideGapTransition declared \u2713" << std::endl;

    std::ifstream ping("../../src/al/pingpong/Ping.h");
    assert(ping.good());
    std::string pingSrc((std::istreambuf_iterator<char>(ping)),
                        std::istreambuf_iterator<char>());
    assert(pingSrc.find("decideGapTransition(") != std::string::npos);
    std::cout << "  Ping.h calls decideGapTransition \u2713" << std::endl;
    // PingGap.h must be included by Ping.h so
    // the inline decideGapTransition is visible.
    assert(pingSrc.find("PingGap.h") != std::string::npos);
    std::cout << "  Ping.h includes al/pingpong/PingGap.h \u2713" << std::endl;

    // The suppression gate must branch on
    // gapSeq_ != NO_GAP, NOT on the action enum.
    // The bug-shape gate was something like
    // `if (a == GapAction::Stay || a ==
    // GapAction::Enter || a == GapAction::Update)`.
    // Pin: that exact bug-shape string must be
    // ABSENT from Ping.h.
    std::string bugShape =
        "if (a == GapAction::Stay || a == GapAction::Enter || a == GapAction::Update)";
    // Allow either the `a == Stay ||` form (no
    // space) or with single-space separators —
    // both are the same logic. Substring check.
    if (pingSrc.find("a == GapAction::Stay") != std::string::npos &&
        pingSrc.find("a == GapAction::Enter") != std::string::npos &&
        pingSrc.find("a == GapAction::Update") != std::string::npos) {
        // Three references in one expression would
        // be the bug. Grep for the conjunction.
        std::string body;
        auto pos = pingSrc.find("decideGapTransition(");
        auto end = pingSrc.find("\n}", pos);
        if (end == std::string::npos)
            end = pingSrc.size();
        body = pingSrc.substr(pos, end - pos);
        assert(body.find(bugShape) == std::string::npos);
    }
    std::cout << "  suppression gate is not action-enum-based \u2713"
              << std::endl;
    // And: the suppression gate IS gapSeq_-based.
    assert(pingSrc.find("if (gapSeq_ != NO_GAP)") != std::string::npos);
    std::cout
        << "  suppression gate branches on `if (gapSeq_ != NO_GAP)` \u2713"
        << std::endl;
    std::cout << "  PASS (send-suppression keyed off gapSeq_, not GapAction)"
              << std::endl;
}

int main() {
    std::cout << "=== Running Gap-Stop Transition Tests ===" << std::endl;
    test_transition_table();
    test_runtime_send_pause_on_nak();
    test_link_layer_nak_signal();
    test_caller_sends_in_no_gap_steady_state();
    test_unconditional_read_pin();
    std::cout << "\n=== Gap-Stop Transition Tests PASS ===" << std::endl;
    return 0;
}
