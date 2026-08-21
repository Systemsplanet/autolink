// Source-level regression test for AL97-7.
//
// LinkRx.cpp's onAck emitted two Log::log().verbose(...) lines per
// ACK (the "wire ACK seq=..." line and its "GBN base ... ->" 1-per-
// ACK companion), and ArqCache::freeBySeq emitted one
// Log::log().debug(...) line per freed slot (a cumulative-walk ACK
// can free several slots at once, so this can fire MORE than once
// per ACK). All three were already runtime-gated behind the
// verbose/debug log level (AL-14: PostSoakFieldFixesTest), not
// unconditional — but a field session that turns verbose logging ON
// specifically to diagnose a wire-level problem is exactly the
// scenario that needs these lines, and at 512000 baud (~600 ACK/s)
// the resulting 3-line-per-ACK volume overran the 128-entry log
// ring inside ~0.2 s, dropping the state-transition lines around
// the actual failure — a captured field run lost its final 5
// seconds of log this way, right where the link went down.
//
// Fix: gate all three behind the existing compile-time
// AUTOLINK_TRACE_WIRE macro (same opt-in the per-chunk "wire COBS
// ok" trace in onPayload already uses — WireTraceOffByDefaultTest /
// SubsystemLoggingTest cover that one), and add an always-on 1 Hz
// aggregate (acks seen, chunks freed) in onTimerOk_unlocked as the
// field-side replacement — bounded-rate regardless of build flags
// or runtime log level.
//
// All three source files here (LinkRx.cpp, ArqCache.cpp,
// LinkTimersOk.cpp) compile in the host suite, but this pin stays
// source-level rather than instrumenting a full two-node MockHal
// session at ASYNC pipeline rate: the defect and the fix are both
// about which lines exist in which conditional block, not about
// runtime call counts under load, and every other hot-path-logging
// fix in this codebase (SubsystemLoggingTest, WireTraceOffByDefault
// coverage) is pinned the same way.
#ifndef ARDUINO

#    include <cassert>
#    include <fstream>
#    include <iostream>
#    include <sstream>
#    include <string>
#    include "TestPaths.h"

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

// Extract the body of a named function by brace-matching from its
// first '{' — same helper shape used by ResyncScanErrTest /
// EspHalBreakFlushGuardTest for the same reason (a plain substring
// grep can't tell "inside this function" from "inside the next
// one").
std::string functionBody(const std::string &src, const std::string &sig) {
    auto fnPos = src.find(sig);
    if (fnPos == std::string::npos)
        return "";
    auto brace = src.find('{', fnPos);
    if (brace == std::string::npos)
        return "";
    int depth = 0;
    bool opened = false;
    for (size_t i = brace; i < src.size(); i++) {
        if (src[i] == '{') {
            depth++;
            opened = true;
        } else if (src[i] == '}') {
            depth--;
            if (opened && depth == 0)
                return src.substr(fnPos, i - fnPos + 1);
        }
    }
    return "";
}

// Pin 1: onAck's per-ACK verbose line ("wire ACK seq=") is inside
// an #ifdef AUTOLINK_TRACE_WIRE block. Toggle the #ifdef/#endif
// pair out -> this line runs at the runtime verbose level again,
// unconditionally on the build flag -> red.
void test_wire_ack_line_gated() {
    std::cout << "\n=== Pin 1: onAck's per-ACK trace line is gated on "
                 "AUTOLINK_TRACE_WIRE ==="
              << std::endl;
    std::string src = readFile(testRepoPath("src/al/link/io/LinkRx.cpp"));
    assert(!src.empty());
    std::string body = functionBody(src, "bool Link::onAck(");
    assert(!body.empty() && "onAck not found");
    auto ackLinePos = body.find("\"wire ACK seq=%u (base=%s");
    assert(ackLinePos != std::string::npos &&
           "wire ACK trace line missing (note: onAck also has an "
           "unrelated, unguarded \"wire ACK seq=%u ignored in SWP "
           "state\" debug line earlier in the function — this must "
           "match the per-ACK (base=..., pending=..., bytesRecvd=...) "
           "variant specifically, not that one)");
    auto ifdefPos = body.rfind("#ifdef AUTOLINK_TRACE_WIRE", ackLinePos);
    assert(ifdefPos != std::string::npos &&
           "the wire ACK trace line must be preceded by "
           "#ifdef AUTOLINK_TRACE_WIRE within onAck's body");
    auto endifPos = body.find("#endif", ackLinePos);
    assert(endifPos != std::string::npos && endifPos > ackLinePos &&
           "the wire ACK trace line must be followed by a matching "
           "#endif");
    // No unrelated #endif between the #ifdef and the trace line —
    // proves this #ifdef is actually the one guarding this line,
    // not some earlier, already-closed block.
    auto strayEndif = body.find("#endif", ifdefPos);
    assert(strayEndif == endifPos &&
           "the #ifdef immediately before the trace line must be "
           "the one that closes after it, not an earlier block");
    std::cout << "  PASS" << std::endl;
}

// Pin 2: onAck's GBN-base companion line ("GBN base ... ->") is
// ALSO gated on AUTOLINK_TRACE_WIRE, AND the functional GBN-state
// reset that follows it (gbnAttempts_ = 0; etc.) is NOT inside that
// same #ifdef — gating the trace line must not also disable the
// state reset it sits next to. (This is the exact mistake caught
// and corrected while implementing this fix: an early draft wrapped
// both the log call and the reset statements in one #ifdef block.)
void test_gbn_base_line_gated_state_reset_unconditional() {
    std::cout << "\n=== Pin 2: GBN-base trace line gated, state reset "
                 "stays unconditional ==="
              << std::endl;
    std::string src = readFile(testRepoPath("src/al/link/io/LinkRx.cpp"));
    assert(!src.empty());
    std::string body = functionBody(src, "bool Link::onAck(");
    assert(!body.empty());
    auto gbnLinePos = body.find("\"GBN base %u -> %u");
    assert(gbnLinePos != std::string::npos && "GBN base trace line missing");
    auto ifdefPos = body.rfind("#ifdef AUTOLINK_TRACE_WIRE", gbnLinePos);
    assert(ifdefPos != std::string::npos &&
           "the GBN base trace line must be preceded by "
           "#ifdef AUTOLINK_TRACE_WIRE");
    auto endifPos = body.find("#endif", gbnLinePos);
    assert(endifPos != std::string::npos && endifPos > gbnLinePos);
    auto resetPos = body.find("gbnAttempts_ = 0;", gbnLinePos);
    assert(resetPos != std::string::npos && "GBN state reset missing");
    assert(resetPos > endifPos &&
           "gbnAttempts_ = 0 (and the rest of the GBN state reset) "
           "must come AFTER the #endif that closes the trace line's "
           "#ifdef — the reset must run unconditionally, on every "
           "build, not only when AUTOLINK_TRACE_WIRE is defined");
    std::cout << "  PASS" << std::endl;
}

// Pin 3: the wireAckAggAcks_ / wireAckAggFreed_ accumulation in
// onAck happens OUTSIDE any AUTOLINK_TRACE_WIRE block — the
// aggregate must accumulate on every build so the 1 Hz summary has
// real data to report even when the trace macro is off (the common
// case).
void test_aggregate_counters_accumulate_unconditionally() {
    std::cout << "\n=== Pin 3: wire-agg counters accumulate outside the "
                 "trace gate ==="
              << std::endl;
    std::string src = readFile(testRepoPath("src/al/link/io/LinkRx.cpp"));
    assert(!src.empty());
    std::string body = functionBody(src, "bool Link::onAck(");
    assert(!body.empty());
    auto accAcksPos = body.find("wireAckAggAcks_++;");
    assert(accAcksPos != std::string::npos &&
           "wireAckAggAcks_++ missing from onAck");
    auto accFreedPos = body.find("wireAckAggFreed_ +=");
    assert(accFreedPos != std::string::npos &&
           "wireAckAggFreed_ accumulation missing from onAck");
    // Neither accumulation line may sit between an #ifdef
    // AUTOLINK_TRACE_WIRE and its #endif.
    size_t pos = 0;
    while ((pos = body.find("#ifdef AUTOLINK_TRACE_WIRE", pos)) !=
           std::string::npos) {
        auto endifPos = body.find("#endif", pos);
        assert(endifPos != std::string::npos);
        assert(!(accAcksPos > pos && accAcksPos < endifPos) &&
               "wireAckAggAcks_++ must not be inside an "
               "AUTOLINK_TRACE_WIRE block");
        assert(!(accFreedPos > pos && accFreedPos < endifPos) &&
               "wireAckAggFreed_ accumulation must not be inside an "
               "AUTOLINK_TRACE_WIRE block");
        pos = endifPos + 1;
    }
    std::cout << "  PASS" << std::endl;
}

// Pin 4: ArqCache::freeBySeq's per-slot "free seq=" debug line is
// gated on AUTOLINK_TRACE_WIRE.
void test_arqcache_free_line_gated() {
    std::cout << "\n=== Pin 4: ArqCache free-slot debug line is gated on "
                 "AUTOLINK_TRACE_WIRE ==="
              << std::endl;
    std::string src = readFile(testRepoPath("src/al/link/arq/ArqCache.cpp"));
    assert(!src.empty());
    std::string body = functionBody(
        src, "void ArqCache::freeBySeq(uint8_t seq, FreeCause cause)");
    assert(!body.empty() && "ArqCache::freeBySeq(seq, cause) not found");
    auto linePos = body.find("\"free seq=%u pending=%d cause=%s\"");
    assert(linePos != std::string::npos && "free-slot debug line missing");
    auto ifdefPos = body.rfind("#ifdef AUTOLINK_TRACE_WIRE", linePos);
    assert(ifdefPos != std::string::npos &&
           "the free-slot debug line must be preceded by "
           "#ifdef AUTOLINK_TRACE_WIRE");
    auto endifPos = body.find("#endif", linePos);
    assert(endifPos != std::string::npos && endifPos > linePos);
    std::cout << "  PASS" << std::endl;
}

// Pin 5: onTimerOk_unlocked contains the always-on 1 Hz aggregate
// log call, and it is NOT gated behind AUTOLINK_TRACE_WIRE — it
// must run regardless of build flags, since it's the field-side
// replacement for the now-gated per-ACK traces above.
void test_aggregate_log_present_and_unconditional() {
    std::cout << "\n=== Pin 5: 1 Hz wire-activity aggregate log is "
                 "present and unconditional ==="
              << std::endl;
    std::string src =
        readFile(testRepoPath("src/al/link/timers/LinkTimersOk.cpp"));
    assert(!src.empty());
    std::string body = functionBody(src, "bool Link::onTimerOk_unlocked()");
    assert(!body.empty() && "onTimerOk_unlocked not found");
    auto linePos = body.find("\"wire: %llu acks, %llu chunks freed");
    assert(linePos != std::string::npos &&
           "1 Hz wire-activity aggregate log line missing from "
           "onTimerOk_unlocked");
    // Must not sit inside any AUTOLINK_TRACE_WIRE block in this
    // function.
    size_t pos = 0;
    while ((pos = body.find("#ifdef AUTOLINK_TRACE_WIRE", pos)) !=
           std::string::npos) {
        auto endifPos = body.find("#endif", pos);
        assert(endifPos != std::string::npos);
        assert(!(linePos > pos && linePos < endifPos) &&
               "the 1 Hz aggregate log must run unconditionally — it "
               "is the always-on replacement for the gated per-ACK "
               "traces, not another opt-in trace line");
        pos = endifPos + 1;
    }
    // And it must be rate-limited against WIRE_AGG_WINDOW_MS, not a
    // per-tick log (onTimerOk_unlocked can run far more often than
    // once a second).
    assert(body.find("WIRE_AGG_WINDOW_MS") != std::string::npos &&
           "the aggregate log must be gated on WIRE_AGG_WINDOW_MS, "
           "or it floods at the timer-tick rate instead of 1 Hz");
    std::cout << "  PASS" << std::endl;
}

} // namespace

int main() {
    std::cout << "=== Ack path not verbose by default (AL97-7) ==="
              << std::endl;
    test_wire_ack_line_gated();
    test_gbn_base_line_gated_state_reset_unconditional();
    test_aggregate_counters_accumulate_unconditionally();
    test_arqcache_free_line_gated();
    test_aggregate_log_present_and_unconditional();
    std::cout << "\nAll AckPathNotVerboseByDefault pins passed." << std::endl;
    return 0;
}
#endif
