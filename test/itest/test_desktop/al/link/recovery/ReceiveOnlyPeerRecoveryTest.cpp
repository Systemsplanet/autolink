// Fix 4: a receive-only peer (the field's Pong role — its application loop
// only calls recvMsg, never sendMsg) never receives an ACK frame of its own,
// so onAck's recentDiscs_ = 0 never executes. recentDiscs_ had exactly one
// clear site (onAck), so a receive-only peer had no path to it at all — three
// disconnects inside 10 s tripped its own DISC_STORM_THRESHOLD veto and
// forced a full P1 walk down to the slowest baud, at the exact moment the
// sender fast-pathed P3 back to the preserved (fast) baud: a guaranteed baud
// mismatch. Fix: a CRC-valid recvMsg delivery is the same "the locked baud is
// actually good" evidence as an ACK, so it clears recentDiscs_ too
// (LinkApi.cpp), with the same clear added at the valid-NAK site (LinkRx.cpp)
// for symmetry. Pin: two real nodes, sender + receive-only peer, locked once
// via a real negotiation. Three HealthWatchdog-reason disconnects inside 10 s
// (simulated time), each followed by a real reconvergence and one real
// sendMsg/recvMsg round trip (the receive-only peer's only path to clear its
// own counter). Revert either recentDiscs_ = 0 insertion -> red: the receive-
// only peer's recentDiscs_ reaches DISC_STORM_THRESHOLD on the third
// disconnect and it walks away from the fast baud.
#include <iostream>
#include <cassert>
#include "MockHal.h"
#include "NullArqCache.h"
#include "LinkTestAccessor.h"
#include "al/link/Link.h"
#include "al/AutoLinkConfig.h"

using namespace autolink;

namespace {

// Fast baud at index 0, slow baud at index 1 — mirrors the field's
// 512000-vs-9600 shape at itest scale.
static const uint32_t kBauds[] = { 115200, 19200 };
static const int kNumBauds = (int)(sizeof(kBauds) / sizeof(kBauds[0]));

AutoLinkConfig makeCfg() {
    AutoLinkConfig cfg;
    for (int i = 0; i < kNumBauds; i++)
        cfg.allowedBauds[i] = kBauds[i];
    cfg.allowedBaudsCount = kNumBauds;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 20000;
    cfg.syncAckTimeoutMs = 200;
    cfg.postLockQuietMs = 0;
    cfg.maxMsg = 256;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    return cfg;
}

bool pumpUntilOk(Link &a, Link &b, MockHal &mHal, MockHal &sHal, int maxIters) {
    for (int i = 0; i < maxIters; i++) {
        mHal.pumpClock(10);
        sHal.pumpClock(10);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
        if (a.getState() == State::OK && b.getState() == State::OK)
            return true;
    }
    return false;
}

void test_receive_only_peer_fast_paths_through_repeated_disconnects() {
    std::cout << "\n=== Pin: receive-only peer clears recentDiscs_ via "
                 "CRC-valid recv, keeps fast-pathing to the preserved baud "
                 "across 3 disconnects in 10 s ==="
              << std::endl;
    NullArqCache cacheA, cacheB;
    AutoLinkConfig cfg = makeCfg();
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    // sender = master (the field's Ping role), peer = slave (Pong
    // role) — the receive-only side whose recentDiscs_ this fix
    // protects.
    Link sender(mHal, cacheA, /*isMaster=*/true, cfg);
    Link peer(sHal, cacheB, /*isMaster=*/false, cfg);
    sender.begin();
    peer.begin();
    assert(pumpUntilOk(sender, peer, mHal, sHal, 400) &&
           "initial negotiation must reach OK");
    int fastSpdI = peer.getCurrentSpdIndex();
    std::cout << "  initial lock: spdI=" << fastSpdI
              << " (baud=" << kBauds[fastSpdI] << ")" << std::endl;
    LinkTestAccessor accPeer(peer);

    for (int cycle = 1; cycle <= 3; cycle++) {
        // Disconnect: HealthWatchdog, preserve=true — the shape a
        // real drop-and-recover takes. A real physical break hits
        // both ends, so both sides reset; only the receive-only
        // peer's recentDiscs_ is at risk (the sender gets real
        // ACKs from the peer's own onAck path and never needed
        // this fix).
        LinkTestAccessor accSender(sender);
        accSender.resetLink(true, /*preserve=*/true,
                            ResetReason::HealthWatchdog);
        accPeer.resetLink(true, /*preserve=*/true, ResetReason::HealthWatchdog);
        assert(pumpUntilOk(sender, peer, mHal, sHal, 600) &&
               "reconvergence must complete well inside the 10 s disc-"
               "storm window");
        assert(peer.getCurrentSpdIndex() == fastSpdI &&
               "cycle must relock at the preserved (fast) baud, not walk "
               "to the slow one — a receive-only peer with a stuck "
               "recentDiscs_ vetoes exactly this fast path");
        std::cout << "  cycle " << cycle
                  << ": relocked at spdI=" << peer.getCurrentSpdIndex()
                  << " recentDiscs=" << accPeer.recentDiscsForTest()
                  << std::endl;
        assert(accPeer.recentDiscsForTest() <
                   3 /* LinkArq DISC_STORM_THRESHOLD */
               && "recentDiscs_ must stay under the storm threshold — a "
                  "receive-only peer with no clear path accumulates it on "
                  "every disconnect and trips the veto by the 3rd");

        // The receive-only peer's only path to a real CRC-valid
        // delivery: the sender sends, the peer receives. The peer
        // itself never calls sendMsg anywhere in this test.
        const uint8_t payload[] = { 'p', 'i', 'n', 'g', (uint8_t)cycle };
        bool sent = sender.sendMsg(payload, sizeof payload);
        assert(sent && "sendMsg must succeed after a real reconverge");
        int delivered = -1;
        for (int i = 0; i < 200; i++) {
            mHal.pumpClock(10);
            sHal.pumpClock(10);
            pipe_data(mHal, sHal);
            pipe_data(sHal, mHal);
            uint8_t sink[64];
            int n = peer.recvMsg(sink, sizeof sink);
            if (n == (int)sizeof payload) {
                delivered = i;
                break;
            }
        }
        assert(delivered >= 0 &&
               "the message must round-trip — this CRC-valid recv is the "
               "receive-only peer's only clear path for recentDiscs_");
    }
    std::cout << "  PASS (3 disconnects in well under 10 s, every one "
                 "fast-pathed to spdI="
              << fastSpdI << ")" << std::endl;
}

} // namespace

int main() {
    std::cout << "=== Receive-only peer recovery (Fix 4) ===" << std::endl;
    test_receive_only_peer_fast_paths_through_repeated_disconnects();
    std::cout << "\nAll ReceiveOnlyPeerRecovery pins passed." << std::endl;
    return 0;
}
