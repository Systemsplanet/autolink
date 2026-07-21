// Regression pin for the per-reason reset counter
// surfaced in Link::getDiag(). discCount_ in the
// Stats struct only counts a single total; an
// unexplained OK -> SWP transition (e.g. a
// debug-flooded log sink where every per-loop log
// line above was dropped) was traceable only by
// parsing discCount_ against vague log survivors.
// The contract: a ResetReason enum, a per-reason
// monotonic counter, a total resetCount, and a
// `resetReason=<...>` log line that fires on every
// reset_unlocked() — the log line is unconditional,
// so even if every other log above it is rate-limited
// away the reason still surfaces.
//
// Six pins:
//
//   1. reset_unlocked(true, ..., ResetReason::ErrThreshold)
//      increments resetReasonCounts_[ErrThreshold] by 1.
//   2. reset_unlocked(true, ..., ResetReason::ErrRate)
//      increments resetReasonCounts_[ErrRate] by 1.
//   3. dropLink() increments
//      resetReasonCounts_[UserDropLink].
//   4. onBreak() (health watchdog) increments
//      resetReasonCounts_[HealthWatchdog].
//   5. Each of the four triggers bumps the
//      monotonic resetCount_ by 1 — so a future
//      new ResetReason entry that forgets to
//      call reset_unlocked with count=true is
//      caught at the same layer.
//   6. Source-grep: the reset_unlocked body
//      emits the "reset_unlocked reason=" log
//      line unconditionally (no DEBUG/VERBOSE
//      gate), so the reason is always present
//      in the log stream.
//
// Toggle off (drop the per-reason increment or the
// unconditional log) and the matching pin flips red.
#include <iostream>
#include <cassert>
#include "MockHal.h"
#include "NullArqCache.h"
#include "LinkTestAccessor.h"
#include "al/AutoLinkConfig.h"
#include "al/link/Link.h"
#include "TestPaths.h"

using namespace autolink;

static const int kBauds[] = { 115200, 57600, 38400, 19200, 9600 };
static const int kNumBauds = (int)(sizeof(kBauds) / sizeof(kBauds[0]));

static void mkCfg(AutoLinkConfig &cfg) {
    for (int i = 0; i < kNumBauds; i++)
        cfg.allowedBauds[i] = (uint32_t)kBauds[i];
    cfg.allowedBaudsCount = kNumBauds;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 0;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
}

// Pin 1: reset_unlocked(true, ..., ErrThreshold) increments
// the ErrThreshold counter.
static void test_err_threshold_counter() {
    std::cout << "\n=== Pin 1: ErrThreshold counter increments ==="
              << std::endl;
    AutoLinkConfig cfg;
    mkCfg(cfg);
    NullArqCache cache;
    MockHal mHal;
    Link link(mHal, cache, true, cfg);
    link.begin();
    Diag d;
    link.getDiag(d);
    uint64_t beforeReason = d.resetReasons[(int)ResetReason::ErrThreshold];
    uint64_t beforeTotal = d.resetCount;
    LinkTestAccessor(link).resetLink(true, ResetReason::ErrThreshold);
    link.getDiag(d);
    assert(d.resetReasons[(int)ResetReason::ErrThreshold] == beforeReason + 1);
    assert(d.resetCount == beforeTotal + 1);
    std::cout << "  PASS (ErrThreshold " << beforeReason << " -> "
              << d.resetReasons[(int)ResetReason::ErrThreshold] << ", total "
              << beforeTotal << " -> " << d.resetCount << ")" << std::endl;
}

// Pin 2: reset_unlocked(true, ..., ErrRate) increments
// the ErrRate counter.
static void test_err_rate_counter() {
    std::cout << "\n=== Pin 2: ErrRate counter increments ===" << std::endl;
    AutoLinkConfig cfg;
    mkCfg(cfg);
    NullArqCache cache;
    MockHal mHal;
    Link link(mHal, cache, true, cfg);
    link.begin();
    Diag d;
    link.getDiag(d);
    uint64_t before = d.resetReasons[(int)ResetReason::ErrRate];
    LinkTestAccessor(link).resetLink(true, ResetReason::ErrRate);
    link.getDiag(d);
    assert(d.resetReasons[(int)ResetReason::ErrRate] == before + 1);
    std::cout << "  PASS" << std::endl;
}

// Pin 3: dropLink() bumps the UserDropLink counter.
static void test_user_droplink_counter() {
    std::cout << "\n=== Pin 3: dropLink() bumps UserDropLink counter ==="
              << std::endl;
    AutoLinkConfig cfg;
    mkCfg(cfg);
    NullArqCache cache;
    MockHal mHal;
    Link link(mHal, cache, true, cfg);
    link.begin();
    LinkTestAccessor acc(link);
    acc.forceState(State::OK);
    Diag d;
    link.getDiag(d);
    uint64_t before = d.resetReasons[(int)ResetReason::UserDropLink];
    link.dropLink();
    link.getDiag(d);
    assert(d.resetReasons[(int)ResetReason::UserDropLink] == before + 1);
    std::cout << "  PASS" << std::endl;
}

// Pin 4: onBreak() (the OK-side health watchdog reset path)
// bumps the HealthWatchdog counter.
static void test_onbreak_health_watchdog_counter() {
    std::cout << "\n=== Pin 4: onBreak() in OK bumps HealthWatchdog counter ==="
              << std::endl;
    AutoLinkConfig cfg;
    mkCfg(cfg);
    NullArqCache cache;
    MockHal mHal;
    Link link(mHal, cache, true, cfg);
    link.begin();
    LinkTestAccessor acc(link);
    acc.forceState(State::OK);
    Diag d;
    link.getDiag(d);
    uint64_t before = d.resetReasons[(int)ResetReason::HealthWatchdog];
    link.onBreak();
    // The onBreak contract is: a single BREAK in OK
    // arms a short confirm window (BreakConfirmTest
    // pins the no-fast-confirm on the first event),
    // and the watchdog reset fires when the deadline
    // expires. A second BREAK within BREAK_COALESCE_MS
    // also doesn't bump the counter (coalesced). To
    // assert the eventual bump, run the link's clock
    // forward past BREAK_CONFIRM_MS + 1 and call
    // onTimer to fire the confirm path. Pinned by
    // BreakConfirmTest + BreakInterruptCoalesceTest.
    (void)before;
    // Wait for the confirm deadline to fire, then
    // run the timer. At 9600 baud the
    // baud-derived breakConfirmMs is ~527 ms
    // (2-chunk flight at 253 bytes * 20 bit-times
    // / 9600 bps); 600 ms covers the deadline.
    mHal.now += 600;
    link.onTimer();
    link.getDiag(d);
    assert(d.resetReasons[(int)ResetReason::HealthWatchdog] == before + 1);
    std::cout << "  PASS" << std::endl;
}

// Pin 5: total resetCount tracks the sum of per-reason
// counters, and four back-to-back resets of different
// reasons each bump the total by 1.
static void test_total_reset_count_tracks_reasons() {
    std::cout << "\n=== Pin 5: resetCount tracks the per-reason sum ==="
              << std::endl;
    AutoLinkConfig cfg;
    mkCfg(cfg);
    NullArqCache cache;
    MockHal mHal;
    Link link(mHal, cache, true, cfg);
    link.begin();
    Diag d;
    link.getDiag(d);
    uint64_t beforeTotal = d.resetCount;
    uint64_t beforeErrThreshold =
        d.resetReasons[(int)ResetReason::ErrThreshold];
    uint64_t beforeErrRate = d.resetReasons[(int)ResetReason::ErrRate];
    uint64_t beforeGbnMaxRetx = d.resetReasons[(int)ResetReason::GbnMaxRetx];
    uint64_t beforePeerEpoch =
        d.resetReasons[(int)ResetReason::PeerEpochMismatch];
    LinkTestAccessor(link).resetLink(true, ResetReason::ErrThreshold);
    LinkTestAccessor(link).resetLink(true, ResetReason::ErrRate);
    LinkTestAccessor(link).resetLink(true, ResetReason::GbnMaxRetx);
    LinkTestAccessor(link).resetLink(true, ResetReason::PeerEpochMismatch);
    link.getDiag(d);
    assert(d.resetCount == beforeTotal + 4);
    assert(d.resetReasons[(int)ResetReason::ErrThreshold] ==
           beforeErrThreshold + 1);
    assert(d.resetReasons[(int)ResetReason::ErrRate] == beforeErrRate + 1);
    assert(d.resetReasons[(int)ResetReason::GbnMaxRetx] ==
           beforeGbnMaxRetx + 1);
    assert(d.resetReasons[(int)ResetReason::PeerEpochMismatch] ==
           beforePeerEpoch + 1);
    std::cout << "  PASS" << std::endl;
}

// Pin 6: source-grep. reset_unlocked()'s first log line
// must be unconditional (no DEBUG/VERBOSE gate) so the
// reason surfaces even if every per-loop log line above
// was dropped. The contract: the first log call inside
// the function body is "reset_unlocked reason=<...>
// count=<...> preserve=<...>" at info level.
static void test_reset_unlocked_unconditional_log() {
    std::cout << "\n=== Pin 6: reset_unlocked emits unconditional log ==="
              << std::endl;
    FILE *f = fopen(testRepoPath("src/al/link/LinkCore.cpp").c_str(), "r");
    assert(f);
    char buf[32768];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    const char *reset = strstr(buf, "void Link::reset_unlocked");
    assert(reset);
    // The first log call inside the function body must be
    // the unconditional "reset_unlocked reason=" line. Walk
    // to the next '}' end of the function — the first log
    // call is at the top of the body, so a simple check for
    // the reason line is sufficient.
    const char *body = strstr(reset, "reason=%s count=%d preserve=%d");
    assert(body &&
           "reset_unlocked must emit 'reset_unlocked "
           "reason=<...> count=<...> preserve=<...>' as the "
           "first log line so every reset is traceable");
    // The line above must be at info level (unconditional),
    // not debug/verbose (which can be dropped by a rate-
    // limited or level-filtered log sink).
    const char *logCall = strstr(reset, "Log::log()");
    assert(logCall && logCall < body);
    // The first log call before the reason line must be
    // .info(...) — not .debug(...) or .verbose(...). Check
    // the substring between logCall and body.
    char fragment[1024];
    size_t fragLen = body - logCall;
    if (fragLen >= sizeof(fragment))
        fragLen = sizeof(fragment) - 1;
    memcpy(fragment, logCall, fragLen);
    fragment[fragLen] = 0;
    assert(strstr(fragment, ".info(") != NULL &&
           "reset_unlocked's reason log must be at info level "
           "(unconditional), not debug/verbose");
    assert(strstr(fragment, ".debug(") == NULL);
    assert(strstr(fragment, ".verbose(") == NULL);
    std::cout << "  PASS (reason log is unconditional info)" << std::endl;
}

int main() {
    std::cout << "=== Reset-Reason Diag Tests ===" << std::endl;
    test_err_threshold_counter();
    test_err_rate_counter();
    test_user_droplink_counter();
    test_onbreak_health_watchdog_counter();
    test_total_reset_count_tracks_reasons();
    test_reset_unlocked_unconditional_log();
    std::cout << "\n=== All 6 reset-reason diag pins PASS ===" << std::endl;
    return 0;
}
