// AL89 pin 5 / HoldNakSelfDescribingTest. AL-D1: converted from a
// source-grep (checking that "holdNakFreeAtSet_" appears as a
// string in LinkRx.cpp) to a real behavioral test that drives the
// actual receive path (Link::onPayload, via a new test-only
// accessor — LinkTestAccessor::onPayloadForTest) and observes the
// real wire output (MockHal's txBuf byte count). The source-grep
// version would pass unchanged even if the suppression logic were
// deleted, as long as the identifier `holdNakFreeAtSet_` still
// existed somewhere in a comment.
#include "FieldWedgeFixes89Common.h"

using namespace autolink;
using namespace autolink::field89;

// AL89-5: an app-buf-full hold must NOT re-NAK on every retx of the
// same seq (that's the field's 29-NAKs-per-base storm) — it must
// re-NAK only once the receiver's appBufFree() has actually grown
// since the hold was set, the only signal the peer's drain made
// real progress. Toggle off (delete the appBufFree-growth gate,
// restoring the old NAK-on-every-retx shape) -> red: this test
// counts wire bytes directly and fails the moment a second NAK goes
// out without any drain between calls.
void test_HoldNakSelfDescribingTest() {
    std::cout << "\n=== Pin 5: hold-NAK self-describing "
                 "(gated on appBufFree growth) ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    MockHal hal;
    Link link(hal, cache, true, cfg);
    link.begin();
    LinkTestAccessor acc(link);
    acc.forceState(State::OK);

    // A small, fixed app-buffer capacity under test-harness control
    // (MockHal::appBufCap / appBuf), independent of cfg's real
    // stream buffer sizing.
    hal.appBufCap = 100;

    // Fill to leave only 5 bytes free.
    std::vector<uint8_t> filler(95, 0xAA);
    hal.pushAppBuf(filler.data(), (int)filler.size());
    if (hal.appBufFree() != 5) {
        std::cerr << "\nFAIL: harness setup wrong — appBufFree()="
                  << hal.appBufFree() << ", want 5" << std::endl;
        assert(false);
    }

    const uint8_t seq = 7;
    std::vector<uint8_t> payload(50, 0x42); // 50 > any appBufFree() used below

    auto txBytesFor = [&](const char *label) -> size_t {
        size_t before = hal.txBuf.size();
        acc.onPayloadForTest(seq, payload.data(), (int)payload.size());
        size_t after = hal.txBuf.size();
        std::cout << "  " << label << ": appBufFree()=" << hal.appBufFree()
                  << " txBuf grew by " << (after - before) << " B"
                  << std::endl;
        return after - before;
    };

    // Call 1: fresh hold, appBufFree()=5 < 50 -> all-or-nothing NAK.
    // This is a fresh hold (holdNakActive_ was false), so it must
    // NAK regardless of the growth gate.
    size_t d1 = txBytesFor("call 1 (fresh hold, free=5)");
    if (d1 == 0) {
        std::cerr << "\nFAIL: the first hold (fresh) sent no NAK at all — "
                     "onPayloadForTest / the all-or-nothing branch isn't "
                     "being reached (check appBufFree() is actually < "
                     "payload size, or that state==OK)"
                  << std::endl;
        assert(false);
    }

    // Call 2: SAME seq, NO drain — appBufFree() is still 5, exactly
    // what it was when the hold was set. This is the suppression
    // this pin exists for: must NOT re-NAK.
    size_t d2 = txBytesFor("call 2 (same hold, free=5, no drain)");
    if (d2 != 0) {
        std::cerr << "\nFAIL: a repeat onPayload for the same held seq, "
                     "with appBufFree() unchanged since the hold was set, "
                     "sent a NAK anyway ("
                  << d2
                  << " B). This is the exact field-capture storm shape "
                     "(29 NAKs for one held base) — the appBufFree-growth "
                     "gate is missing or broken."
                  << std::endl;
        assert(false);
    }

    // Drain 20 bytes from the app buffer (simulating the app
    // actually reading), growing appBufFree() from 5 to 25.
    uint8_t drain[20];
    hal.popAppBuf(drain, 20);
    if (hal.appBufFree() != 25) {
        std::cerr << "\nFAIL: harness drain wrong — appBufFree()="
                  << hal.appBufFree() << ", want 25" << std::endl;
        assert(false);
    }

    // Call 3: SAME seq, appBufFree() grew (5 -> 25) since the hold
    // was set. Must re-NAK exactly once, refreshing the snapshot.
    size_t d3 = txBytesFor("call 3 (same hold, free=25, drained since)");
    if (d3 == 0) {
        std::cerr << "\nFAIL: appBufFree() grew (5 -> 25) since the hold "
                     "was set, but no NAK went out on the next call — a "
                     "receiver whose app IS draining would never see a "
                     "retry NAK, stalling recovery indefinitely."
                  << std::endl;
        assert(false);
    }

    // Call 4: SAME seq, no further drain since call 3 refreshed the
    // snapshot to 25. Must suppress again.
    size_t d4 = txBytesFor("call 4 (same hold, free=25, no further drain)");
    if (d4 != 0) {
        std::cerr << "\nFAIL: a repeat onPayload right after a refreshed "
                     "hold (no drain since) sent a NAK anyway ("
                  << d4 << " B) — the snapshot refresh isn't suppressing."
                  << std::endl;
        assert(false);
    }

    std::cout << "  PASS (NAK on fresh hold, suppressed with no drain, "
                 "NAK on drain, suppressed again with no further drain)"
              << std::endl;
}
