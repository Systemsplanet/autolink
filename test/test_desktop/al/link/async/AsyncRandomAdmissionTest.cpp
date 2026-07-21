// Regression pins for the ASYNC-random death spiral: SYNC (both
// fills) and ASYNC-sequential admit cleanly; ASYNC-random drops the
// link.
//
// Root cause: Ping's RANDOM fill drew message sizes up to the full
// maxMsg (5120 B = 22 chunks). A 22-chunk message cannot be admitted
// while a realistically-loaded pipeline holds inflight chunks
// (22 + inflight > window=32). sendMsg rejects the whole message,
// the app cools down, and the still-stuck GBN base spins
// whole-window verbatim retransmits until maxRetx drops the link.
// SEQUENTIAL avoids it (sizes ramp from 1, small early) and SYNC
// avoids it (one frame in flight, inflight always 0).
//
// Fix: bound the RANDOM draw to maxLenForChunkBudget(window/2) so
// every random message stays co-admittable with a half-full window,
// AND in a second pass () clamp the draw to
// the live free window (maxLenForFreeWindow(window, arqPendingCount))
// so the per-call draw can never exceed the live free slot count.
// The send loop also gates admission on the live free window —
// stops admitting once the window can't hold a minimum viable
// message — so a MAX_TX_PER_LOOP=16 burst can never fill the
// pipeline past the live free slot count before the next loop
// iteration sees the new occupancy.
//
// Six pins:
//   1. maxLenForChunkBudget math: budget→len is the inverse of
//      chunksForMsgLen; a message at the returned len fits `budget`
//      slots and one byte more does not.
//   2. A message sized at the RANDOM ceiling co-admits with a
//      half-loaded window (inflight=16 + chunks<=16 <= 32); the
//      old uncapped full-maxMsg draw does NOT (inflight=16 +
//      22 > 32 → reject). Runtime, via the real ArqCache window.
//   3. Source pin: Ping's RANDOM_MAX_BYTES is
//      maxLenForChunkBudget(AUTOLINK_ARQ_PIPELINE_WINDOW / 2), and
//      pickMsgSize_ clamps the random span to it. Revert the cap
//      (RANDOM_MAX_BYTES back to maxSeqSize_) → red.
//   4. Helper is a fixed function of the wire constants, not the
//      window: maxLenForChunkBudget(1) is the single-frame merged
//      ceiling (MAX_CHUNK - MSG_HDR).
//   5. Live free-window helper math: maxLenForFreeWindow(window,
//      inflight) returns the largest message length whose chunk
//      count fits (window - inflight) GBN slots, floored at 1 so
//      the draw never drops to zero. Reverts to red if the helper
//      stops clamping to the live free window.
//   6. Source pin: Ping::loop's send loop holds a burst gate that
//      breaks out when the live free window drops below 1, so a
//      MAX_TX_PER_LOOP burst can never fill the pipeline past
//      the live free slot count. Revert the gate (drop the
//      `if (free_ < 1) break;` check) → red.
#ifndef AUTOLINK_HOST_TEST
#    error "Build with -DAUTOLINK_HOST_TEST (see Makefile)"
#endif

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <cassert>
#include "MockHal.h"
#include "LinkTestAccessor.h"
#include "al/AutoLinkConfig.h"
#include "al/link/Link.h"
#include "al/link/arq/ArqCache.h"
#include "al/util/Log.h"

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

struct WindowDriver {
    MockHal hal;
    ArqCache cache{ AUTOLINK_ARQ_PIPELINE_WINDOW };
    AutoLinkConfig cfg;
    Link link;

    WindowDriver() : link(hal, cache, true, cfg) {
        assert(link.getState() == State::OK);
    }
};

void test_maxLenForChunkBudget_inverts_chunks() {
    std::cout << "\n=== Pin 1: maxLenForChunkBudget inverts chunksForMsgLen ==="
              << std::endl;
    for (int budget = 1; budget <= AUTOLINK_ARQ_PIPELINE_WINDOW; budget++) {
        int len = maxLenForChunkBudget(budget);
        assert(len > 0);
        assert(chunksForMsgLen(len) <= budget &&
               "message at the budget ceiling must fit the budget");
        assert(chunksForMsgLen(len + 1) > budget &&
               "one byte past the ceiling must overflow the budget");
    }
    assert(maxLenForChunkBudget(0) == 0);
    assert(maxLenForChunkBudget(-3) == 0);
    std::cout << "  PASS (budget 1.." << AUTOLINK_ARQ_PIPELINE_WINDOW
              << " each round-trips through chunksForMsgLen)" << std::endl;
}

void test_random_ceiling_coadmits_full_maxmsg_does_not() {
    std::cout << "\n=== Pin 2: RANDOM ceiling co-admits a half-full window; "
                 "full maxMsg does not ==="
              << std::endl;
    constexpr int kHalf = AUTOLINK_ARQ_PIPELINE_WINDOW / 2;
    const int ceilingLen = maxLenForChunkBudget(kHalf);

    // A message at the RANDOM ceiling needs <= window/2 chunks, so it
    // co-admits with a half-loaded pipeline.
    {
        WindowDriver d;
        LinkTestAccessor acc(d.link);
        for (uint8_t s = 0; s < kHalf; s++)
            acc.markAckedPending(s);
        std::string buf((size_t)ceilingLen, 'x');
        uint8_t seq = 0xFF;
        bool ok = d.link.sendMsg((const uint8_t *)buf.data(), ceilingLen, &seq);
        assert(ok &&
               "ceiling-sized message must admit with a half-full window");
    }

    {
        WindowDriver d;
        LinkTestAccessor acc(d.link);
        for (uint8_t s = 0; s < kHalf; s++)
            acc.markAckedPending(s);
        const int fullLen = (int)AUTOLINK_DEFAULT_MAX_MSG;
        assert(chunksForMsgLen(fullLen) + kHalf >
                   AUTOLINK_ARQ_PIPELINE_WINDOW &&
               "full-maxMsg draw must overflow a half-loaded window");
        std::string buf((size_t)fullLen, 'x');
        uint8_t seq = 0xFF;
        bool ok = d.link.sendMsg((const uint8_t *)buf.data(), fullLen, &seq);
        assert(!ok &&
               "full-maxMsg message must be rejected with a half-full window "
               "(this is the spiral the RANDOM cap avoids)");
    }
    std::cout << "  PASS (ceiling len=" << ceilingLen
              << " chunks=" << chunksForMsgLen(ceilingLen)
              << " admits; maxMsg=" << (int)AUTOLINK_DEFAULT_MAX_MSG
              << " chunks=" << chunksForMsgLen((int)AUTOLINK_DEFAULT_MAX_MSG)
              << " rejected at inflight=" << kHalf << ")" << std::endl;
}

void test_ping_random_cap_source_pin() {
    std::cout << "\n=== Pin 3: Ping RANDOM_MAX_BYTES uses the half-window "
                 "helper and pickMsgSize_ clamps to it ==="
              << std::endl;
    std::string src = readFile(projectRoot() + "/src/al/pingpong/Ping.h");
    assert(!src.empty() && "Ping.h must be readable");
    assert(src.find("RANDOM_MAX_BYTES =") != std::string::npos &&
           "Ping must define a RANDOM_MAX_BYTES ceiling");
    assert(src.find("maxLenForChunkBudget(AUTOLINK_ARQ_PIPELINE_WINDOW / 2)") !=
               std::string::npos &&
           "RANDOM_MAX_BYTES must be the half-window chunk budget");
    // pickMsgSize_ must clamp the random span to the ceiling, not to
    // maxSeqSize_ alone (the reverted bug).
    assert(src.find("if (RANDOM_MAX_BYTES < cap)") != std::string::npos &&
           "pickMsgSize_ must clamp the random span to RANDOM_MAX_BYTES");
    std::cout << "  PASS (ceiling constant + span clamp present)" << std::endl;
}

void test_helper_single_frame_ceiling() {
    std::cout << "\n=== Pin 4: budget=1 yields the merged single-frame ceiling "
                 "==="
              << std::endl;
    // A one-slot budget can only carry the merged hdr+payload frame:
    // MAX_CHUNK - MSG_HDR payload bytes.
    assert(maxLenForChunkBudget(1) == MAX_CHUNK - MSG_HDR);
    assert(chunksForMsgLen(maxLenForChunkBudget(1)) == 1);
    std::cout << "  PASS (budget=1 -> " << (MAX_CHUNK - MSG_HDR) << " bytes)"
              << std::endl;
}

void test_maxLenForFreeWindow_clamps_to_live_free() {
    std::cout << "\n=== Pin 5: maxLenForFreeWindow clamps to live free window "
                 "==="
              << std::endl;
    constexpr int kWindow = AUTOLINK_ARQ_PIPELINE_WINDOW;

    // Empty pipeline: live free = window; draw can use the full
    // chunk budget.
    assert(maxLenForFreeWindow(kWindow, 0) == maxLenForChunkBudget(kWindow));

    // Half-loaded: live free = window/2; draw caps at the
    // half-window chunk budget (the same number Pin 3's static
    // ceiling is computed from).
    {
        int free_ = kWindow / 2;
        int got = maxLenForFreeWindow(kWindow, free_);
        assert(got == maxLenForChunkBudget(free_) &&
               "half-loaded pipeline must clamp to the half-window budget");
        // And the chunk count for that length is exactly free_ (or
        // 1 when free_==1), which is what the live ceiling promises.
        assert(chunksForMsgLen(got) <= free_);
    }

    // Over-loaded: live free = 1; draw caps at the merged
    // single-frame ceiling. The floor ensures pickMsgSize_ still
    // returns a viable one-chunk frame (never zero), so the
    // RANDOM draw is never stuck returning 0.
    {
        int got = maxLenForFreeWindow(kWindow, kWindow + 5);
        assert(got == MAX_CHUNK - MSG_HDR);
    }
    // Defensive: negative inflight, zero/negative window — all
    // floor to the single-frame ceiling.
    assert(maxLenForFreeWindow(kWindow, -3) == MAX_CHUNK - MSG_HDR);
    assert(maxLenForFreeWindow(0, 0) == MAX_CHUNK - MSG_HDR);
    assert(maxLenForFreeWindow(-1, 5) == MAX_CHUNK - MSG_HDR);

    // Inversion: a message at the returned length fits the live
    // free window (chunks <= window - inflight) and one byte more
    // does not. Spot-check at a few inflight values around the
    // half-loaded inflection.
    for (int inflight : { 0, 4, 8, 16, 24, 30, 31 }) {
        int len = maxLenForFreeWindow(kWindow, inflight);
        assert(len > 0);
        int free_ = kWindow - inflight;
        if (free_ < 1)
            free_ = 1;
        assert(chunksForMsgLen(len) <= free_ &&
               "returned length must fit the live free window");
        assert(chunksForMsgLen(len + 1) > free_ &&
               "one byte past the live ceiling must overflow the free window");
    }
    std::cout << "  PASS (live free window correctly clamped across "
              << "inflight 0..35, -3, etc.)" << std::endl;
}

// Pull the body of a function out of source for source-level pins
// that need to verify a sub-clause is inside the function body in
// the right order. Naive brace-counting; the functions pinned here
// (Ping::loop, pickMsgSize_) don't have braces inside string
// literals, so a simple counter is enough.
std::string extractFnBody(const std::string &src, const std::string &name) {
    auto start = src.find(name);
    if (start == std::string::npos)
        return "";
    auto bodyStart = src.find('{', start);
    if (bodyStart == std::string::npos)
        return "";
    int depth = 0;
    for (size_t i = bodyStart; i < src.size(); i++) {
        if (src[i] == '{')
            depth++;
        else if (src[i] == '}') {
            depth--;
            if (depth == 0)
                return src.substr(start, i + 1 - start);
        }
    }
    return "";
}

void test_ping_loop_burst_gate_source_pin() {
    std::cout << "\n=== Pin 6: Ping::loop send loop holds a live free-window "
                 "burst gate ==="
              << std::endl;
    std::string src = readFile(projectRoot() + "/src/al/pingpong/Ping.h");
    assert(!src.empty() && "Ping.h must be readable");

    // The send loop must break out when the live free window
    // drops below 1. A burst-only gate (count_ < WINDOW alone)
    // would let a MAX_TX_PER_LOOP=16 burst fill the pipeline
    // past 32 before the next loop sees the new occupancy —
    // that's broken (static half-window ceiling only)
    // death-spiral seed for any pipeline state where inflight
    // is already > 16.
    //
    // The gate must read arqPendingCount() and compute
    // WINDOW - arqPendingCount(). Revert the gate (drop the
    // `if (free_ < 1) break;` check) → red.
    assert(src.find("int free_ = WINDOW - base_.comm_.arqPendingCount();") !=
               std::string::npos &&
           "send loop must compute the live free window from arqPendingCount");
    assert(src.find("if (free_ < 1) {") != std::string::npos &&
           "send loop must break out when the live free window is full");

    // The gate must be inside the while-loop body, BEFORE the
    // pickMsgSize_/sendMsg path (it can't be a post-admit check
    // — that's the bug class the gate closes).
    auto loopBody = extractFnBody(src, "void loop()");
    assert(!loopBody.empty());
    auto burstPos =
        loopBody.find("int free_ = WINDOW - base_.comm_.arqPendingCount();");
    auto pickPos = loopBody.find("int n = pickMsgSize_(");
    assert(burstPos != std::string::npos && pickPos != std::string::npos);
    assert(burstPos < pickPos &&
           "burst gate must precede the pickMsgSize_/sendMsg path");

    std::cout << "  PASS (live free-window burst gate precedes the draw)"
              << std::endl;
}

} // namespace

int main() {
    test_maxLenForChunkBudget_inverts_chunks();
    test_random_ceiling_coadmits_full_maxmsg_does_not();
    test_ping_random_cap_source_pin();
    test_helper_single_frame_ceiling();
    test_maxLenForFreeWindow_clamps_to_live_free();
    test_ping_loop_burst_gate_source_pin();
    std::cout << "\nAll AsyncRandomAdmission pins passed." << std::endl;
    return 0;
}
