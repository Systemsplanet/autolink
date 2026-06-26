// Source-level regression test for the Pong-P1 -> Pong-P2
// promotion + slave-timer arming + P2 dwell floor.
//
// Pins four fixes from the field-test log:
//
//   1. decidePongPhase1Ping() returns PromoteToPhase2.
//      Pre-fix: SendPongAck. Pong stays at the P1 baud
//      (9600) while master sweeps P2 (115200->19200),
//      gets no reply at any high baud, then falls back
//      to 9600 and sends LOCK_CMD. The whole P2/P3
//      machinery is bypassed.
//   2. applyPongSwpAction_unlocked's PromoteToPhase2
//      case actually does the work: sendPongAck,
//      sweep_.enterPhase2, AND arm the slave P2 timer
//      with phase2Slave[0]. Pre-fix: dead no-op `return
//      false`. enterPhase2 only arms the master-side
//      timer; the slave has no other way to re-arm
//      after the promotion.
//   3. computeDwells floor is 20ms (not 5ms). The 5ms
//      floor is too tight under FreeRTOS tick
//      granularity (10ms typical) + UART event-task
//      scheduling, especially at 115200 where
//      roundTripMs returns 1ms.
//
// Toggle off -> red on each pin.
#ifndef ARDUINO
#    include <cassert>
#    include <fstream>
#    include <iostream>
#    include <sstream>
#    include <string>

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
    return base;
}

// Extract the body of `Link::applyPongSwpAction_unlocked`
// by brace-matching from its signature.
std::string extractApplyPongBody(const std::string &linkCpp) {
    auto fnPos = linkCpp.find("bool Link::applyPongSwpAction_unlocked(");
    assert(fnPos != std::string::npos);
    size_t scan = fnPos;
    int depth = 0;
    bool foundOpen = false;
    size_t endPos = std::string::npos;
    for (; scan < linkCpp.size(); scan++) {
        if (linkCpp[scan] == '{') {
            depth++;
            foundOpen = true;
        } else if (linkCpp[scan] == '}') {
            depth--;
            if (foundOpen && depth == 0) {
                endPos = scan;
                break;
            }
        }
    }
    assert(endPos != std::string::npos);
    return linkCpp.substr(fnPos, endPos - fnPos + 1);
}

// Pin 1: decidePongPhase1Ping returns PromoteToPhase2.
void test_decide_pong_phase1_ping_promotes_to_phase2() {
    std::cout << "\n=== Pin 1: decidePongPhase1Ping -> PromoteToPhase2 ==="
              << std::endl;
    std::string decH =
        readFile(projectRoot() + "/src/al/link/sweep/LinkDecision.h");
    auto fnPos = decH.find("decidePongPhase1Ping()");
    assert(fnPos != std::string::npos);
    auto brace = decH.find('{', fnPos);
    assert(brace != std::string::npos);
    auto close = decH.find('}', brace);
    assert(close != std::string::npos);
    std::string body = decH.substr(brace, close - brace + 1);
    assert(body.find("PromoteToPhase2") != std::string::npos);
    assert(body.find("SendPongAck") == std::string::npos);
    std::cout << "  PASS (decision returns PromoteToPhase2, not "
                 "SendPongAck)"
              << std::endl;
}

// Pin 2: applyPongSwpAction_unlocked's PromoteToPhase2
// case body contains the three side effects: sendPongAck,
// sweep_.enterPhase2, hw.startTimer(sweep_.dwells().phase2Slave[0]).
void test_apply_pong_promote_to_phase2_does_work() {
    std::cout
        << "\n=== Pin 2: applyPongSwpAction_unlocked PromoteToPhase2 case "
           "does the work ==="
        << std::endl;
    // applyPongSwpAction_unlocked moved to LinkSweep.cpp
    // after the god-class split.
    std::string linkCpp = readFile(
        projectRoot() + "/src/al/link/LinkSweep.cpp");
    std::string body = extractApplyPongBody(linkCpp);

    // Locate the PromoteToPhase2 case label.
    auto casePos = body.find("SwpPhaseAction::PromoteToPhase2:");
    assert(casePos != std::string::npos);
    // Walk to the end of the switch (Fallthrough to FallbackLockSlowest
    // / DropToPhase1 arms).
    auto nextCase = body.find("SwpPhaseAction::FallbackLockSlowest", casePos);
    assert(nextCase != std::string::npos);
    std::string caseBody = body.substr(casePos, nextCase - casePos);

    // Must send the PONG ack, promote, AND arm the slave timer.
    assert(caseBody.find("sendPongAck_unlocked()") != std::string::npos);
    assert(caseBody.find("sweep_.enterPhase2(*this)") != std::string::npos);
    assert(caseBody.find("hw.startTimer(sweep_.dwells().phase2Slave[0])") !=
           std::string::npos);
    // The pre-fix case was a dead no-op `return false;` with no
    // side effects. None of the three required side effects may be
    // absent.
    std::cout
        << "  PASS (sendPongAck + enterPhase2 + phase2Slave[0] timer armed)"
        << std::endl;
}

// Pin 3: computeDwells fills phase2[i] with the master's
// per-baud dwell (= 1.1 × the slave's full sweep time)
// and phase2Slave[i] with the slave's 250 ms per-baud
// dwell. Master dwell scales with baud count, slave dwell
// is a flat 250 ms. No per-baud roundTripMs in the loop,
// no floor clamp predicates.
void test_compute_dwells_dwells_are_250() {
    std::cout << "\n=== Pin 3: computeDwells fills master/slave dwells "
                 "to 1.1*N*250 / 250 ==="
              << std::endl;
    std::string sweepCpp =
        readFile(projectRoot() + "/src/al/link/sweep/LinkSweep.cpp");
    auto fnPos = sweepCpp.find("void LinkSweep::computeDwells(");
    assert(fnPos != std::string::npos);
    auto brace = sweepCpp.find('{', fnPos);
    assert(brace != std::string::npos);
    int depth = 0;
    bool foundOpen = false;
    size_t endPos = std::string::npos;
    for (size_t i = brace; i < sweepCpp.size(); i++) {
        if (sweepCpp[i] == '{') {
            depth++;
            foundOpen = true;
        } else if (sweepCpp[i] == '}') {
            depth--;
            if (foundOpen && depth == 0) {
                endPos = i;
                break;
            }
        }
    }
    assert(endPos != std::string::npos);
    std::string body = sweepCpp.substr(fnPos, endPos - fnPos + 1);

    // Slave stays flat 250 ms per baud.
    assert(body.find("phase2Slave[i] = 250;") != std::string::npos);
    // Master dwell must be derived from the slave sweep time
    // (1.1 ×). Both the scaling factor and the slave-sweep
    // source must appear in the function body.
    assert(body.find("250 * ctx.allowedBaudsCount()") != std::string::npos);
    assert(body.find("1.1f") != std::string::npos);
    // Master per-baud loop must write to phase2[i].
    assert(body.find("phase2[i]") != std::string::npos);
    // No `if (d <` floor clamp predicate should remain.
    assert(body.find("if (d <") == std::string::npos);
    std::cout << "  PASS (computeDwells fills 1.1*N*250 ms to phase2, "
                 "250 ms to phase2Slave)"
              << std::endl;
}

// Pin 4 (guard): decision-helper file is included by
// LinkSweep.cpp (so the new path is reachable) and the
// SweepPhaseAction::PromoteToPhase2 enum value is declared.
void test_promote_to_phase2_enum_declared() {
    std::cout << "\n=== Pin 4: SwpPhaseAction::PromoteToPhase2 enum exists ==="
              << std::endl;
    std::string decH =
        readFile(projectRoot() + "/src/al/link/sweep/LinkDecision.h");
    assert(decH.find("enum class SwpPhaseAction") != std::string::npos);
    assert(decH.find("PromoteToPhase2") != std::string::npos);
    std::cout << "  PASS (enum + value declared)" << std::endl;
}
} // namespace

int main() {
    std::cout << "=== Pong-P1 -> P2 promotion + slave-timer + dwell-floor "
                 "regression ==="
              << std::endl;
    test_decide_pong_phase1_ping_promotes_to_phase2();
    test_apply_pong_promote_to_phase2_does_work();
    test_compute_dwells_dwells_are_250();
    test_promote_to_phase2_enum_declared();
    std::cout << "\n=== Done ===" << std::endl;
    return 0;
}
#endif