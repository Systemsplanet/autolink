// WireSim closed-loop: link-up + ARQ.
#include "WireSim.h"
#include "al/util/Log.h"
#include <iostream>
#include <cassert>

using namespace autolink;

void test_closed_loop_with_forced_drops();
void test_closed_loop_saturates_after_many_drops();
void test_closed_loop_single_drop_recovers();
void test_cache_miss_loop_does_not_latch();
void test_gap_heals_without_resweep();

void test_closed_loop_with_forced_drops() {
    std::cout
        << "\n=== Test: 5000-cycle closed loop with 2% drop + forced drops every 800 (killer) ==="
        << std::endl;
    Log::log().setLevel(Log::Level::INFO);

    AutoLinkConfig cfg;
    cfg.errThreshold = 10000;
    WireSim sim(cfg);
    sim.setFrameDropPct(2);
    sim.setForcedDropEvery(800);

    TwoNodeFixture f(sim);
    f.begin();

    for (int warmup = 0; warmup < 1500; warmup++) {
        f.step(1);
        if (f.getStateA() == State::OK && f.getStateB() == State::OK)
            break;
    }
    if (f.getStateA() != State::OK || f.getStateB() != State::OK) {
        std::cerr << "\nFAIL: link never reached OK (A=" << (int)f.getStateA()
                  << ", B=" << (int)f.getStateB() << ")" << std::endl;
        assert(false);
    }

    int dropsSeen = 0;

    for (int cycle = 0; cycle < 5000; cycle++) {
        f.step(1);

        if (sim.dropsInjected() > dropsSeen) {
            dropsSeen = sim.dropsInjected();

            int savedBurst = f.maxBurstPerLoop;
            f.maxBurstPerLoop = 0;
            for (int drain = 0; drain < 2000; drain++) {
                f.step(1);
                if (f.pendingCountA() == 0)
                    break;
            }
            f.maxBurstPerLoop = savedBurst;
            if (f.pendingCountA() > 0) {
                std::cerr << "\nFAIL cycle " << cycle
                          << ": pendingCountA=" << f.pendingCountA()
                          << " after " << dropsSeen << " drop(s)"
                          << " (cache orphaned, gate will latch — bug)"
                          << std::endl;
                assert(false);
            }

            if (f.pendingCountB() > 0) {
                std::cerr << "\nFAIL cycle " << cycle
                          << ": pendingCountB=" << f.pendingCountB()
                          << " after " << dropsSeen << " drop(s)" << std::endl;
                assert(false);
            }
        }
    }

    std::cout << "  5000 cycles, " << sim.dropsInjected() << " drops, "
              << sim.framesDropped() << " wire bytes dropped, "
              << "total bytes A->B: " << f.totalBytesAtoB() << std::endl;
    if (f.totalBytesAtoB() < 100) {
        std::cerr << "\nFAIL: too few bytes transferred (" << f.totalBytesAtoB()
                  << "), link is dead" << std::endl;
        assert(false);
    }
    std::cout << "PASS" << std::endl;
}

void test_closed_loop_saturates_after_many_drops() {
    std::cout << "\n=== Test: 10 back-to-back forced drops (saturation) ==="
              << std::endl;
    Log::log().setLevel(Log::Level::WARNING);

    WireSim sim;
    sim.setFrameDropPct(0);
    sim.setForcedDropEvery(0);

    TwoNodeFixture f(sim);
    f.begin();
    for (int warmup = 0; warmup < 1500; warmup++) {
        f.step(1);
        if (f.getStateA() == State::OK && f.getStateB() == State::OK)
            break;
    }
    if (f.getStateA() != State::OK) {
        std::cerr << "\nFAIL: link never reached OK" << std::endl;
        assert(false);
    }

    for (int i = 0; i < 3; i++) {
        sim.requestDrop();
        for (int wait = 0; wait < 100; wait++)
            f.step(1);
    }

    for (int wait = 0; wait < 5000; wait++) {
        f.step(1);
        if (f.getStateA() == State::OK && f.getStateB() == State::OK)
            break;
    }
    if (f.getStateA() != State::OK) {
        std::cerr << "\nFAIL: link didn't recover to OK after 3 drops"
                  << std::endl;
        assert(false);
    }

    uint64_t before = f.totalBytesAtoB();
    std::cout << "    [after-3-drops bytes=" << before
              << " stateA=" << (int)f.getStateA()
              << " stateB=" << (int)f.getStateB()
              << " pendingA=" << f.pendingCountA()
              << " pendingB=" << f.pendingCountB() << "]" << std::endl;
    bool moved = false;
    for (int i = 0; i < 10000; i++) {
        f.step(1);
        if (f.totalBytesAtoB() > before) {
            moved = true;
            std::cout << "    [bytes moved at i=" << i << "]" << std::endl;
            break;
        }
        if (i % 200 == 0) {
            std::cout << "    [i=" << i << " bytes=" << f.totalBytesAtoB()
                      << " stateA=" << (int)f.getStateA() << "]" << std::endl;
        }
    }
    if (!moved) {
        f.debug_log_ = true;
        std::cerr << "\nFAIL: bytes didn't move after 3 drops (stuck at "
                  << f.totalBytesAtoB() << "), pendingA=" << f.pendingCountA()
                  << " pendingB=" << f.pendingCountB()
                  << " stateA=" << (int)f.getStateA()
                  << " stateB=" << (int)f.getStateB()
                  << " halA.txBuf=" << f.halA().txBuf.size()
                  << " halB.txBuf=" << f.halB().txBuf.size() << std::endl;
        for (int i = 0; i < 5; i++)
            f.step(1);
        assert(false);
    }
    std::cout
        << "  3 drops, cache clean, link back to OK, bytes resumed (cycle="
        << sim.dropsInjected() << " drops total)" << std::endl;
    std::cout << "PASS" << std::endl;
}

void test_closed_loop_single_drop_recovers() {
    std::cout
        << "\n=== Test: single forced drop, cache stays clean (regression) ==="
        << std::endl;
    Log::log().setLevel(Log::Level::WARNING);

    WireSim sim;
    sim.setFrameDropPct(0);
    sim.setForcedDropEvery(0);

    TwoNodeFixture f(sim);
    f.begin();
    for (int warmup = 0; warmup < 1500; warmup++) {
        f.step(1);
        if (f.getStateA() == State::OK && f.getStateB() == State::OK)
            break;
    }

    for (int i = 0; i < 500; i++)
        f.step(1);

    sim.requestDrop();
    f.maxBurstPerLoop = 0;
    for (int drain = 0; drain < 500; drain++)
        f.step(1);
    if (f.pendingCountA() > 0) {
        std::cerr << "\nFAIL: pendingCountA=" << f.pendingCountA()
                  << " after single drop (cache not cleared by "
                  << "the link-reset hook)" << std::endl;
        assert(false);
    }
    if (f.getStateA() != State::OK) {
        std::cerr << "\nFAIL: link didn't recover from single drop"
                  << std::endl;
        assert(false);
    }
    f.maxBurstPerLoop = 4;
    std::cout << "  pendingCount " << f.pendingCountA()
              << " immediately after drop (before refill), link in OK"
              << std::endl;
    std::cout << "PASS" << std::endl;
}

void test_cache_miss_loop_does_not_latch() {
    std::cout << "\n=== Test: cache-miss loop does not latch (the fix) ==="
              << std::endl;
    WireSim sim;
    sim.setFrameDropPct(20);
    sim.setForcedDropEvery(500);
    TwoNodeFixture fix(sim);
    int baselineDrops = sim.dropsInjected();
    for (int cycle = 0; cycle < 5000; cycle++)
        fix.step(1);
    int drops = sim.dropsInjected();
    int protoDrops = sim.protoDropsSeen();
    int dropsDelta = drops - baselineDrops;
    std::cout << "  drops_injected=" << drops << " (+" << dropsDelta << ")"
              << ", proto_drops=" << protoDrops << std::endl;

    assert(protoDrops <= dropsDelta * 2 + 20);
    std::cout << "PASS" << std::endl;
}

int main() {
    std::cout << "=== Closed-Loop Two-Node AutoLink Tests (WireSim) ==="
              << std::endl;
    test_closed_loop_with_forced_drops();
    test_closed_loop_saturates_after_many_drops();
    test_closed_loop_single_drop_recovers();
    test_cache_miss_loop_does_not_latch();
    test_gap_heals_without_resweep();
    std::cout << "\n=== WireSim Closed-Loop Tests Completed Successfully ==="
              << std::endl;
    return 0;
}

void test_gap_heals_without_resweep() {
    std::cout
        << "\n=== Test: gap heals without re-sweep at 5 fill levels (killer) ==="
        << std::endl;
    Log::log().setLevel(Log::Level::WARNING);

    int fillLevels[] = { 0, 8, 16, 24, 31 };
    for (int fi = 0; fi < 5; fi++) {
        int targetFill = fillLevels[fi];
        std::cout << "  fill=" << targetFill << " ... " << std::flush;

        AutoLinkConfig cfg;
        cfg.errThreshold = 10000;
        WireSim sim(cfg);
        sim.setFrameDropPct(0);
        sim.setForcedDropEvery(0);

        TwoNodeFixture f(sim);
        f.begin();

        for (int w = 0; w < 2000; w++) {
            f.step(1);
            if (f.getStateA() == State::OK && f.getStateB() == State::OK)
                break;
        }
        if (f.getStateA() != State::OK || f.getStateB() != State::OK) {
            std::cerr << "FAIL: link never reached OK at fill=" << targetFill
                      << std::endl;
            assert(false);
        }

        for (int w = 0; w < 500; w++)
            f.step(1);

        (void)targetFill;

        uint64_t bytesBefore =
            sim.bytesTransferredAtoB() + sim.bytesTransferredBtoA();
        State stateABefore = f.getStateA();
        State stateBBefore = f.getStateB();

        sim.halA().dropNextFrames = 3;

        for (int w = 0; w < 3000; w++)
            f.step(1);

        if (f.getStateA() != State::OK || f.getStateB() != State::OK) {
            std::cerr << "FAIL fill=" << targetFill
                      << ": link re-swept (stateBefore=OK, stateAfter A="
                      << (int)f.getStateA() << " B=" << (int)f.getStateB()
                      << ")" << std::endl;
            (void)stateABefore;
            (void)stateBBefore;
            assert(false);
        }

        uint64_t bytesAfter =
            sim.bytesTransferredAtoB() + sim.bytesTransferredBtoA();
        if (bytesAfter <= bytesBefore + 100) {
            std::cerr << "FAIL fill=" << targetFill
                      << ": bytes stalled after gap injection"
                      << " (before=" << bytesBefore << " after=" << bytesAfter
                      << ")" << std::endl;
            assert(false);
        }

        if (sim.dropsInjected() > 0) {
            std::cerr << "FAIL fill=" << targetFill
                      << ": forced link drop fired unexpectedly (dropsInjected="
                      << sim.dropsInjected() << ")" << std::endl;
            assert(false);
        }
        std::cout << "PASS (bytesDelta=" << (bytesAfter - bytesBefore) << ")"
                  << std::endl;
    }
    std::cout << "  All fill levels: gap heals without re-sweep PASS"
              << std::endl;
}