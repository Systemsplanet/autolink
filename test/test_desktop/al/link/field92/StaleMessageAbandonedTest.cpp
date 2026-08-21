// AL92-3: recvMsg had no bound on how long a
// partial message may sit "in progress" waiting
// for appBufAvailable() to reach msgRx_.len(). A
// sender that abandons a message mid-stream
// (AL91-1's SYNC mid-loop abort, or any ASYNC
// path that simply stops producing chunks) left
// the receiver parked in that partial state
// indefinitely — the NEXT legitimate message's
// header and early payload bytes get spliced onto
// the old length claim as filler. The completion
// CRC check catches the resulting garbage (this is
// not a silent-corruption risk), but the
// spliced-onto message is lost. msgRxStartedMs_
// (stamped on beginMsg, checked at the top of
// recvMsg, cleared at every msgRx_.reset() site)
// bounds this: a partial older than
// 2*syncAckTimeoutMs is abandoned and the receiver
// resyncs instead of waiting on it forever.
//
// Structural pin (behavioural test deferred): a
// full behavioural test needs to get msgRx_ into
// an in-progress state through the real header +
// chunk pipeline (ARQ, COBS framing) and then
// simulate the sender going silent for longer than
// the stale limit while continuing to pump the
// clock — the same rig SyncDrainTxRingWithLockDropTest
// / SyncMidLoopDrainAbortTest already flag as
// out-of-scope-for-this-release plumbing (a helper
// thread driving the blocking SYNC ladder, or an
// ASYNC harness that can suspend mid-window without
// tripping the receive-only-peer / disc-storm paths
// this same file's neighbors are pinning). This pin
// verifies the four load-bearing pieces are wired
// together correctly: the member exists, it's
// stamped on successful beginMsg, the staleness
// check runs before the normal dispatch (so it can't
// be bypassed), and it's cleared everywhere msgRx_
// itself is reset (so a stale timestamp from an
// abandoned message can never be read against a
// LATER, legitimate in-progress message).
#ifndef ARDUINO

#    include <cassert>
#    include <cstdio>
#    include <fstream>
#    include <iostream>
#    include <sstream>
#    include <string>

static std::string readFile(const std::string &path) {
    std::ifstream f(path);
    if (!f.good())
        return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string projectRoot() {
    std::string base = ".";
    for (int i = 0; i < 8; i++) {
        std::ifstream pf(base + "/AGENTS.md");
        if (pf.good())
            return base;
        base += "/..";
    }
    return ".";
}

static int countOccurrences(const std::string &s, const std::string &needle) {
    int n = 0;
    size_t pos = 0;
    while ((pos = s.find(needle, pos)) != std::string::npos) {
        n++;
        pos += needle.size();
    }
    return n;
}

static void test_StaleMessageAbandonedTest() {
    std::cout << "\n=== AL92-3: a stale partial message is abandoned "
                 "and resynced, not spliced onto ==="
              << std::endl;

    std::string linkH = readFile(projectRoot() + "/src/al/link/Link.h");
    assert(!linkH.empty());
    assert(linkH.find("uint32_t msgRxStartedMs_") != std::string::npos &&
           "Link.h no longer declares msgRxStartedMs_ — the staleness "
           "bound on a partial message has been removed");

    std::string api = readFile(projectRoot() + "/src/al/link/LinkApi.cpp");
    assert(!api.empty());

    // The staleness check must run BEFORE the
    // normal !inMsg()/beginMsg dispatch inside
    // recvMsg — otherwise a stale partial could be
    // read past instead of abandoned.
    auto recvMsgPos = api.find("int Link::recvMsg(");
    assert(recvMsgPos != std::string::npos);
    auto staleCheckPos =
        api.find("msgRxStartedMs_ != 0", recvMsgPos);
    auto dispatchPos = api.find("if (!msgRx_.inMsg())", recvMsgPos);
    assert(staleCheckPos != std::string::npos &&
           dispatchPos != std::string::npos &&
           staleCheckPos < dispatchPos &&
           "the msgRxStartedMs_ staleness check must run before "
           "recvMsg's normal !inMsg()/beginMsg dispatch");

    // The stale-abandon path must actually reset
    // the partial state and attempt a resync scan
    // — not just log and continue.
    auto staleIfPos = api.find("if (staleMs > staleLimitMs)", staleCheckPos);
    assert(staleIfPos != std::string::npos);
    // Bound on the closing "return -1;" of this specific if-block
    // (not a fixed char count from staleCheckPos) — AL97-4 added a
    // multi-line comment ahead of the computation this pin doesn't
    // care about, and a fixed-width window silently stopped
    // reaching the block it's meant to check the moment that
    // comment grew past the window. Bounding on the block's own
    // closing statement is correct regardless of how much
    // unrelated text precedes it.
    auto blockEnd = api.find("return -1;", staleIfPos);
    assert(blockEnd != std::string::npos);
    std::string staleRegion = api.substr(staleIfPos, blockEnd - staleIfPos);
    assert(staleRegion.find("msgRx_.reset()") != std::string::npos &&
           "the stale-abandon path must reset msgRx_, not merely "
           "detect the staleness");
    assert(staleRegion.find("findMsgHeaderResync_unlocked") !=
               std::string::npos &&
           "the stale-abandon path must attempt a resync scan — "
           "otherwise the byte immediately after the abandoned "
           "partial is still misread as a length-prefix continuation");
    assert(staleRegion.find("FrameErrCause::BadHeader") != std::string::npos &&
           "an abandoned partial message is a genuine frame error "
           "and must be counted the same way beginMsg's own "
           "resync-scan failure is (badHeaderErrs / errThreshold)");

    // msgRxStartedMs_ must be cleared everywhere
    // msgRx_.reset() is called, in both LinkApi.cpp
    // and LinkCore.cpp — a clear-site miss would let
    // a stale timestamp from an abandoned message
    // survive into a later, legitimate in-progress
    // message and misfire the staleness check
    // against it.
    std::string core = readFile(projectRoot() + "/src/al/link/LinkCore.cpp");
    assert(!core.empty());
    int resetCount = countOccurrences(api, "msgRx_.reset();") +
        countOccurrences(core, "msgRx_.reset();");
    int clearCount = countOccurrences(api, "msgRxStartedMs_ = 0;") +
        countOccurrences(core, "msgRxStartedMs_ = 0;");
    // One reset (the stale-abandon path itself)
    // self-clears inline right after — already
    // counted above via staleRegion, not here — so
    // clearCount should match resetCount exactly:
    // every reset site pairs with a clear.
    if (clearCount < resetCount) {
        std::cerr << "\nFAIL: " << resetCount << " msgRx_.reset() call(s) "
                  << "but only " << clearCount
                  << " msgRxStartedMs_ = 0 clear(s) — a reset site is "
                     "missing its paired clear, which can leave a stale "
                     "timestamp to misfire the staleness check against "
                     "a later, legitimate in-progress message"
                  << std::endl;
        assert(false);
    }

    std::cout << "  PASS (member present, staleness check precedes the "
                 "normal dispatch, abandon path resets+resyncs+counts "
                 "the error, "
              << resetCount << " reset site(s) all paired with a clear)"
              << std::endl;
}

// AL97-4: the stale-abandon limit above was originally
// 2*syncAckTimeoutMs — a SYNC-shaped constant applied to ASYNC too.
// ASYNC's own GBN retx ladder can legitimately run a single round
// out to maxRetx*baudAwareRtoMs + gbnBackoffCapMs_unlocked(), well
// past the SYNC floor at the defaults, so a message still being
// repaired was abandoned mid-repair. This pin checks the actual
// computation, not just that SOME staleLimitMs exists (the parent
// pin above already covers structure): both floors are computed and
// combined via the larger-wins comparison, not the SYNC floor alone.
// Same source-grep-only shape as the parent pin for the same reason
// (driving a real in-progress msgRx_ through the ARQ/COBS pipeline
// and holding it there across a multi-second simulated stall is the
// same out-of-scope rig SyncMidLoopDrainAbortTest already flags).
static void test_pin_2_stale_limit_covers_gbn_ladder() {
    std::cout << "\n=== AL97-4: recvMsg's stale-abandon limit covers "
                 "the ASYNC GBN retx ladder, not just the SYNC floor ==="
              << std::endl;

    std::string api = readFile(projectRoot() + "/src/al/link/LinkApi.cpp");
    assert(!api.empty());
    auto recvMsgPos = api.find("int Link::recvMsg(");
    assert(recvMsgPos != std::string::npos);
    auto staleCheckPos = api.find("msgRxStartedMs_ != 0", recvMsgPos);
    assert(staleCheckPos != std::string::npos);

    // Bound the search to this staleness block only (up to the
    // matching `if (staleMs > staleLimitMs)` check), so a false
    // match elsewhere in recvMsg can't pass this pin.
    auto staleIfPos = api.find("if (staleMs > staleLimitMs)", staleCheckPos);
    assert(staleIfPos != std::string::npos &&
           "staleMs must be compared against staleLimitMs — a "
           "differently-named or restructured check would silently "
           "invalidate this pin's window");
    std::string block = api.substr(staleCheckPos, staleIfPos - staleCheckPos);

    assert(block.find("cfg.syncAckTimeoutMs") != std::string::npos &&
           "the SYNC-derived floor (2 * syncAckTimeoutMs) must still "
           "be part of the computation — SYNC's own horizon must "
           "not regress");
    assert(block.find("cfg.maxRetx") != std::string::npos &&
           block.find("baudAwareRtoMs_unlocked()") != std::string::npos &&
           block.find("gbnBackoffCapMs_unlocked()") != std::string::npos &&
           "the ASYNC-derived floor (maxRetx * baudAwareRtoMs + "
           "gbnBackoffCapMs) must be computed here — without it, the "
           "stale-abandon limit is the SYNC constant alone and a "
           "message mid-GBN-repair can be abandoned before its own "
           "retx ladder has run out");

    // The two floors must be combined by taking the larger — not a
    // fixed choice of one or the other, and not a sum (which would
    // over-extend the SYNC path's horizon instead of leaving it
    // unaffected, contrary to the fix's stated intent).
    bool combinedByMax =
        (block.find("asyncFloorMs > syncFloorMs ? asyncFloorMs : "
                     "syncFloorMs") != std::string::npos) ||
        (block.find("syncFloorMs > asyncFloorMs ? syncFloorMs : "
                     "asyncFloorMs") != std::string::npos) ||
        (block.find("std::max(") != std::string::npos &&
         block.find("syncFloorMs") != std::string::npos &&
         block.find("asyncFloorMs") != std::string::npos);
    assert(combinedByMax &&
           "the SYNC and ASYNC floors must be combined by taking the "
           "larger of the two — a fixed preference for one or a sum "
           "of both is not the intended fix");

    std::cout << "  PASS (both floors computed, combined by max)"
              << std::endl;
}

int main() {
    test_StaleMessageAbandonedTest();
    test_pin_2_stale_limit_covers_gbn_ladder();
    std::cout << "\nAll StaleMessageAbandoned pins passed." << std::endl;
    return 0;
}

#else
int main() { return 0; }
#endif
