// AL87-18: the ARQ pipeline window (how many chunks sendMsg will
// admit in flight at once) is a compile-time default
// (AUTOLINK_ARQ_PIPELINE_WINDOW) with no knowledge of the actual
// installed TX ring. A heap-clamped or config-shrunk ring can hold
// fewer worst-case frames than the window would admit, so a full
// window burst can be accepted by the admission gate and then have
// nowhere to go on the wire ("TX ring can't fit header"). This
// suite pins ArqCache::setWindow's shrink-only clamp in isolation,
// then Link::begin()'s use of it against a real installed ring.
#ifndef ARDUINO

#    include <cassert>
#    include <iostream>
#    include "al/link/arq/ArqCache.h"
#    include "al/link/Link.h"
#    include "al/AutoLinkConfig.h"
#    include "MockHal.h"

using namespace autolink;

void test_setWindow_clamps_between_one_and_constructed_max() {
    std::cout << "\n=== Test: ArqCache::setWindow clamps to [1, constructed "
                 "window] ==="
              << std::endl;
    ArqCache c(8);
    assert(c.window() == 8);

    c.setWindow(20); // above the ceiling — clamps back to 8, never grows
    assert(c.window() == 8);

    c.setWindow(3); // shrink
    assert(c.window() == 3);

    c.setWindow(0); // floor at 1, never zero (admission would wedge)
    assert(c.window() == 1);

    c.setWindow(-5);
    assert(c.window() == 1);

    c.setWindow(8); // recovers back up to the constructed ceiling
    assert(c.window() == 8);

    std::cout << "PASS" << std::endl;
}

static AutoLinkConfig minimalCfg(AutoLinkConfig::Mode mode) {
    AutoLinkConfig cfg;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 115200;
    cfg.mode = mode;
    return cfg;
}

void test_begin_shrinks_window_to_fit_narrow_ring_async() {
    std::cout << "\n=== Test: Link::begin() clamps window to a narrow "
                 "installed ring (ASYNC) ==="
              << std::endl;
    // ASYNC's own ring-floor formula (uartTxBufferFloor) sizes a
    // ring to hold chunksForMsgLen(maxMsg) chunks at MAX_CHUNK+4
    // bytes each, not kWorstCaseCobsFrame — the clamp's divisor
    // must match that same assumption or it under-counts a ring
    // that was deliberately sized to hold exactly this many
    // chunks. Above MAX_CHUNK+4 (254) so begin() doesn't reject
    // the ring outright, but well below windowMax_ * (MAX_CHUNK+4)
    // (32*254=8128) so the clamp actually engages. 1000/254 = 3.
    MockHal hal;
    hal.setTxCapForTest(1000);
    ArqCache cache; // default window: AUTOLINK_ARQ_PIPELINE_WINDOW (32)
    assert(cache.window() == AUTOLINK_ARQ_PIPELINE_WINDOW);
    Link link(hal, cache, /*isMasterNode=*/true,
              minimalCfg(AutoLinkConfig::Mode::ASYNC));

    bool ok = link.begin();
    assert(ok);
    int want = (int)(1000 / (size_t)(MAX_CHUNK + 4));
    assert(cache.window() == want);
    assert(cache.window() < AUTOLINK_ARQ_PIPELINE_WINDOW);
    std::cout << "PASS (window clamped " << AUTOLINK_ARQ_PIPELINE_WINDOW
              << " -> " << cache.window() << " for a 1000-byte ring)"
              << std::endl;
}

void test_begin_shrinks_window_to_fit_narrow_ring_sync() {
    std::cout << "\n=== Test: Link::begin() clamps window to a narrow "
                 "installed ring (SYNC) ==="
              << std::endl;
    // SYNC's ring-floor formula reserves a full kWorstCaseCobsFrame
    // per chunk (plus one) — a stricter divisor than ASYNC's. Ring
    // large enough to admit a few frames but well below
    // windowMax_ * kWorstCaseCobsFrame (32*262=8384).
    // 1500/262 = 5.
    MockHal hal;
    hal.setTxCapForTest(1500);
    ArqCache cache;
    Link link(hal, cache, /*isMasterNode=*/true,
              minimalCfg(AutoLinkConfig::Mode::SYNC));

    bool ok = link.begin();
    assert(ok);
    int want = (int)(1500 / (size_t)kWorstCaseCobsFrame);
    assert(cache.window() == want);
    assert(cache.window() < AUTOLINK_ARQ_PIPELINE_WINDOW);
    std::cout << "PASS (window clamped " << AUTOLINK_ARQ_PIPELINE_WINDOW
              << " -> " << cache.window() << " for a 1500-byte ring, SYNC)"
              << std::endl;
}

void test_begin_leaves_window_alone_on_a_roomy_ring() {
    std::cout << "\n=== Test: Link::begin() leaves the window at its "
                 "compile-time default on a roomy ring ==="
              << std::endl;
    MockHal hal;
    hal.setTxCapForTest(
        65536); // comfortably above windowMax_ * kWorstCaseCobsFrame
    ArqCache cache;
    Link link(hal, cache, /*isMasterNode=*/true,
              minimalCfg(AutoLinkConfig::Mode::ASYNC));

    bool ok = link.begin();
    assert(ok);
    assert(cache.window() == AUTOLINK_ARQ_PIPELINE_WINDOW);
    std::cout << "PASS (window stays at " << AUTOLINK_ARQ_PIPELINE_WINDOW
              << " when the ring can hold the whole window)" << std::endl;
}

int main() {
    std::cout << "=== Running Pipeline Window Clamp Tests ===" << std::endl;
    test_setWindow_clamps_between_one_and_constructed_max();
    test_begin_shrinks_window_to_fit_narrow_ring_async();
    test_begin_shrinks_window_to_fit_narrow_ring_sync();
    test_begin_leaves_window_alone_on_a_roomy_ring();
    std::cout << "\n=== Pipeline Window Clamp Tests Completed Successfully ==="
              << std::endl;
    return 0;
}

#endif
