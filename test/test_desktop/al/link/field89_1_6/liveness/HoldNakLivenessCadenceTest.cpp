// AL97-2 / HoldNakLivenessCadenceTest.
//
// HoldNakSelfDescribingTest (AL89-5, parent directory) pins that a
// repeat onPayload for an already-held seq is suppressed UNTIL
// appBufFree() grows — the receiver's app actually draining is the
// only thing that re-arms the NAK. That's correct for a receiver
// that's slow but alive. It's wrong for a receiver whose app never
// drains at all: appBufFree() never grows, so after the one fresh-
// hold NAK the sender hears nothing further from this peer — and
// the sender's peer-stalled watchdog (a separate, baud-derived
// clock, LinkHealth.h) reads that silence as a dead link and tears
// it down, even though the peer is alive and correctly
// backpressuring.
//
// Fix: re-emit the hold NAK once per HOLD_NAK_LIVENESS_MS even with
// zero drain progress, not just on drain growth. Toggle off (drop
// the liveness branch, keep only the drain-growth re-emit) -> red:
// this test advances the clock past the liveness window with the
// app buffer frozen and asserts a NAK still goes out.
#include "../FieldWedgeFixes89Common.h"

using namespace autolink;
using namespace autolink::field89;

static Link *makeLinkOk(MockHal &hal, NullArqCache &cache,
                        LinkTestAccessor **outAcc) {
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    Link *link = new Link(hal, cache, true, cfg);
    link->begin();
    *outAcc = new LinkTestAccessor(*link);
    (*outAcc)->forceState(State::OK);
    return link;
}

// Pin 1: a fresh hold NAKs once, a same-state repeat suppresses (the
// pre-existing AL89-5 contract, re-verified here as the baseline
// this fix must not disturb), and once HOLD_NAK_LIVENESS_MS elapses
// with STILL no drain, the next onPayload call re-NAKs anyway.
static void test_liveness_reemits_with_zero_drain() {
    std::cout << "\n=== Pin 1: hold-NAK re-emits on the liveness timer "
                 "even with zero drain progress ==="
              << std::endl;
    NullArqCache cache;
    MockHal hal;
    LinkTestAccessor *acc = nullptr;
    Link *link = makeLinkOk(hal, cache, &acc);

    hal.appBufCap = 100;
    std::vector<uint8_t> filler(95, 0xAA);
    hal.pushAppBuf(filler.data(), (int)filler.size());
    assert(hal.appBufFree() == 5);

    const uint8_t seq = 3;
    std::vector<uint8_t> payload(50, 0x11); // > appBufFree(), forces the hold.

    auto callAndTxDelta = [&](const char *label) -> size_t {
        size_t before = hal.txBuf.size();
        acc->onPayloadForTest(seq, payload.data(), (int)payload.size());
        size_t after = hal.txBuf.size();
        std::cout << "  " << label << ": now=" << hal.nowMs()
                  << " appBufFree()=" << hal.appBufFree() << " txBuf grew by "
                  << (after - before) << " B" << std::endl;
        return after - before;
    };

    size_t d1 = callAndTxDelta("call 1 (fresh hold, t=0)");
    assert(d1 > 0 && "fresh hold must NAK");
    uint32_t lastMsAfterFresh = acc->holdNakLastMsForTest();
    assert(lastMsAfterFresh == hal.nowMs() &&
           "holdNakLastMs_ must be stamped on the fresh-hold NAK");

    size_t d2 = callAndTxDelta("call 2 (same hold, t=0, no drain)");
    assert(d2 == 0 &&
           "an immediate repeat with no drain and no liveness window "
           "elapsed must still suppress (AL89-5 baseline)");

    // Advance to just BEFORE the liveness window — must still
    // suppress (this isn't a "NAK on every call" regression check).
    hal.pumpClock(LinkTestAccessor::HOLD_NAK_LIVENESS_MS_FOR_TEST - 1);
    size_t d3 = callAndTxDelta("call 3 (same hold, t=window-1, no drain)");
    assert(d3 == 0 &&
           "must not re-NAK before the liveness window elapses — "
           "this is a bounded-rate liveness signal, not a per-call NAK");

    // Cross the liveness window with STILL zero drain. This is the
    // exact scenario the fix targets: a receiver whose app never
    // drains at all must still be heard from.
    hal.pumpClock(2);
    size_t d4 = callAndTxDelta("call 4 (same hold, t=window+1, no drain)");
    assert(d4 > 0 &&
           "AL97-2 regression: a stalled (zero-drain) receiver must "
           "re-emit the hold NAK once the liveness window elapses, "
           "even with no appBufFree() growth — otherwise the sender's "
           "peer-stalled watchdog reads the silence as a dead peer "
           "and tears down a link that is alive and correctly "
           "backpressuring");
    assert(acc->holdNakLastMsForTest() == hal.nowMs() &&
           "holdNakLastMs_ must be restamped on the liveness re-emit, "
           "not left at the fresh-hold timestamp");

    // Immediately after the liveness re-emit, suppress again (the
    // timer just reset — this must be periodic, not a one-shot).
    size_t d5 = callAndTxDelta("call 5 (same hold, right after reemit)");
    assert(d5 == 0 &&
           "immediately after a liveness re-emit, the next call must "
           "suppress again — the liveness window must restart, not "
           "fire on every subsequent call");

    // A second full window later, still zero drain: re-emits again.
    // Proves periodicity, not a single extra grace NAK.
    hal.pumpClock(LinkTestAccessor::HOLD_NAK_LIVENESS_MS_FOR_TEST);
    size_t d6 = callAndTxDelta("call 6 (same hold, one more window later)");
    assert(d6 > 0 &&
           "the liveness re-emit must be periodic — a second window "
           "with zero drain must NAK again, not just the first one");

    std::cout << "  PASS" << std::endl;
    delete acc;
    delete link;
}

// Pin 2: drain-triggered re-emits (the pre-existing AL89-5 path)
// still work unchanged, and correctly reset the liveness clock too
// — a drain-triggered NAK must not leave a stale liveness timestamp
// that immediately re-fires on the very next call.
static void test_drain_reemit_also_resets_liveness_clock() {
    std::cout << "\n=== Pin 2: a drain-triggered re-emit resets the "
                 "liveness clock too ==="
              << std::endl;
    NullArqCache cache;
    MockHal hal;
    LinkTestAccessor *acc = nullptr;
    Link *link = makeLinkOk(hal, cache, &acc);

    hal.appBufCap = 100;
    std::vector<uint8_t> filler(95, 0xAA);
    hal.pushAppBuf(filler.data(), (int)filler.size());

    const uint8_t seq = 9;
    std::vector<uint8_t> payload(50, 0x22);

    auto callAndTxDelta = [&]() -> size_t {
        size_t before = hal.txBuf.size();
        acc->onPayloadForTest(seq, payload.data(), (int)payload.size());
        return hal.txBuf.size() - before;
    };

    assert(callAndTxDelta() > 0 && "fresh hold must NAK");

    // Advance partway into the liveness window, then drain — the
    // drain-triggered NAK must fire (existing AL89-5 contract) AND
    // restamp holdNakLastMs_, so the immediate next call (still
    // within a fresh window) suppresses rather than double-firing.
    hal.pumpClock(LinkTestAccessor::HOLD_NAK_LIVENESS_MS_FOR_TEST / 2);
    uint8_t drain[10];
    hal.popAppBuf(drain, 10);
    size_t dDrain = callAndTxDelta();
    assert(dDrain > 0 && "drain-triggered re-emit must still fire");
    uint32_t afterDrainMs = acc->holdNakLastMsForTest();
    assert(afterDrainMs == hal.nowMs() &&
           "a drain-triggered re-emit must also restamp holdNakLastMs_");

    size_t dImmediate = callAndTxDelta();
    assert(dImmediate == 0 &&
           "right after a drain-triggered re-emit, an immediate "
           "same-state repeat must suppress — the liveness clock "
           "must have been restamped, not left stale");

    std::cout << "  PASS" << std::endl;
    delete acc;
    delete link;
}

int main() {
    std::cout << "=== Hold-NAK Liveness Cadence (AL97-2) ===" << std::endl;
    test_liveness_reemits_with_zero_drain();
    test_drain_reemit_also_resets_liveness_clock();
    std::cout << "\nAll HoldNakLivenessCadence pins passed." << std::endl;
    return 0;
}
