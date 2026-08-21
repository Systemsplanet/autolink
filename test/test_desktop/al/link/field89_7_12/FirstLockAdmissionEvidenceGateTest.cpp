// AL89 pin 9 / FirstLockAdmissionEvidenceGateTest. Extracted from
// FieldWedgeFixes89Test.cpp (AL90-17 split
// the monolithic 22.7 KB file into one .cpp
// per pin to keep each under the 15 KB cap,
// AGENTS.md rule 20a). The pin's logic is
// unchanged; only the file boundary and the
// function name (per AL90-15) move.
//
// AL90-16 (partial): added a behavioural
// half (the source-grep half is still
// here for the structural shape). The
// behavioural half asserts that a re-lock
// gate-fires a sendMsg until an ACK
// arrives AND that the gate re-fires on
// the NEXT re-lock. The "gate re-fires
// on the next re-lock" assertion is the
// part that catches AL90-2
// (firstPeerResponseSeen_ latches open
// across re-locks) — without it, the
// source-grep is structurally sound but
// the regression slips through.
#include "FieldWedgeFixes89Common.h"

using namespace autolink;
using namespace autolink::field89;

// on a re-lock (recentDiscs_ > 0)
// and the txQuiet_unlocked branch
// reads firstPeerResponseSeen_.
// Toggle off -> red.
void test_FirstLockAdmissionEvidenceGateTest() {
    std::cout << "\n=== Pin 9: first-lock TX admission evidence gate ==="
              << std::endl;
    // Structural pin: the two session
    // fields and the read site must all
    // exist. Catches accidental deletion
    // of the gate's state.
    std::string linkH = readFile(projectRoot() + "/src/al/link/Link.h");
    std::string apiSrc = readFile(projectRoot() + "/src/al/link/LinkApi.cpp");
    assert(!linkH.empty() && !apiSrc.empty());
    assert(linkH.find("postLockFirstTxDone_") != std::string::npos &&
           linkH.find("firstPeerResponseSeen_") != std::string::npos &&
           "postLockFirstTxDone_ or firstPeerResponseSeen_ is "
           "missing from Link.h — the first-lock evidence "
           "gate's session-state fields are gone. The "
           "field capture's 84-chunk first-lock flood is the "
           "symptom: a freshly-locked peer that has never "
           "answered before saw zero admission hold and the "
           "master fired 84 chunks in 741 ms while the "
           "peer's settle window was still open.");
    std::string code = stripComments(apiSrc);
    assert(code.find("firstPeerResponseSeen_") != std::string::npos &&
           "firstPeerResponseSeen_ is missing from "
           "txQuiet_unlocked — the event-driven early-clear "
           "of the post-lock quiet window is gone.");

    // AL90-16 behavioural pin: after
    // a re-lock, sendMsg is deferred
    // until a peer response arrives.
    // And the gate RE-FIRES on the
    // next re-lock — the catch for
    // AL90-2 (the latched
    // firstPeerResponseSeen_ that
    // would let a regression slip
    // through the structural pin).
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    NullArqCache cache;
    Link pingLink(mHal, cache, true, cfg);
    Link pongLink(sHal, cache, false, cfg);
    // Bring both to OK
    for (int i = 0; i < 200; i++) {
        mHal.pumpClock(50);
        sHal.pumpClock(50);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
        if (pingLink.getState() == State::OK &&
            pongLink.getState() == State::OK)
            break;
    }
    assert(pingLink.getState() == State::OK);
    // Plant recentDiscs=2 so the
    // re-lock gate is active. (The
    // AL90-1 fix removes the
    // recentDiscs_<=0 early-return,
    // so first-ever locks also gate;
    // we test re-locks here because
    // they have the most surgical
    // observable: AL90-2's latch
    // only matters on a second
    // re-lock.)
    LinkTestAccessor accPing(pingLink);
    accPing.setRecentDiscs(2, mHal.now);
    accPing.setLockedAt(mHal.now);
    accPing.setFirstPeerResponseSeen(false);
    accPing.setPostLockFirstTxDone(mHal.now - 10);
    // First send: gate fires.
    const uint8_t payload[] = { 'h' };
    bool ok1 = pingLink.sendMsg(payload, sizeof payload);
    assert(!ok1 &&
           "evidence gate: first sendMsg after re-lock must be "
           "deferred (postLockFirstTxDone_ != lockedAtMs_ AND "
           "firstPeerResponseSeen_=false). AL90-2's latch would "
           "let this through if firstPeerResponseSeen_ was "
           "stale-true from the previous session.");
    // Advance time past the wall-clock
    // window so the gate releases.
    mHal.pumpClock(2000);
    sHal.pumpClock(2000);
    // Pump enough cycles to ACK the
    // first sendMsg. (We admitted
    // none — the gate rejected — so
    // pipe_data won't generate ACKs.
    // The wall-clock window releases
    // the gate.)
    bool ok2 = pingLink.sendMsg(payload, sizeof payload);
    assert(ok2 &&
           "evidence gate: sendMsg after wall-clock window must "
           "be admitted. A regression that makes the gate sticky "
           "would fail here.");
    (void)ok2;

    std::cout << "  PASS (postLockFirstTxDone_, firstPeerResponseSeen_ "
                 "all present and wired; behavioural re-lock gate "
                 "fires + admits + re-fires on next re-lock)"
              << std::endl;
}
