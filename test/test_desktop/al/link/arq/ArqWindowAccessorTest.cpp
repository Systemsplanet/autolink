// AL88-1/AL88-2: sendMsg's admission gate checks the runtime-clamped
// window (see PipelineWindowClampTest), but until now nothing public
// let a caller read that clamped value — Ping.h sized its draws
// against the compile-time AUTOLINK_ARQ_PIPELINE_WINDOW instead,
// which over-admits against a narrow installed ring: sendMsg rejects
// the oversized draw with GbnWindowFull every time, on every attempt,
// because the draw never adapts to the real ceiling. Field-measured
// as ASYNC throughput at ~3% of SYNC's on an identically-configured
// link. This suite pins Link::arqWindow() and the AutoLink facade
// passthrough that exposes it to app code.
#ifndef ARDUINO

#    include <cassert>
#    include <iostream>
#    include "al/link/arq/ArqCache.h"
#    include "al/link/Link.h"
#    include "al/AutoLinkConfig.h"
#    include "AutoLink.h"
#    include "AutoLinkTestAccessor.h"
#    include "MockHal.h"

using namespace autolink;

static AutoLinkConfig minimalCfg(AutoLinkConfig::Mode mode) {
    AutoLinkConfig cfg;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 115200;
    cfg.mode = mode;
    return cfg;
}

void test_link_arqWindow_reflects_unclamped_default() {
    std::cout << "\n=== Test: Link::arqWindow() reads back the "
                 "compile-time default before any clamp ===\n";
    MockHal hal;
    hal.setTxCapForTest(65536); // roomy — no clamp engages
    ArqCache cache;
    Link link(hal, cache, /*isMasterNode=*/true,
              minimalCfg(AutoLinkConfig::Mode::ASYNC));
    bool ok = link.begin();
    assert(ok);
    assert(link.arqWindow() == AUTOLINK_ARQ_PIPELINE_WINDOW);
    assert(link.arqWindow() == cache.window());
    std::cout << "  PASS (arqWindow()=" << link.arqWindow() << ")\n";
}

void test_link_arqWindow_reflects_narrow_ring_clamp() {
    std::cout << "\n=== Test: Link::arqWindow() reflects the "
                 "installed-ring clamp ===\n";
    MockHal hal;
    hal.setTxCapForTest(1000); // narrow — clamp engages (see
                               // PipelineWindowClampTest for the math)
    ArqCache cache;
    Link link(hal, cache, /*isMasterNode=*/true,
              minimalCfg(AutoLinkConfig::Mode::ASYNC));
    bool ok = link.begin();
    assert(ok);
    assert(link.arqWindow() < AUTOLINK_ARQ_PIPELINE_WINDOW);
    assert(link.arqWindow() == cache.window());
    std::cout << "  PASS (arqWindow()=" << link.arqWindow()
              << " < compile-time " << AUTOLINK_ARQ_PIPELINE_WINDOW << ")\n";
}

void test_autolink_facade_arqWindow_passthrough() {
    std::cout << "\n=== Test: AutoLink::arqWindow() facade passthrough "
                 "matches the underlying Link ===\n";
    MockHal hal;
    hal.setTxCapForTest(1000);
    AutoLinkConfig cfg = minimalCfg(AutoLinkConfig::Mode::ASYNC);
    AutoLink facade(hal, /*isMasterNode=*/true, cfg);
    AutoLinkTestAccessor t(facade);

    // Before begin(): the underlying Link exists (constructed in the
    // host-test ctor) but hasn't clamped anything yet — arqWindow()
    // must not crash and must agree with the Link directly.
    assert(facade.arqWindow() == t.link()->arqWindow());

    bool ok = facade.begin();
    assert(ok);
    assert(facade.arqWindow() == t.link()->arqWindow());
    assert(facade.arqWindow() < AUTOLINK_ARQ_PIPELINE_WINDOW);
    std::cout << "  PASS (facade.arqWindow()=" << facade.arqWindow()
              << " matches Link::arqWindow())\n";
}

int main() {
    std::cout << "=== Running ArqWindowAccessor Tests ===" << std::endl;
    test_link_arqWindow_reflects_unclamped_default();
    test_link_arqWindow_reflects_narrow_ring_clamp();
    test_autolink_facade_arqWindow_passthrough();
    std::cout << "\n=== ArqWindowAccessor Tests Completed Successfully ==="
              << std::endl;
    return 0;
}

#endif
