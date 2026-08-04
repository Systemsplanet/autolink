// The OK-state keepalive: a PING_CMD emitted every idleTimeoutMs/2 from
// onTimerOk_unlocked. Without it, DropSilentPeer (3 * idleTimeoutMs) tears
// down a healthy but duty-cycled link — a sensor reporting once a minute, an
// operator-triggered command link, a paused dashboard.
//
//   Pin 1 (two-node) — survival. Both nodes OK, zero application traffic,
//   4 * idleTimeoutMs of clock: both must stay OK with discCount unchanged.
//
//   Pin 2 (single-node) — wire shape. The keepalive must be the 5-byte CTRL
//   frame, not a COBS ACK. An ACK carries a cobsSeq, and the peer's onAck
//   would walk gbnBase past it.
//
//   Pin 3 (two-node) — ARQ invariant. B's lastRxSeq_ is planted ahead of A's
//   gbnBase (the gapped-receiver shape: onPayload stamps lastRxSeq_ before it
//   classifies the gap, so a dropped out-of-order frame still moves it). B's
//   keepalive then fires. A's gbnBase and pendingCount must not move. An
//   ACK-shaped keepalive would cumulative-ACK chunks B never delivered.

#include <iostream>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>
#include "MockHal.h"
#include "WireSim.h"
#include "NullArqCache.h"
#include "AutoLinkTestAccessor.h"
#include "LinkTestAccessor.h"

using namespace autolink;

static void bringToOk(WireSim &sim, TwoNodeFixture &fix) {
    fix.nodeA().begin();
    fix.nodeB().begin();
    for (int i = 0; i < 300; i++) {
        sim.step(50);
        if (fix.getStateA() == State::OK && fix.getStateB() == State::OK)
            return;
    }
    assert(false && "failed to bring two nodes to OK within 15s");
}

void test_idle_link_survives_4x_idle() {
    std::cout
        << "\n=== Pin 1: idle-but-healthy link survives 4*idleTimeoutMs "
           "(two-node; no app traffic; both stay OK, discCount unchanged) ==="
        << std::endl;
    AutoLinkConfig cfg;
    cfg.idleTimeoutMs = 10000;
    cfg.syncAckTimeoutMs = 1000;
    cfg.maxRetx = 50;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 115200;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    WireSim sim(cfg);
    sim.setFrameDropPct(0);
    TwoNodeFixture fix(sim);
    bringToOk(sim, fix);

    Stats sA0, sB0;
    AutoLinkTestAccessor(sim.linkA()).link()->getStats(sA0);
    AutoLinkTestAccessor(sim.linkB()).link()->getStats(sB0);
    uint32_t discA0 = sA0.discCount;
    uint32_t discB0 = sB0.discCount;

    // The keepalive fires at 5 s; the silent-peer watchdog would fire at
    // 30 s. Four windows is enough to catch it twice.
    const uint32_t step = 1000;
    const uint32_t total = 4 * cfg.idleTimeoutMs;
    for (uint32_t t = 0; t < total; t += step)
        sim.step(step);

    Stats sA1, sB1;
    AutoLinkTestAccessor(sim.linkA()).link()->getStats(sA1);
    AutoLinkTestAccessor(sim.linkB()).link()->getStats(sB1);
    std::cout << "  stateA=" << (int)fix.getStateA()
              << " stateB=" << (int)fix.getStateB()
              << " discA=" << sA1.discCount << " (was " << discA0 << ")"
              << " discB=" << sB1.discCount << " (was " << discB0 << ")"
              << std::endl;
    assert(
        fix.getStateA() == State::OK &&
        "Pin 1: node A must stay OK across 4*idleTimeoutMs (no app traffic) — "
        "the keepalive is the only thing that refreshes lastRxMs in steady state");
    assert(
        fix.getStateB() == State::OK &&
        "Pin 1: node B must stay OK across 4*idleTimeoutMs — the silent-peer "
        "watchdog would have dropped it at 3*idleTimeoutMs without the keepalive");
    assert(sA1.discCount == discA0 &&
           "Pin 1: discCount on A must not increment across 4*idleTimeoutMs");
    assert(sB1.discCount == discB0 &&
           "Pin 1: discCount on B must not increment across 4*idleTimeoutMs");
    std::cout
        << "  Pin 1 PASS (both nodes still OK, discCount unchanged across "
        << total << " ms)" << std::endl;
}

void test_keepalive_emits_ping_ctrl_frame() {
    std::cout << "\n=== Pin 2: keepalive emits a 5-byte CTRL frame "
                 "[0xAA 0x55 cobsSeq PING_CMD crc8], NOT a COBS ACK frame ==="
              << std::endl;
    AutoLinkConfig cfg;
    cfg.idleTimeoutMs = 10000;
    cfg.syncAckTimeoutMs = 1000;
    cfg.maxRetx = 50;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 115200;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    MockHal hal;
    NullArqCache cache;
    Link link(hal, cache, /*isMaster=*/true, cfg);
    link.begin();
    LinkTestAccessor acc(link);
    acc.forceState(State::OK);
    acc.setLastRx(hal.now);
    acc.setLastTx(hal.now);
    hal.clearTx();

    // Pump past the keepalive boundary.
    const uint32_t step = 200;
    const uint32_t target = (uint32_t)cfg.idleTimeoutMs / 2 + 200;
    int txAfter = 0;
    for (uint32_t t = 0; t < target; t += step) {
        hal.pumpClock(step);
        link.onTimer();
        txAfter = (int)hal.txBuf.size();
        if (txAfter >= CTRL_FRAME_SIZE)
            break;
    }
    assert(txAfter >= CTRL_FRAME_SIZE &&
           "Pin 2 pre: keepalive must have emitted at least one "
           "CTRL_FRAME_SIZE-byte frame");

    // An ACK-shaped keepalive emits an 8-byte COBS frame beginning with
    // 0x00, so the size and magic assertions flip red.
    assert(hal.txBuf.size() == CTRL_FRAME_SIZE &&
           "Pin 2: keepalive must emit a 5-byte CTRL frame, NOT a COBS frame "
           "(an ACK-shaped keepalive emits a multi-byte COBS frame and would "
           "fail this assertion)");
    assert(hal.txBuf[0] == 0xAA &&
           "Pin 2: keepalive CTRL frame byte[0] must be 0xAA (the magic "
           "prefix that the OK-side receive path uses to detect CTRL frames)");
    assert(hal.txBuf[1] == 0x55 &&
           "Pin 2: keepalive CTRL frame byte[1] must be 0x55 (the magic "
           "suffix matching the OK-side receive path's CTRL detection)");
    assert(hal.txBuf[CTRL_FRAME_PAYLOAD_IDX] == PING_CMD &&
           "Pin 2: keepalive CTRL frame payload (byte[3]) must be PING_CMD "
           "(0x22) — the keepalive is a PING, not an ACK");
    std::cout << "  Pin 2 PASS (5-byte CTRL frame: "
              << "0x" << std::hex << (int)hal.txBuf[0] << " "
              << "0x" << std::hex << (int)hal.txBuf[1] << " "
              << "0x" << std::hex << (int)hal.txBuf[2] << " "
              << "0x" << std::hex << (int)hal.txBuf[3] << " "
              << "0x" << std::hex << (int)hal.txBuf[4] << std::dec << ")"
              << std::endl;
}

void test_keepalive_does_not_corrupt_sender_arq_under_gap() {
    std::cout << "\n=== Pin 3: keepalive does not corrupt the *sender's* ARQ "
                 "state across a gapped receiver (two-node; B's lastRxSeq_ is "
                 "ahead of A's gbnBase) ==="
              << std::endl;
    AutoLinkConfig cfg;
    cfg.idleTimeoutMs = 2000; // keepalive fires at 1 s
    // RTO=5 s so 2*RTO=10 s > the test's 1.5 s
    // pump window — keeps the DropAsymIdle
    // watchdog (rxAge > 2*rtoMs && txAge < 1000 ms
    // && pending > 0) from tearing down A
    // before B's keepalive can fire.
    cfg.syncAckTimeoutMs = 5000;
    cfg.maxRetx = 50;
    cfg.streamBufferSize = 16384;
    cfg.txBufferSize = 16384;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 115200;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;

    // Raw two-node: plant pending chunks on A and a gapped lastRxSeq_ on B,
    // drain B's TX so the next byte on the wire is the keepalive.
    MockHal mHal, sHal;
    sHal.peer = &mHal;
    mHal.peer = &sHal;
    NullArqCache cacheA, cacheB;
    Link a(mHal, cacheA, /*isMaster=*/true, cfg);
    Link b(sHal, cacheB, /*isMaster=*/false, cfg);
    a.begin();
    b.begin();
    negotiate_to_ok(a, b, mHal, sHal);
    mHal.txBuf.clear();
    sHal.txBuf.clear();
    mHal.txBaudPerByte.clear();
    sHal.txBaudPerByte.clear();

    // Plant pending ARQ state on A. 4 chunks at
    // seq 0..3, gbnBase=0. This is the post-sendMsg
    // state of a 4-chunk ASYNC message; we use the
    // accessor so the test doesn't have to drive a
    // real sendMsg + RTO ladder to get there.
    LinkTestAccessor accA(a);
    LinkTestAccessor accB(b);
    // Plant 4 pending chunks on A (gbnBase=0,
    // pending=4) via the test-only markAckedPending
    // accessor; stamp B's lastRxSeq_ ahead of
    // gbnBase to simulate the gapped-receiver shape.
    for (uint8_t s = 0; s < 4; s++)
        accA.markAckedPending(s);
    // B's lastRxSeq_ is set ahead of gbnBase to
    // simulate the gapped-receiver shape: chunk 0
    // delivered, but B's lastRxSeq_ got stamped on
    // a later chunk's Gap-classified frame.
    accB.setLastRxSeqForTest(3);
    // Stale lastTxMs on B so the keepalive condition
    // (now - lastTxMs > idleTimeoutMs/2) fires after
    // the keepalive window.
    accB.setLastTx(mHal.now);
    accA.setLastTx(mHal.now);
    accA.setLastRx(mHal.now);
    accB.setLastRx(mHal.now);

    uint8_t gbnBaseBefore = accA.arqBaseForTest();
    int pendingBefore = accA.arqPendingCountForTest();
    sHal.clearTx(); // drain any pre-keepalive TX
    std::cout << "  pre-keepalive: A.gbnBase=" << (unsigned)gbnBaseBefore
              << " A.pending=" << pendingBefore
              << " B.lastRxSeq=" << (int)accB.getLastRxSeqForTest()
              << std::endl;
    assert(gbnBaseBefore == 0 &&
           "Pin 3 pre: gbnBase must be 0 after seeding 4 pending chunks");
    assert(pendingBefore == 4 &&
           "Pin 3 pre: pending must be 4 after seeding 4 pending chunks");

    // Pump past B's keepalive boundary and pipe whatever it emits into A.
    const uint32_t step = 200;
    // Pump past B's keepalive boundary (1 s +
    // slack) and past the round-trip time for the
    // keepalive's PING/PONG to land on A.
    const uint32_t target = (uint32_t)cfg.idleTimeoutMs / 2 + 400;
    for (uint32_t t = 0; t < target; t += step) {
        a.onTimer();
        b.onTimer();
        mHal.pumpClock(step);
        sHal.pumpClock(step);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
    }
    uint8_t gbnBaseAfter = accA.arqBaseForTest();
    int pendingAfter = accA.arqPendingCountForTest();
    Stats sa, sb;
    a.getStats(sa);
    b.getStats(sb);
    std::cout << "  post-keepalive: A.gbnBase=" << (unsigned)gbnBaseAfter
              << " A.pending=" << pendingAfter
              << " stateA=" << (int)a.getState()
              << " stateB=" << (int)b.getState() << std::endl;
    assert(gbnBaseAfter == gbnBaseBefore &&
           "Pin 3: keepalive must not advance A's gbnBase — an ACK-shaped "
           "keepalive would carry B's gapped lastRxSeq_=3 and A's onAck "
           "cumulative handler would walk gbnBase 0 -> 4, freeing chunks "
           "B never delivered");
    assert(pendingAfter == pendingBefore &&
           "Pin 3: keepalive must not change A's pendingCount — same "
           "reason as gbnBase; an ACK-shaped keepalive would free the "
           "slots the gapped seq walks past");
    std::cout << "  Pin 3 PASS (A's gbnBase and pendingCount unchanged across "
                 "B's keepalive round-trip on a gapped receiver)"
              << std::endl;
}

int main() {
    std::cout
        << "=== OK-state keepalive (DropSilentPeer false-positive fix) ==="
        << std::endl;
    test_idle_link_survives_4x_idle();
    test_keepalive_emits_ping_ctrl_frame();
    test_keepalive_does_not_corrupt_sender_arq_under_gap();
    std::cout << "\n=== OK-state keepalive tests completed ===" << std::endl;
    return 0;
}
