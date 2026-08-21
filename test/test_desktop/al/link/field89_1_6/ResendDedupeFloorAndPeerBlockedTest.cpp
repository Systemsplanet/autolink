// AL89 pin 6 / ResendDedupeFloorAndPeerBlockedTest. AL-D1: converted
// from a source-grep (checking the string "nakCount >
// (uint8_t)cfg.maxRetx" appears in LinkRx.cpp) to a real behavioral
// test that drives Link::onNak directly (the real NAK-dispatch
// function) via a new accessor and counts real wire bytes.
#include "FieldWedgeFixes89Common.h"

using namespace autolink;
using namespace autolink::field89;

// Pin 6 (AL89-6): once a base's NAK count exceeds cfg.maxRetx
// without the base advancing, the resend must suppress entirely —
// a peer answering with NAK after NAK for the same base is holding
// it deliberately (the AL89-5 shape), not losing frames, and
// resending more copies cannot help. Toggle off (drop the
// nakCount > maxRetx gate, resending on every fresh NAK forever)
// -> red: this test counts exactly how many of N onNak calls
// produce a real wire resend and fails if that count exceeds
// maxRetx.
void test_ResendDedupeFloorAndPeerBlockedTest() {
    std::cout << "\n=== Pin 6: dedup floor and peer-blocked suppression ==="
              << std::endl;
    ArqCache pingArq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
    ArqCache pongArq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
    AutoLinkConfig cfg;
    testBaseCfg(cfg);
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    cfg.maxRetx = 3;

    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link ping(mHal, pingArq, true, cfg);
    Link pong(sHal, pongArq, false, cfg);
    ping.begin();
    pong.begin();
    lockPair(ping, pong, mHal, sHal);
    assert(ping.getState() == State::OK);
    mHal.txBuf.clear();

    // Queue an ASYNC message (does not block) so arq_ has a real
    // pending base, then cut the wire so nothing but our own
    // onNakForTest calls ever touch it — no real ACK can clear the
    // base out from under the test.
    uint8_t payload[64];
    for (int i = 0; i < 64; i++)
        payload[i] = (uint8_t)i;
    bool queued = ping.sendMsg(payload, sizeof(payload));
    assert(queued && "ASYNC sendMsg should queue without blocking");
    mHal.peer = nullptr;
    LinkTestAccessor pa(ping);

    // The base seq admitted by sendMsg is txSeq's starting value —
    // NOT observable directly by name, but onNak(missingCobsSeq)
    // only produces a resend when missingCobsSeq matches the
    // current gbnBase(); if the first call produces no wire bytes
    // at all, the harness picked the wrong seq, which is a test
    // bug, not a product one — fail loudly rather than silently
    // passing on an untested path.
    uint8_t baseSeq = pa.gbnBase();

    int resends = 0;
    const int totalNaks = 8; // > cfg.maxRetx (3)
    for (int i = 0; i < totalNaks; i++) {
        // Comfortably clear the baud-derived resend dedup window
        // (AL89-6's OTHER suppression, a short flight-time window)
        // between calls so only the nakCount > maxRetx gate is
        // under test here, not the dedup window.
        mHal.pumpClock(200);
        size_t before = mHal.txBuf.size();
        pa.onNakForTest(baseSeq);
        size_t after = mHal.txBuf.size();
        bool didResend = after > before;
        if (didResend)
            resends++;
        std::cout << "  NAK " << (i + 1) << "/" << totalNaks << ": "
                  << (didResend ? "resend" : "suppressed") << std::endl;
    }

    if (resends == 0) {
        std::cerr << "\nFAIL: zero of " << totalNaks << " onNak calls "
                     "produced a resend — the harness likely has the "
                     "wrong base seq or state; this test isn't exercising "
                     "the path it claims to."
                  << std::endl;
        assert(false);
    }
    if (resends > (int)cfg.maxRetx + 1) {
        std::cerr << "\nFAIL: " << resends << " of " << totalNaks
                  << " NAKs produced a resend — more than "
                     "cfg.maxRetx+1 ("
                  << (int)cfg.maxRetx + 1
                  << "; the gate reads nakCount BEFORE this call's own "
                     "increment, so nakCount=0..maxRetx all still "
                     "resend — maxRetx+1 total — and only nakCount > "
                     "maxRetx, i.e. the (maxRetx+2)th NAK onward, "
                     "suppresses). A peer that keeps NAKing the same "
                     "un-advancing base is blocked, not lossy; resending "
                     "past that is the exact 29-NAKs-per-base storm "
                     "shape from the field capture."
                  << std::endl;
        assert(false);
    }
    std::cout << "  PASS (" << resends << " resend(s) out of " << totalNaks
              << " NAKs, at or under maxRetx+1=" << ((int)cfg.maxRetx + 1)
              << ")" << std::endl;
}
