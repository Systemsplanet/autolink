// Field failure this pins (SYNC mode, 512000 baud): the peer NAKed
// the in-flight seq ~6 ms after every retransmission, and the
// sender ignored it and slept the full 500 ms syncAckTimeoutMs
// before resending — four times over. Log timestamps show the
// timeouts landing exactly 500 ms after each retx (13.340 -> 13.840),
// not after the NAK at 13.346, confirming waitForAck runs its own
// deadline and nothing on the SYNC path consumed the NAK. Two
// seconds of dead air later the BREAK storm fired and the link
// walked all the way down to 9600.
//
// Fix: a CRC-valid NAK naming the in-flight seq wakes waitForAck so
// the existing SYNC ladder takes its next step immediately. The
// ladder keeps ownership of SYNC recovery (SYNC never populates the
// ArqCache, so a NAK-driven cache resend would ship a zero-byte
// frame the peer reads as a seq advance).
//
// Pin 1: the wake makes waitForAck return promptly instead of at
// RTO. Pin 2: it is capped at one wake per ladder attempt, so a NAK
// burst cannot spin the ladder through every attempt in
// milliseconds. Pin 3: a NAK for some other seq must not wake it.
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
#include "al/AutoLinkConfig.h"

using namespace autolink;

namespace {

const uint32_t kRto = 500;
const uint8_t kSeq = 64;

AutoLinkConfig makeCfg() {
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 512000;
    cfg.syncAckTimeoutMs = (int)kRto;
    cfg.maxRetx = 5;
    cfg.idleTimeoutMs = 100000;
    cfg.mode = AutoLinkConfig::Mode::SYNC;
    return cfg;
}

// MockHal's waitForAck spin advances no clock on its own, so the
// host build needs the clock stepped from the "peer" side. This
// hal subclass injects a NAK once the wait has been running for
// nakAtMs, mimicking the peer answering mid-wait.
struct NakInjectingHal : MockHal {
    Link *link = nullptr;
    LinkTestAccessor *acc = nullptr;
    uint32_t nakAtMs = 6;
    uint8_t nakSeq = kSeq;
    int nakBurst = 1;
    uint32_t waitStartMs = 0;
    bool armed = false;
    int naksSent = 0;
    bool injecting = false;

    uint32_t nowMs() override {
        // Each observation advances the simulated clock by 1 ms.
        // The wait loop polls nowMs(), so this drives time forward
        // deterministically without a second thread.
        MockHal::now += 1;
        // onNak itself reads nowMs(); without this guard the
        // injection re-enters here and recurses until the stack
        // is gone.
        if (!injecting && armed && naksSent < nakBurst &&
            (uint32_t)(MockHal::now - waitStartMs) >= nakAtMs) {
            injecting = true;
            for (int i = 0; i < nakBurst; i++) {
                acc->onNak(nakSeq);
                naksSent++;
            }
            injecting = false;
        }
        return MockHal::now;
    }
};

void seedInFlight(NakInjectingHal &hal, LinkTestAccessor &acc, uint8_t seq) {
    hal.lock();
    acc.arq().onSent(seq, seq, hal.now);
    hal.unlock();
}

void test_nak_wakes_wait_before_rto() {
    std::cout << "\n=== Pin 1: a NAK for the in-flight seq wakes waitForAck "
                 "well before the RTO ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg = makeCfg();
    NakInjectingHal hal;
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor acc(link);
    acc.forceState(State::OK);
    hal.link = &link;
    hal.acc = &acc;

    seedInFlight(hal, acc, kSeq);
    hal.waitStartMs = hal.now;
    hal.armed = true;

    uint32_t t0 = hal.now;
    hal.lock();
    bool acked = acc.arq().waitForAck(acc.halCtx(), kSeq, kRto);
    hal.unlock();
    uint32_t elapsed = hal.now - t0;

    std::cout << "  acked=" << (acked ? "true" : "false")
              << " elapsed=" << elapsed << " ms (RTO=" << kRto << ")"
              << std::endl;
    assert(!acked && "a NAK is not an ACK — the wait must report failure so "
                     "the ladder retransmits");
    assert(elapsed < kRto / 2 &&
           "the wait must end on the NAK, not run out the full RTO — "
           "sleeping through the peer's explicit 'I am missing this' "
           "is the 2 s of dead air seen in the field");
    std::cout << "  PASS" << std::endl;
}

void test_nak_burst_capped_at_one_wake_per_attempt() {
    std::cout << "\n=== Pin 2: a NAK burst yields one wake, not one per NAK ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg = makeCfg();
    NakInjectingHal hal;
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor acc(link);
    acc.forceState(State::OK);
    hal.link = &link;
    hal.acc = &acc;
    // The field burst: 17 NAKs inside 65 ms.
    hal.nakBurst = 17;

    seedInFlight(hal, acc, kSeq);
    hal.waitStartMs = hal.now;
    hal.armed = true;
    hal.lock();
    bool first = acc.arq().waitForAck(acc.halCtx(), kSeq, kRto);
    hal.unlock();
    assert(!first);
    std::cout << "  burst delivered " << hal.naksSent << " NAKs" << std::endl;
    assert(hal.naksSent == 17 && "the burst must actually have been delivered");

    // Next ladder attempt: the seq goes back in flight and no new
    // NAK arrives. The leftover burst must not short-circuit this
    // wait — it has to run its own full RTO.
    hal.armed = false;
    seedInFlight(hal, acc, kSeq);
    uint32_t t0 = hal.now;
    hal.lock();
    bool second = acc.arq().waitForAck(acc.halCtx(), kSeq, kRto);
    hal.unlock();
    uint32_t elapsed = hal.now - t0;

    std::cout << "  second attempt: acked=" << (second ? "true" : "false")
              << " elapsed=" << elapsed << " ms" << std::endl;
    assert(!second);
    assert(elapsed >= kRto &&
           "stale NAKs from a previous attempt must not wake this one — "
           "otherwise a 17-NAK burst spins the whole ladder in "
           "milliseconds and the link drops instead of recovering");
    std::cout << "  PASS" << std::endl;
}

void test_nak_for_other_seq_does_not_wake() {
    std::cout << "\n=== Pin 3: a NAK naming a different seq does not wake ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg = makeCfg();
    NakInjectingHal hal;
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor acc(link);
    acc.forceState(State::OK);
    hal.link = &link;
    hal.acc = &acc;

    // Put a different seq in flight too, so the foreign NAK is for
    // a genuinely pending slot and reaches the wake path.
    seedInFlight(hal, acc, (uint8_t)(kSeq + 1));
    seedInFlight(hal, acc, kSeq);
    hal.nakSeq = (uint8_t)(kSeq + 1);
    hal.waitStartMs = hal.now;
    hal.armed = true;

    uint32_t t0 = hal.now;
    hal.lock();
    bool acked = acc.arq().waitForAck(acc.halCtx(), kSeq, kRto);
    hal.unlock();
    uint32_t elapsed = hal.now - t0;

    std::cout << "  acked=" << (acked ? "true" : "false")
              << " elapsed=" << elapsed << " ms" << std::endl;
    assert(!acked);
    assert(elapsed >= kRto &&
           "only a NAK naming the seq under wait may cut the wait short");
    std::cout << "  PASS" << std::endl;
}

} // namespace

int main() {
    std::cout << "=== SYNC NAK fast retx ===" << std::endl;
    test_nak_wakes_wait_before_rto();
    test_nak_burst_capped_at_one_wake_per_attempt();
    test_nak_for_other_seq_does_not_wake();
    std::cout << "\nAll SyncNakFastRetx pins passed." << std::endl;
    return 0;
}
