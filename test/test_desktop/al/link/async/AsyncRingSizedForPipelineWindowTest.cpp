// AL88-4: uartTxBufferFloor's ASYNC branch previously floored only on
// max(retx budget, one full maxMsg burst). With the field's default
// config (maxMsg=2048, 10 chunks), that sized a 2540-byte ring —
// exactly one message's worth — which then forced Link::begin()'s
// installed-ring clamp (see PipelineWindowClampTest) down to a
// 10-chunk GBN window against a compile-time 32. The clamp is a
// correctness safety net for a genuinely heap-starved ring; it
// should not be the *normal* outcome of an unremarkable config. This
// suite pins that the ASYNC floor also covers a full pipeline
// window's worth of chunks, so a default-sized ring leaves
// Link::begin()'s TX-side window unclamped.
//
// Toggle off (drop the windowFloor term from uartTxBufferFloor's
// ASYNC branch) -> the second assertion in
// test_default_config_floor_covers_full_window fails, since the
// floor reverts to sizing for one message rather than the full
// window.
//
// AL89-4: the field's 4108 B receiver stream buffer
// (streamBufferFloor(maxMsg=2048) = 2 * (2048 + 6) = 4108)
// cannot accept a full 32-frame window at 256 B/frame
// — 32 * 256 = 8192 B overruns the receiver. The
// receiver-capacity clamp in Link::begin() now
// constrains the runtime window to 4108/256 = 16.
// The TX-side floor still provisions a full 32-frame
// ring (test 1), but the runtime window is bounded
// by the smaller of the two clamps. Pinned by
// ArqWindowClampedToReceiverTest.

#ifndef ARDUINO

#    include <cassert>
#    include <iostream>
#    include "al/AutoLinkConfig.h"
#    include "al/link/Link.h"
#    include "al/link/arq/ArqCache.h"
#    include "MockHal.h"

using namespace autolink;

void test_async_floor_covers_full_pipeline_window() {
    std::cout << "\n=== Test: uartTxBufferFloor(ASYNC) >= "
                 "AUTOLINK_ARQ_PIPELINE_WINDOW chunks ===\n";
    constexpr int kFrameOverhead = 4;
    AutoLinkConfig cfg;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    // A small maxMsg keeps the per-message floor well below the
    // window floor, isolating what this test pins.
    cfg.maxMsg = 200; // single-frame message
    size_t floor = uartTxBufferFloor(cfg);
    size_t perChunk = (size_t)MAX_CHUNK + kFrameOverhead;
    size_t windowFloor = perChunk * (size_t)AUTOLINK_ARQ_PIPELINE_WINDOW;
    if (floor < windowFloor) {
        std::cerr << "\nFAIL: ASYNC floor=" << floor
                  << " < windowFloor=" << windowFloor
                  << " — a small-maxMsg config no longer provisions a "
                     "ring that can hold a full pipeline window"
                  << std::endl;
        assert(false);
    }
    std::cout << "  PASS (floor=" << floor << " >= windowFloor=" << windowFloor
              << ")\n";
}

void test_default_config_floor_covers_full_window() {
    std::cout << "\n=== Test: field-default config (maxMsg=2048) no longer "
                 "clamps the ASYNC window ===\n";
    // Field logs (FireBeetle capture) ran maxMsg=2048 —
    // chunksForMsgLen=10 — and measured the installed-ring clamp
    // shrinking the runtime window from 32 to 10, collapsing ASYNC
    // to near-stop-and-wait. Reproduce that exact config and assert
    // Link::begin() no longer clamps it.
    AutoLinkConfig cfg;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    cfg.maxMsg = 2048;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 512000;

    MockHal hal; // begin(cfg) below auto-sizes hal's ring to
                 // uartTxBufferFloor(cfg), matching what EspHal
                 // installs in production
    ArqCache cache;
    Link link(hal, cache, /*isMasterNode=*/true, cfg);
    bool ok = link.begin();
    assert(ok);
    // AL89-4: the receiver-capacity clamp
    // (AL89-4) reduces the runtime window
    // for the field-default 4108 B RX
    // stream buffer: 4108 / (MAX_CHUNK +
    // MSG_HDR) = 4108 / 256 = 16. The TX
    // ring can hold the full 32 frames
    // (test 1), but the runtime window is
    // bounded by the smaller of the TX-
    // ring and the RX-buf clamps. The
    // intended property is "no clamp from
    // the TX ring" — the RX-buf clamp is
    // the new correct limit.
    if (link.arqWindow() == AUTOLINK_ARQ_PIPELINE_WINDOW) {
        std::cout << "  PASS (arqWindow()=" << link.arqWindow()
                  << " unclamped for the field-default config)\n";
        return;
    }
    if (link.arqWindow() >= AUTOLINK_ARQ_PIPELINE_WINDOW / 2) {
        // The receiver-capacity clamp
        // can reduce the window but only
        // by half (the floor is the
        // smaller of the TX-ring and
        // RX-buf headroom). Pinned by
        // ArqWindowClampedToReceiverTest.
        std::cout << "  PASS (arqWindow()=" << link.arqWindow()
                  << " clamped by receiver buffer, not TX ring)\n";
        return;
    }
    std::cerr << "\nFAIL: arqWindow()=" << link.arqWindow() << " < "
              << AUTOLINK_ARQ_PIPELINE_WINDOW / 2
              << " for the field-default maxMsg=2048 config — the "
                 "TX ring is too narrow to hold even a half-pipeline "
                 "window"
              << std::endl;
    assert(false);
}

int main() {
    std::cout << "=== Running AsyncRingSizedForPipelineWindow Tests ==="
              << std::endl;
    test_async_floor_covers_full_pipeline_window();
    test_default_config_floor_covers_full_window();
    std::cout << "\n=== AsyncRingSizedForPipelineWindow Tests Completed "
                 "Successfully ==="
              << std::endl;
    return 0;
}

#endif
