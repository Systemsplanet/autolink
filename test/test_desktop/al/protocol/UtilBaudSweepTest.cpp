// Host-only unit tests for UtilBaudSweep. Compiled in with the other
// Util*Test binaries; Arduino/ESP-IDF builds skip this file (no #ifndef
// ARDUINO guard needed -- it's never #included from Arduino sketches).
#include <iostream>
#include <cassert>
#include "al/protocol/UtilBaudSweep.h"

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

static void test_fastest_baud_above_threshold_wins() {
 std::cout << "\n=== Test: Fastest Baud Above Threshold Wins ===" << std::endl;
 // scoring 4/4 and threshold 50% (>= 2 hits), the first qualifying
 // baud is the fastest one, index 0.
 UtilBaudSweep s(5);
 s.configure({4, 0.5f, -1});
 for (int j = 0; j < 5; j++) {
 for (int k = 0; k < 4; k++) s.score(j);
 }
 assert(s.pickBest() == 0);
 std::cout << "PASS" << std::endl;
}

static void test_baud_below_threshold_falls_back() {
 std::cout << "\n=== Test: Flaky Top Baud Falls Back to Reliable One ===" << std::endl;
 // a slower baud is reliable, pickBest should still fall through to
 // the reliable one rather than locking at the flaky top baud.
 UtilBaudSweep s(5);
 s.configure({4, 0.5f, -1});
 // Baud 0 got 1 hit (25%, below 50% threshold). Baud 1 got all 4.
 // The pick should be 1, not 0.
 for (int k = 0; k < 1; k++) s.score(0);
 for (int k = 0; k < 4; k++) s.score(1);
 for (int k = 0; k < 4; k++) s.score(2);
 for (int k = 0; k < 4; k++) s.score(3);
 for (int k = 0; k < 4; k++) s.score(4);
 assert(s.pickBest() == 1);
 std::cout << "PASS" << std::endl;
}

static void test_strict_threshold() {
 std::cout << "\n=== Test: Strict Threshold (80%) ===" << std::endl;
 // * If baud 0 is flaky (1/5), it fails. The fastest that passes
 // (>= 4 hits) wins.
 // * If baud 0 is perfect (5/5), it wins.
 UtilBaudSweep s(5);
 s.configure({5, 0.8f, -1});
 // Baud 0 flaky: 1/5 = 20%, fails. Baud 1 perfect: 5/5 = 100%, passes.
 for (int k = 0; k < 1; k++) s.score(0);
 for (int k = 0; k < 5; k++) s.score(1);
 for (int k = 0; k < 5; k++) s.score(2);
 for (int k = 0; k < 5; k++) s.score(3);
 for (int k = 0; k < 5; k++) s.score(4);
 assert(s.pickBest() == 1);
 std::cout << "PASS" << std::endl;
}

static void test_lenient_threshold_picks_flaky_top() {
 std::cout << "\n=== Test: Lenient Threshold (10%) Picks Flaky Top ===" << std::endl;
 // every baud, the fastest one (index 0) qualifies first.
 UtilBaudSweep s(5);
 s.configure({10, 0.1f, -1});
 s.score(4); // 1/10 = 10%, meets
 for (int k = 0; k < 10; k++) s.score(3); // 100%
 for (int k = 0; k < 10; k++) s.score(2);
 for (int k = 0; k < 10; k++) s.score(1);
 for (int k = 0; k < 10; k++) s.score(0);
 assert(s.pickBest() == 0);
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
 // {115200, 57600, 38400, 19200, 9600} -- index 0 is the fastest.
 std::cout << "\n=== Test: Realistic Cable Scenario ===" << std::endl;
 UtilBaudSweep s(5); // index 0 = 115200, index 4 = 9600
 s.configure({4, 0.5f, -1});
 // 115200 (index 0) only got 1 decode (25%, below 50% threshold).
 // All other bauds are 4/4 (100%, pass).
 s.score(0);
 for (int k = 0; k < 4; k++) s.score(1);
 for (int k = 0; k < 4; k++) s.score(2);
 for (int k = 0; k < 4; k++) s.score(3);
 for (int k = 0; k < 4; k++) s.score(4);
 // pickBest (fastest-first) skips the flaky 115200 and locks at the
 // next-fastest reliable baud: index 1 = 57600. No frame loss.
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
 std::cout << "\n=== UtilBaudSweep Tests Completed Successfully ===" << std::endl;
 return 0;
}
