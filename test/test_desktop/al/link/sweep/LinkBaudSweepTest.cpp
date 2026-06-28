// UtilBaudSweep score + pickBest logic.
#include <iostream>
#include <cassert>
#include "al/link/sweep/LinkBaudSweep.h"

using namespace autolink;

static void test_no_scores_picks_nothing() {
    std::cout << "\n=== Test: No Scores -> -1 ===" << std::endl;
    UtilBaudSweep s(5);
    s.configure({ 4, 0.5f, -1 });
    assert(s.pickBest() == -1);
    std::cout << "PASS" << std::endl;
}

static void test_one_perfect_baud_wins() {
    std::cout << "\n=== Test: Single Perfect Baud Wins ===" << std::endl;
    UtilBaudSweep s(5);
    s.configure({ 4, 0.5f, -1 });

    s.score(0);
    assert(s.pickBest() == 0);
    std::cout << "PASS" << std::endl;
}

static void test_fastest_baud_above_threshold_wins() {
    std::cout << "\n=== Test: Fastest Baud Above Threshold Wins ==="
              << std::endl;

    UtilBaudSweep s(5);
    s.configure({ 4, 0.5f, -1 });
    for (int j = 0; j < 5; j++) {
        for (int k = 0; k < 4; k++)
            s.score(j);
    }
    assert(s.pickBest() == 0);
    std::cout << "PASS" << std::endl;
}

static void test_baud_below_threshold_falls_back() {
    std::cout << "\n=== Test: Flaky Top Baud Falls Back to Reliable One ==="
              << std::endl;

    UtilBaudSweep s(5);
    s.configure({ 4, 0.5f, -1 });

    for (int k = 0; k < 1; k++)
        s.score(0);
    for (int k = 0; k < 4; k++)
        s.score(1);
    for (int k = 0; k < 4; k++)
        s.score(2);
    for (int k = 0; k < 4; k++)
        s.score(3);
    for (int k = 0; k < 4; k++)
        s.score(4);
    assert(s.pickBest() == 1);
    std::cout << "PASS" << std::endl;
}

static void test_strict_threshold() {
    std::cout << "\n=== Test: Strict Threshold (80%) ===" << std::endl;

    UtilBaudSweep s(5);
    s.configure({ 5, 0.8f, -1 });

    for (int k = 0; k < 1; k++)
        s.score(0);
    for (int k = 0; k < 5; k++)
        s.score(1);
    for (int k = 0; k < 5; k++)
        s.score(2);
    for (int k = 0; k < 5; k++)
        s.score(3);
    for (int k = 0; k < 5; k++)
        s.score(4);
    assert(s.pickBest() == 1);
    std::cout << "PASS" << std::endl;
}

static void test_lenient_threshold_picks_flaky_top() {
    std::cout << "\n=== Test: Lenient Threshold (10%) Picks Flaky Top ==="
              << std::endl;

    UtilBaudSweep s(5);
    s.configure({ 10, 0.1f, -1 });
    s.score(4);
    for (int k = 0; k < 10; k++)
        s.score(3);
    for (int k = 0; k < 10; k++)
        s.score(2);
    for (int k = 0; k < 10; k++)
        s.score(1);
    for (int k = 0; k < 10; k++)
        s.score(0);
    assert(s.pickBest() == 0);
    std::cout << "PASS" << std::endl;
}

static void test_zero_threshold_legacy_behavior() {
    std::cout << "\n=== Test: Zero Threshold = First-Wins Legacy ==="
              << std::endl;
    UtilBaudSweep s(5);
    s.configure({ 1, 0.0f, -1 });

    s.score(2);
    assert(s.pickBest() == 2);
    std::cout << "PASS" << std::endl;
}

static void test_reset_all_clears_scores() {
    std::cout << "\n=== Test: resetAll() Clears Scores ===" << std::endl;
    UtilBaudSweep s(5);
    s.configure({ 4, 0.5f, -1 });
    for (int k = 0; k < 4; k++)
        s.score(4);
    assert(s.pickBest() == 4);
    s.resetAll();
    assert(s.pickBest() == -1);
    for (int k = 0; k < 4; k++)
        s.score(2);
    assert(s.pickBest() == 2);
    std::cout << "PASS" << std::endl;
}

static void test_score_at_out_of_range_is_safe() {
    std::cout << "\n=== Test: Out-of-Range score()/scoreAt() Are Safe ==="
              << std::endl;
    UtilBaudSweep s(3);
    s.configure({ 4, 0.5f, -1 });
    s.score(-1);
    s.score(99);
    assert(s.scoreAt(-1) == 0);
    assert(s.scoreAt(99) == 0);
    assert(s.scoreAt(0) == 0);
    assert(s.pickBest() == -1);
    std::cout << "PASS" << std::endl;
}

static void test_configure_sets_defaults() {
    std::cout << "\n=== Test: configure() Fills in expectedSamples ==="
              << std::endl;

    UtilBaudSweep s(5);
    s.configure({ 3, 0.7f, -1 });

    s.score(4);

    for (int k = 0; k < 3; k++)
        s.score(3);
    assert(s.pickBest() == 3);
    std::cout << "PASS" << std::endl;
}

static void test_explicit_expected_samples_overrides() {
    std::cout << "\n=== Test: Explicit expectedSamples Overrides ==="
              << std::endl;

    UtilBaudSweep s(5);
    s.configure({ 4, 0.5f, 8 });
    for (int k = 0; k < 4; k++)
        s.score(4);
    assert(s.pickBest() == 4);
    std::cout << "PASS" << std::endl;
}

static void test_realistic_cable_scenario() {
    std::cout << "\n=== Test: Realistic Cable Scenario ===" << std::endl;
    UtilBaudSweep s(5);
    s.configure({ 4, 0.5f, -1 });

    s.score(0);
    for (int k = 0; k < 4; k++)
        s.score(1);
    for (int k = 0; k < 4; k++)
        s.score(2);
    for (int k = 0; k < 4; k++)
        s.score(3);
    for (int k = 0; k < 4; k++)
        s.score(4);

    assert(s.pickBest() == 1);
    std::cout << "PASS" << std::endl;
}

int main() {
    std::cout << "=== Running UtilBaudSweep Tests ===" << std::endl;
    test_no_scores_picks_nothing();
    test_one_perfect_baud_wins();
    test_fastest_baud_above_threshold_wins();
    test_baud_below_threshold_falls_back();
    test_strict_threshold();
    test_lenient_threshold_picks_flaky_top();
    test_zero_threshold_legacy_behavior();
    test_reset_all_clears_scores();
    test_score_at_out_of_range_is_safe();
    test_configure_sets_defaults();
    test_explicit_expected_samples_overrides();
    test_realistic_cable_scenario();
    std::cout << "\n=== UtilBaudSweep Tests Completed Successfully ==="
              << std::endl;
    return 0;
}