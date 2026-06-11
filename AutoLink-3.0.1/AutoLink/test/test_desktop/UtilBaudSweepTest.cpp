// Host-only unit tests for UtilBaudSweep. Compiled in with the other
// Util*Test binaries; Arduino/ESP-IDF builds skip this file (no #ifndef
// ARDUINO guard needed -- it's never #included from Arduino sketches).
#include <iostream>
#include <cassert>
#include "UtilBaudSweep.h"

using namespace autolink;

static void test_no_scores_picks_nothing() {
    std::cout << "\n=== Test: No Scores -> -1 ===" << std::endl;
    UtilBaudSweep s(5);
    s.configure({4, 0.5f, -1});
    assert(s.pickBest() == -1);
    std::cout << "PASS" << std::endl;
}

static void test_one_perfect_baud_wins() {
    std::cout << "\n=== Test: Single Perfect Baud Wins ===" << std::endl;
    UtilBaudSweep s(5);
    s.configure({4, 0.5f, -1});
    // Only baud 0 got any hits -- should still be picked, even though the
    // rate is 1/4 = 25% (< 50% threshold). The fallback loop in pickBest
    // returns the highest baud with > 0 decodes.
    s.score(0);
    assert(s.pickBest() == 0);
    std::cout << "PASS" << std::endl;
}

static void test_highest_baud_with_threshold_wins() {
    std::cout << "\n=== Test: Highest Baud Above Threshold Wins ===" << std::endl;
    UtilBaudSweep s(5);
    s.configure({4, 0.5f, -1});
    // 4/4 = 100% at all 5 bauds. With threshold 50% (>= 2 hits), pick the
    // highest: index 4.
    for (int j = 0; j < 5; j++) {
        for (int k = 0; k < 4; k++) s.score(j);
    }
    assert(s.pickBest() == 4);
    std::cout << "PASS" << std::endl;
}

static void test_baud_below_threshold_falls_back() {
    std::cout << "\n=== Test: Flaky Top Baud Falls Back to Reliable One ===" << std::endl;
    UtilBaudSweep s(5);
    s.configure({4, 0.5f, -1});
    // Baud 4 got only 1 hit (25%, below 50% threshold). Baud 3 got all 4.
    // The pick should be 3, not 4.
    for (int k = 0; k < 1; k++) s.score(4);
    for (int k = 0; k < 4; k++) s.score(3);
    for (int k = 0; k < 4; k++) s.score(2);
    for (int k = 0; k < 4; k++) s.score(1);
    for (int k = 0; k < 4; k++) s.score(0);
    assert(s.pickBest() == 3);
    std::cout << "PASS" << std::endl;
}

static void test_strict_threshold() {
    std::cout << "\n=== Test: Strict Threshold (80%) ===" << std::endl;
    UtilBaudSweep s(5);
    s.configure({5, 0.8f, -1});
    // 4/5 = 80% passes. 3/5 = 60% fails.
    for (int k = 0; k < 4; k++) s.score(4);   // 80% -- passes
    for (int k = 0; k < 3; k++) s.score(3);   // 60% -- fails, falls to 3
    // pickBest should return 3, the highest that *passes* the 80% bar.
    // Wait: 3/5 = 60%, threshold is 0.8*5=4 hits. So 3 fails. 4 passes.
    // pickBest returns 4. Hmm, let me re-check.
    // Actually the test should have baud 4 with < threshold so it falls
    // back to 3. Let me redo with clearer numbers.
    UtilBaudSweep s2(5);
    s2.configure({5, 0.8f, -1});
    for (int k = 0; k < 3; k++) s2.score(4);   // 60% -- fails
    for (int k = 0; k < 5; k++) s2.score(3);   // 100% -- passes
    for (int k = 0; k < 5; k++) s2.score(2);
    for (int k = 0; k < 5; k++) s2.score(1);
    for (int k = 0; k < 5; k++) s2.score(0);
    assert(s2.pickBest() == 3);
    std::cout << "PASS" << std::endl;
}

static void test_lenient_threshold_picks_flaky_top() {
    std::cout << "\n=== Test: Lenient Threshold (10%) Picks Flaky Top ===" << std::endl;
    UtilBaudSweep s(5);
    s.configure({10, 0.1f, -1});
    // 1/10 = 10% meets a 10% threshold. Pick the highest such baud.
    s.score(4);   // 1/10 = 10%, meets
    for (int k = 0; k < 10; k++) s.score(3);   // 100%
    for (int k = 0; k < 10; k++) s.score(2);
    for (int k = 0; k < 10; k++) s.score(1);
    for (int k = 0; k < 10; k++) s.score(0);
    assert(s.pickBest() == 4);
    std::cout << "PASS" << std::endl;
}

static void test_zero_threshold_legacy_behavior() {
    std::cout << "\n=== Test: Zero Threshold = First-Wins Legacy ===" << std::endl;
    UtilBaudSweep s(5);
    s.configure({1, 0.0f, -1});
    // minHits = 0 * 1 = 0; clamped to 1. So at least 1 decode is still
    // needed. That's a reasonable "safety" floor.
    s.score(2);
    assert(s.pickBest() == 2);
    std::cout << "PASS" << std::endl;
}

static void test_reset_all_clears_scores() {
    std::cout << "\n=== Test: resetAll() Clears Scores ===" << std::endl;
    UtilBaudSweep s(5);
    s.configure({4, 0.5f, -1});
    for (int k = 0; k < 4; k++) s.score(4);
    assert(s.pickBest() == 4);
    s.resetAll();
    assert(s.pickBest() == -1);
    for (int k = 0; k < 4; k++) s.score(2);
    assert(s.pickBest() == 2);
    std::cout << "PASS" << std::endl;
}

static void test_score_at_out_of_range_is_safe() {
    std::cout << "\n=== Test: Out-of-Range score()/scoreAt() Are Safe ===" << std::endl;
    UtilBaudSweep s(3);
    s.configure({4, 0.5f, -1});
    s.score(-1);
    s.score(99);
    assert(s.scoreAt(-1) == 0);
    assert(s.scoreAt(99) == 0);
    assert(s.scoreAt(0) == 0);
    assert(s.pickBest() == -1);
    std::cout << "PASS" << std::endl;
}

static void test_configure_sets_defaults() {
    std::cout << "\n=== Test: configure() Fills in expectedSamples ===" << std::endl;
    // expectedSamples == -1 means "inherit from pingSamplesPerBaud".
    UtilBaudSweep s(5);
    s.configure({3, 0.7f, -1});
    // 1/3 = 33% fails a 70% threshold (need >= ceil(0.7*3)=3 hits).
    s.score(4);
    // 3/3 = 100% passes.
    for (int k = 0; k < 3; k++) s.score(3);
    assert(s.pickBest() == 3);
    std::cout << "PASS" << std::endl;
}

static void test_explicit_expected_samples_overrides() {
    std::cout << "\n=== Test: Explicit expectedSamples Overrides ===" << std::endl;
    // Send 4 PINGs per baud but tell the class to evaluate against 8
    // (incomplete sweep). 4/8 = 50% with a 50% threshold = passes, but
    // the math is: 0.5 * 8 = 4, so 4 >= 4 passes. The class shouldn't
    // "know" the sweep is incomplete -- the caller does.
    UtilBaudSweep s(5);
    s.configure({4, 0.5f, 8});
    for (int k = 0; k < 4; k++) s.score(4);
    assert(s.pickBest() == 4);
    std::cout << "PASS" << std::endl;
}

static void test_realistic_cable_scenario() {
    // The scenario from the user's log: 115200 lost a frame, 57600 clean.
    std::cout << "\n=== Test: Realistic Cable Scenario ===" << std::endl;
    UtilBaudSweep s(5);  // {9600, 19200, 38400, 57600, 115200}
    s.configure({4, 0.5f, -1});
    // 4/4 at every baud except the top: 115200 only got 1 decode.
    for (int k = 0; k < 4; k++) s.score(0);
    for (int k = 0; k < 4; k++) s.score(1);
    for (int k = 0; k < 4; k++) s.score(2);
    for (int k = 0; k < 4; k++) s.score(3);
    s.score(4);
    // Old behavior: pickBest returns 4 (the highest with score > 0).
    //   -> link negotiates to 115200 with 25% reliability. Frame loss.
    // New behavior: pickBest returns 3 (highest with >= 50%).
    //   -> link negotiates to 57600 with 100% reliability. No loss.
    assert(s.pickBest() == 3);
    std::cout << "PASS" << std::endl;
}

int main() {
    std::cout << "=== Running UtilBaudSweep Tests ===" << std::endl;
    test_no_scores_picks_nothing();
    test_one_perfect_baud_wins();
    test_highest_baud_with_threshold_wins();
    test_baud_below_threshold_falls_back();
    test_strict_threshold();
    test_lenient_threshold_picks_flaky_top();
    test_zero_threshold_legacy_behavior();
    test_reset_all_clears_scores();
    test_score_at_out_of_range_is_safe();
    test_configure_sets_defaults();
    test_explicit_expected_samples_overrides();
    test_realistic_cable_scenario();
    std::cout << "\n=== UtilBaudSweep Tests Completed Successfully ===" << std::endl;
    return 0;
}
