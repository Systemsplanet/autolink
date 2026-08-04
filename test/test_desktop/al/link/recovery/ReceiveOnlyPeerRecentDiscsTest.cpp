// A receive-only peer (Pong: recv()-only loop, never sends) gets
// zero ACK frames back, so onAck's `recentDiscs_ = 0` — the ONLY
// clear site before this fix — never executes on that side.
// recentDiscs_ climbs on every real (wasEverOk_) reset regardless
// of role; three HealthWatchdog resets inside 10 s trips
// DISC_STORM_THRESHOLD and vetoes the receive-only side's own P3
// fast-path, forcing a full P1 walk while its peer (which DOES get
// ACKed and stays cleared) fast-paths P3 at the proven baud —
// guaranteed miss, an outage until the walk re-converges.
//
// Fix: a CRC-valid recvMsg (LinkApi.cpp) and a valid NAK
// (LinkRx.cpp onNak) are the same "link is delivering at the
// locked baud" evidence onAck already uses, so both also clear
// recentDiscs_ now.
//
// Pin 1: a valid NAK clears recentDiscs_ on the side that received
// it (LinkRx.cpp onNak site). Pin 2: a real CRC-valid message
// round-trip clears recentDiscs_ on the receiving side (LinkApi.cpp
// recvMsg site) — the exact mechanism a receive-only peer relies
// on, since it never receives an ACK. Revert either clear -> red.
#ifndef AUTOLINK_HOST_TEST
#    error "Build with -DAUTOLINK_HOST_TEST (see Makefile)"
#endif

#include <iostream>
#include <cassert>
#include "MockHal.h"
#include "NullArqCache.h"
#include "LinkTestAccessor.h"
#include "al/link/Link.h"
#include "al/AutoLinkConfig.h"

using namespace autolink;

namespace {

static const int kBauds[] = { 512000, 256000, 115200, 57600, 9600 };
static const int kNumBauds = 5;

AutoLinkConfig cfgCommon() {
    AutoLinkConfig cfg;
    for (int i = 0; i < kNumBauds; i++)
        cfg.allowedBauds[i] = (uint32_t)kBauds[i];
    cfg.allowedBaudsCount = kNumBauds;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 10000;
    cfg.syncAckTimeoutMs = 1000;
    cfg.maxRetx = 50;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    return cfg;
}

void bringToOk(Link &a, Link &b, MockHal &mHal, MockHal &sHal) {
    a.begin();
    b.begin();
    for (int i = 0; i < 200; i++) {
        mHal.pumpClock(50);
        sHal.pumpClock(50);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
        if (a.getState() == State::OK && b.getState() == State::OK)
            return;
    }
    assert(false && "failed to bring two nodes to OK");
}

// Pin 1: a valid NAK clears recentDiscs_ on the receiving side.
void test_valid_nak_clears_recent_discs() {
    std::cout << "\n=== Pin 1: a valid NAK clears recentDiscs_ (LinkRx.cpp "
                 "onNak site) ==="
              << std::endl;
    NullArqCache cacheA, cacheB;
    AutoLinkConfig cfg = cfgCommon();
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link a(mHal, cacheA, /*isMaster=*/true, cfg);
    Link b(sHal, cacheB, /*isMaster=*/false, cfg);
    bringToOk(a, b, mHal, sHal);
    LinkTestAccessor accA(a);

    // Simulate two disc-storm resets already accumulated on `a`
    // (one shy of DISC_STORM_THRESHOLD=3) — the exact
    // receive-only-peer signature: nothing has cleared it because
    // `a` hasn't received a real ACK since.
    accA.setRecentDiscs(2, mHal.now);
    assert(accA.recentDiscsForTest() == 2);

    // Seed a pending slot so the NAK is a live, valid crossing
    // (not a no-op for an already-acked/out-of-window seq).
    accA.markAckedPending(5);
    bool got = accA.onNak(5);
    (void)got;

    std::cout << "  recentDiscs after valid NAK = "
              << accA.recentDiscsForTest() << std::endl;
    assert(accA.recentDiscsForTest() == 0 &&
           "a valid NAK proves the link is delivering at the locked "
           "baud, same as onAck — it must clear recentDiscs_. "
           "Without this, a peer that only ever NAKs (never gets "
           "ACKed) can never clear its own disc-storm counter.");
    std::cout << "  PASS (valid NAK cleared recentDiscs_)" << std::endl;
}

// Pin 2: a real CRC-valid message round-trip clears recentDiscs_ on
// the receiving side — the mechanism a receive-only peer (never
// sends, only recvMsg's) actually relies on.
void test_recv_msg_clears_recent_discs() {
    std::cout << "\n=== Pin 2: a CRC-valid recvMsg clears recentDiscs_ on "
                 "the receiving side (LinkApi.cpp recvMsg site) ==="
              << std::endl;
    NullArqCache cacheA, cacheB;
    AutoLinkConfig cfg = cfgCommon();
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link a(mHal, cacheA, /*isMaster=*/true, cfg);
    Link b(sHal, cacheB, /*isMaster=*/false, cfg);
    bringToOk(a, b, mHal, sHal);
    LinkTestAccessor accB(b);

    // Two disc-storm resets already accumulated on `b` (the
    // receive-only side in the field scenario) — one shy of
    // DISC_STORM_THRESHOLD=3.
    accB.setRecentDiscs(2, sHal.now);
    assert(accB.recentDiscsForTest() == 2);

    // Pump past postLockQuietMs so the send below isn't deferred.
    for (uint32_t t = 0; t < 1500; t += 50) {
        mHal.pumpClock(50);
        sHal.pumpClock(50);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
    }
    const uint8_t payload[] = { 'h', 'i' };
    bool ok = a.sendMsg(payload, sizeof payload);
    assert(ok && "sendMsg must succeed once postLockQuietMs has elapsed");

    int delivered = -1;
    for (int i = 0; i < 200; i++) {
        mHal.pumpClock(50);
        sHal.pumpClock(50);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
        uint8_t sink[64];
        int n = b.recvMsg(sink, sizeof sink);
        if (n == (int)sizeof payload) {
            delivered = i;
            break;
        }
    }
    assert(delivered >= 0 && "a real message must round-trip to b");

    std::cout << "  recentDiscs after CRC-valid recv = "
              << accB.recentDiscsForTest() << std::endl;
    assert(accB.recentDiscsForTest() == 0 &&
           "a CRC-valid recvMsg proves b is actually delivering at "
           "the locked baud, same rationale as the existing "
           "locksWithoutRecv_ clear right next to it — it must also "
           "clear recentDiscs_. A receive-only peer (Pong: never "
           "sends, only recvMsg's) has no other path to this clear, "
           "since it never receives an ACK; without this fix its "
           "recentDiscs_ only ever climbs, eventually vetoing its "
           "own P3 fast-path recovery at exactly the baud its "
           "sending peer already proved good.");
    std::cout << "  PASS (CRC-valid recv cleared recentDiscs_ in "
              << delivered << " iterations)" << std::endl;
}

} // namespace

int main() {
    std::cout << "=== Receive-only peer recentDiscs_ clearing "
                 "(disc-storm veto fix) ==="
              << std::endl;
    test_valid_nak_clears_recent_discs();
    test_recv_msg_clears_recent_discs();
    std::cout << "\nAll ReceiveOnlyPeerRecentDiscs pins passed." << std::endl;
    return 0;
}
