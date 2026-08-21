// AL89 pin 4 / ArqWindowClampedToReceiverTest. Extracted from
// FieldWedgeFixes89Test.cpp (AL90-17 split
// the monolithic 22.7 KB file into one .cpp
// per pin to keep each under the 15 KB cap,
// AGENTS.md rule 20a). The pin's logic is
// unchanged; only the file boundary and the
// function name (per AL90-15) move.
#include "FieldWedgeFixes89Common.h"

using namespace autolink;
using namespace autolink::field89;

// runtime window is bounded by both
// the TX ring AND the RX stream
// buffer. Toggle off (drop the
// receiver clamp) -> red.
void test_ArqWindowClampedToReceiverTest() {
    std::cout << "\n=== Pin 4: arqWindow clamped to receiver buffer ==="
              << std::endl;
    AutoLinkConfig cfg;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    cfg.maxMsg = 2048;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 512000;
    cfg.streamBufferSize = 4108; // The field's 4108 B receiver.
    MockHal hal;
    ArqCache cache;
    Link link(hal, cache, /*isMasterNode=*/true, cfg);
    bool ok = link.begin();
    assert(ok);
    int w = link.arqWindow();
    // The receiver-capacity clamp: 4108 / (MAX_CHUNK + MSG_HDR)
    // = 4108 / 256 = 16. The compile-time
    // AUTOLINK_ARQ_PIPELINE_WINDOW is 32; without
    // the receiver clamp, the runtime
    // window would be 32.
    assert(w <= 16 &&
           "arqWindow exceeded the receiver-capacity clamp — "
           "the field's 8 KB GBN window overran a 4108 B RX "
           "stream buffer, producing 29-NAKs-per-base storms. "
           "AL89-4's fix was to bound the runtime window by "
           "min(TX-ring capacity, RX-buf capacity).");
    std::cout << "  PASS (arqWindow=" << w
              << " <= 16, the receiver-capacity floor)" << std::endl;
}

// Pin 5 (AL89-5): hold-NAK is
// self-describing — re-emits only when
// the receiver's appBufFree has grown
// since the hold was set. Toggle off
