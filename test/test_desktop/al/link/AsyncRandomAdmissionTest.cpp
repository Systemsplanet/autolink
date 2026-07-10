// Regression pins for the ASYNC-random death spiral in the
// FireBeetle bench logs. SYNC (both fills) and ASYNC-sequential
// worked; ASYNC-random dropped the link three times in a row.
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
// every random message stays co-admittable with a half-full window.
//
// Four pins:
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

    // The pre-fix uncapped draw (full maxMsg = 22 chunks) is rejected
    // by the very same half-loaded window — the death-spiral seed.
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

} // namespace

int main() {
    test_maxLenForChunkBudget_inverts_chunks();
    test_random_ceiling_coadmits_full_maxmsg_does_not();
    test_ping_random_cap_source_pin();
    test_helper_single_frame_ceiling();
    std::cout << "\nAll AsyncRandomAdmission pins passed." << std::endl;
    return 0;
}
