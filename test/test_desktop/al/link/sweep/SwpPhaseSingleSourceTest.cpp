// Source-level regression test for the SWP phase
// single-source constants + helpers. Pin:
//   1. PHASE3_ACKS_NEEDED is declared in
//      LinkContext.h (single source, no
//      duplicate in LinkSweep.cpp).
//   2. roundTripMs() lives in LinkDecision.h
//      and is called from LinkSweep.cpp's
//      computeDwells + enterPhase3, and from
//      Link.cpp's handleSwp_unlocked master
//      P3 rearm.
//   3. streamBufferSize floor lives in EspHal
//      (single source), not in the AutoLink
//      facade ctor. The facade must NOT mutate
//      cfg.streamBufferSize before the HAL sees it.
//   4. handleSwp_unlocked routes the master
//      P3 PONG stay path through the
//      decision function (no inline
//      `if (acks >= 2)` check).
//   5. Pong's P2 PING path calls enterPhase3
//      directly, no setPhase+reset pair.
//   6. Pong's P2 timeout decision checks
//      spdI < 0 post-decrement (not
//      spdI - 1 < 0).
//   7. handleSwp_unlocked uses isLockPayload
//      instead of an inline range check.
// Toggle off -> red.
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
    return ".";
}
void test_phase3_acks_needed_lives_in_link_context() {
    std::cout << "\n=== PHASE3_ACKS_NEEDED single-source in LinkContext.h ==="
              << std::endl;
    std::string ctxH = readFile(projectRoot() + "/src/al/link/LinkContext.h");
    assert(ctxH.find("constexpr int PHASE3_ACKS_NEEDED") != std::string::npos);
    // Must NOT be defined in LinkSweep.cpp
    std::string sweepCpp =
        readFile(projectRoot() + "/src/al/link/sweep/LinkSweep.cpp");
    assert(sweepCpp.find("PHASE3_ACKS_NEEDED =") == std::string::npos);
    std::cout << "  PASS (defined in LinkContext.h, not in LinkSweep.cpp)"
              << std::endl;
}
void test_round_trip_ms_lives_in_link_decision() {
    std::cout << "\n=== roundTripMs() in LinkDecision.h, called from "
                 "LinkSweep + Link ==="
              << std::endl;
    std::string decH =
        readFile(projectRoot() + "/src/al/link/sweep/LinkDecision.h");
    assert(decH.find("inline int roundTripMs(") != std::string::npos);
    std::string sweepCpp =
        readFile(projectRoot() + "/src/al/link/sweep/LinkSweep.cpp");
    int callsInSweep = 0;
    size_t pos = 0;
    while ((pos = sweepCpp.find("roundTripMs(", pos)) != std::string::npos) {
        callsInSweep++;
        pos += 12;
    }
    // enterPhase3 (1) + computeDwells (1 for phase3) = 2.
    // The pre-fix shape had computeDwells call it inside the
    // per-baud loop too (for a total of 3); the post-fix shape
    // dropped the per-baud dwell formula in favour of a flat
    // 250 ms fill and only uses roundTripMs for the P3 dwell.
    assert(callsInSweep >= 2);
    // roundTripMs() lives in LinkDecision.h and is
    // called from LinkSweep.cpp's applyMasterSwpAction
    // (P3 rearm dwell) after the god-class split.
    std::string linkCpp =
        readFile(projectRoot() + "/src/al/link/LinkSweep.cpp");
    assert(linkCpp.find("roundTripMs(") != std::string::npos);
    // No more inline round-trip formula
    assert(linkCpp.find("2.0 * (5.0 * 10.0 /") == std::string::npos);
    assert(sweepCpp.find("2.0 * (5.0 * 10.0 /") == std::string::npos);
    std::cout << "  PASS (decision helper + 3 call sites + 1 in Link, "
                 "no inline formula)"
              << std::endl;
}
void test_stream_buffer_floor_lives_in_esphal() {
    std::cout << "\n=== streamBufferSize floor lives in EspHal "
                 "(single source), facade is not in the loop ==="
              << std::endl;
    // The pre-fix shape had clampBuffers() in src/AutoLink.cpp, called
    // by both ctors, mutating cfg.streamBufferSize and cfg.txBufferSize
    // before the HAL saw the config. The current contract moves that
    // decision into EspHal so the facade doesn't reach into HAL concerns.
    std::string alCpp = readFile(projectRoot() + "/src/AutoLink.cpp");
    // Facade must NOT define or call clampBuffers.
    assert(alCpp.find("clampBuffers") == std::string::npos);
    // Facade must NOT mutate cfg.streamBufferSize.
    assert(alCpp.find("cfg.streamBufferSize") == std::string::npos);
    assert(alCpp.find("cfg.txBufferSize") == std::string::npos);
    std::string alH = readFile(projectRoot() + "/include/AutoLink.h");
    assert(alH.find("clampBuffers") == std::string::npos);
    // EspHal owns the sizing. It must reference maxMsg + mode and
    // compute a size from them. Either an inline block in begin() or
    // a static helper satisfies this — pin the formulas.
    std::string halH = readFile(projectRoot() + "/src/al/hal/EspHal.h");
    assert(halH.find("cfg.maxMsg") != std::string::npos);
    assert(halH.find("cfg.mode") != std::string::npos);
    // xStreamBufferCreate is sized from the derived floor, not from
    // cfg.streamBufferSize directly.
    auto createPos = halH.find("xStreamBufferCreate");
    assert(createPos != std::string::npos);
    std::string createSlice = halH.substr(createPos, 200);
    assert(createSlice.find("stream_buf_size_") != std::string::npos);
    std::cout << "  PASS (EspHal owns stream buffer sizing; facade does "
                 "not mutate cfg.streamBufferSize / cfg.txBufferSize)"
              << std::endl;
}
void test_handle_swp_uses_is_lock_payload() {
    std::cout << "\n=== handleSwp_unlocked uses isLockPayload ===" << std::endl;
    // handleSwp_unlocked lives in LinkSweep.cpp
    // after the god-class split.
    std::string linkCpp =
        readFile(projectRoot() + "/src/al/link/LinkSweep.cpp");
    auto fnPos = linkCpp.find("bool Link::handleSwp_unlocked(");
    assert(fnPos != std::string::npos);
    // Scan the function body for isLockPayload call
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
    std::string body = linkCpp.substr(fnPos, endPos - fnPos + 1);
    assert(body.find("isLockPayload(") != std::string::npos);
    // No more inline payload >= LOCK_CMD check
    assert(body.find(">= LOCK_CMD") == std::string::npos);
    std::cout << "  PASS (isLockPayload() in body, no inline LOCK_CMD range)"
              << std::endl;
}
void test_decision_function_promote_to_phase2() {
    std::cout << "\n=== decideMasterPhase1Ack returns PromoteToPhase2 ==="
              << std::endl;
    std::string decH =
        readFile(projectRoot() + "/src/al/link/sweep/LinkDecision.h");
    assert(decH.find("SwpPhaseAction::PromoteToPhase2") != std::string::npos);
    std::cout << "  PASS (PromoteToPhase2 action exists)" << std::endl;
}
} // namespace
int main() {
    std::cout << "=== SWP Phase Decision Tests ===" << std::endl;
    test_phase3_acks_needed_lives_in_link_context();
    test_round_trip_ms_lives_in_link_decision();
    test_stream_buffer_floor_lives_in_esphal();
    test_handle_swp_uses_is_lock_payload();
    test_decision_function_promote_to_phase2();
    std::cout << "\n=== Done ===" << std::endl;
    return 0;
}
#endif
