// Field failure this pins: a session that never disconnected
// (disc=0) stalled the Ping app four times for 5001 ms each, always
// on seq=189, over 23 s — long after seq 189 had been delivered and
// acked. Two defects combined:
//
//   1. lastNakSeq_ was released only by reset_unlocked, so with no
//      disconnect it held 189 for the whole session.
//   2. decideGapTransition resumed only on lastAck == currentGap.
//      lastAckSeq_ advances thousands of times a second and
//      Ping::loop samples it at loop rate, so Resume was a
//      sampling race it effectively always lost — the field log
//      contains zero "gap resumed" lines.
//
// With the latch stuck and Resume unreachable, every cobsSeq wrap
// back to 189 made the slot briefly pending again and re-entered
// gap-stop. Pin 1 covers the latch release, Pin 2 the pending
// predicate, Pin 3 the wrap that turned the pair into a repeating
// stall.
#ifndef AUTOLINK_HOST_TEST
#    error "Build with -DAUTOLINK_HOST_TEST (see Makefile)"
#endif

#include <iostream>
#include <cassert>
#include "MockHal.h"
#include "NullArqCache.h"
#include "LinkTestAccessor.h"
#include "al/link/Link.h"
#include "al/link/arq/LinkArq.h"
#include "al/pingpong/PingGap.h"
#include "al/AutoLinkConfig.h"

using namespace autolink;

namespace {

AutoLinkConfig makeCfg() {
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 512000;
    cfg.syncAckTimeoutMs = 500;
    cfg.maxRetx = 5;
    cfg.idleTimeoutMs = 100000;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    return cfg;
}

const uint8_t kSeq = 189;

// Pin 1: an ACK for the NAKed seq releases lastNakSeq_, with no
// link reset involved.
void test_ack_releases_nak_latch() {
    std::cout << "\n=== Pin 1: ACK of the NAKed seq releases lastNakSeq_ ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg = makeCfg();
    MockHal hal;
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor acc(link);
    acc.forceState(State::OK);

    hal.lock();
    acc.arq().setGbnBase(kSeq);
    acc.arq().setGbnActive(true);
    acc.arq().onSent(kSeq, kSeq, hal.now);
    hal.unlock();

    acc.onNak(kSeq);
    assert(link.lastNakSeq() == kSeq &&
           "a NAK for a pending seq must latch lastNakSeq_");

    acc.driveOnAck(kSeq, 64);

    std::cout << "  after ACK: lastNakSeq=" << (int)link.lastNakSeq()
              << " disc=" << (unsigned long long)acc.getDiagCountForTest()
              << std::endl;
    assert(link.lastNakSeq() == PING_GAP_NO_GAP &&
           "the ACK must release the latch — leaving it set is what "
           "held seq=189 for an entire disc=0 session");
    assert(acc.getDiagCountForTest() == 0 &&
           "no reset may be involved; reset_unlocked was the only "
           "release path before this fix");
    std::cout << "  PASS" << std::endl;
}

// Pin 2: Resume is driven by the pending predicate, not by an
// exact lastAck match the caller has to sample at the right
// instant.
void test_resume_uses_pending_not_lastack_equality() {
    std::cout
        << "\n=== Pin 2: Resume keys off pending, not lastAck equality ==="
        << std::endl;
    uint8_t next = 0;
    // Gap latched on 189; the seq is no longer pending. Resume must
    // fire regardless of what the most recent ACK happened to name.
    GapAction a = decideGapTransition(kSeq, kSeq, /*gapPending=*/false, next);
    assert(a == GapAction::Resume && next == PING_GAP_NO_GAP &&
           "a delivered gap seq must resume even when lastAck has "
           "long since moved past it");

    // Still pending -> stay stopped.
    a = decideGapTransition(kSeq, kSeq, /*gapPending=*/true, next);
    assert(a == GapAction::Stay && next == kSeq);

    // No gap latched and the NAKed seq already delivered -> never
    // enter. This is the wrap case's guard.
    a = decideGapTransition(PING_GAP_NO_GAP, kSeq, /*gapPending=*/false, next);
    assert(a == GapAction::Stay && next == PING_GAP_NO_GAP &&
           "entering gap-stop on an already-delivered seq is the "
           "stall signature");
    std::cout << "  PASS" << std::endl;
}

// Pin 3: the field shape end to end — NAK, deliver, then run
// cobsSeq all the way around so the same numeric seq is pending
// again. The stale latch must not survive to re-trigger.
void test_seq_wrap_does_not_reenter_gap_stop() {
    std::cout << "\n=== Pin 3: cobsSeq wrap back to the old NAK seq does not "
                 "re-enter gap-stop ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg = makeCfg();
    MockHal hal;
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor acc(link);
    acc.forceState(State::OK);

    hal.lock();
    acc.arq().setGbnBase(kSeq);
    acc.arq().setGbnActive(true);
    acc.arq().onSent(kSeq, kSeq, hal.now);
    hal.unlock();
    acc.onNak(kSeq);
    acc.driveOnAck(kSeq, 64);
    assert(link.lastNakSeq() == PING_GAP_NO_GAP);

    // Wrap all 256 seqs so kSeq is in flight again, exactly as it
    // is 256 messages later on a live stream.
    uint8_t s = (uint8_t)(kSeq + 1);
    for (int i = 0; i < 256; i++) {
        hal.lock();
        acc.arq().setGbnBase(s);
        acc.arq().onSent(s, s, hal.now);
        hal.unlock();
        acc.driveOnAck(s, 64);
        s = (uint8_t)(s + 1);
    }
    hal.lock();
    acc.arq().setGbnBase(kSeq);
    acc.arq().onSent(kSeq, kSeq, hal.now);
    hal.unlock();

    bool pendingAgain = !link.isAcked(kSeq);
    std::cout << "  after wrap: seq=" << (int)kSeq
              << " pending=" << (pendingAgain ? "yes" : "no")
              << " lastNakSeq=" << (int)link.lastNakSeq() << std::endl;
    assert(pendingAgain &&
           "the wrap must actually put kSeq in flight again, or this "
           "pin proves nothing");
    assert(link.lastNakSeq() == PING_GAP_NO_GAP &&
           "no new NAK arrived, so nothing may re-latch the seq — a "
           "surviving latch plus a now-pending slot is exactly the "
           "4x 5 s stall observed in the field");

    uint8_t next = 0;
    GapAction a = decideGapTransition(PING_GAP_NO_GAP, link.lastNakSeq(),
                                      /*gapPending=*/pendingAgain, next);
    assert(a == GapAction::Stay && next == PING_GAP_NO_GAP &&
           "with the latch released the wrap cannot re-enter "
           "gap-stop, even though the slot is pending again");
    std::cout << "  PASS" << std::endl;
}

} // namespace

int main() {
    std::cout << "=== Ping gap-stop latch release ===" << std::endl;
    test_ack_releases_nak_latch();
    test_resume_uses_pending_not_lastack_equality();
    test_seq_wrap_does_not_reenter_gap_stop();
    std::cout << "\nAll PingGapLatch pins passed." << std::endl;
    return 0;
}
