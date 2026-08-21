// Regression pins for four of the post-soak field-fix batch
// fixes that don't already have dedicated coverage elsewhere
// (AL-01/02/03/09/13/13b are covered by LogTest.cpp,
// ResetReasonDiagTest.cpp / the kReasonNames table's own
// static_assert, EpochBounceTest.cpp + PeerResyncOnMissedBreakTest.cpp,
// and LastValidRxMsTest.cpp respectively — see the source
// comments at each fix site).
//
//   AL-06 (runtime + source-grep): the peer-reset watchdog log
//   line prints thresholdMs = 2 * campBudgetMs, the value the
//   verdict actually compares against.
//
//   AL-07 (runtime): a BREAK-storm window vetoes the P3
//   preferredBaud_ camp on the next preserving reset.
//
//   AL-10 (runtime): LinkArq::gbnBaseStrForLog returns "N/A"
//   when gbnActive_ is false (SYNC / inactive GBN), not a
//   constant "0".
//
//   AL-14 (source-grep): the three per-ACK/per-NAK/GBN-base
//   lines in LinkRx.cpp that were the field-observed flood are
//   at verbose, not debug.
#ifndef ARDUINO

#    include <cassert>
#    include <cstring>
#    include <fstream>
#    include <iostream>
#    include <sstream>
#    include <string>
#    include "MockHal.h"
#    include "LinkTestAccessor.h"
#    include "NullArqCache.h"
#    include "TestPaths.h"
#    include "al/link/arq/LinkArq.h"

using namespace autolink;

namespace {

std::string readFile(const std::string &path) {
    std::ifstream f(path);
    if (!f.good())
        return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static const int kBauds[] = { 9600, 115200 };
static const int kNumBauds = (int)(sizeof(kBauds) / sizeof(kBauds[0]));

void bringToOk(Link &a, Link &b, MockHal &mHal, MockHal &sHal) {
    a.begin();
    b.begin();
    for (int i = 0; i < 400; i++) {
        mHal.pumpClock(50);
        sHal.pumpClock(50);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
        if (a.getState() == State::OK && b.getState() == State::OK)
            return;
    }
    assert(false && "failed to bring two nodes to OK");
}

// AL-06.
void test_peer_reset_watchdog_threshold_source_and_math() {
    std::cout << "\n=== AL-06: peer-reset watchdog prints the actual "
                 "threshold ===\n";
    std::string src =
        readFile(testRepoPath("src/al/link/timers/LinkTimersOk.cpp"));
    assert(!src.empty());
    auto pos = src.find("peer-reset watchdog -> drop");
    assert(pos != std::string::npos);
    // Look at the surrounding ~600 bytes for both the format
    // string and the value expression — must be the same
    // multiplier the health machine actually gates on
    // (LinkHealth.h: rxAge > 2u * campBudgetMs).
    std::string window = src.substr(pos > 300 ? pos - 300 : 0, 900);
    assert(window.find("thresholdMs=%lu") != std::string::npos &&
           "AL-06: the log line must carry a thresholdMs field");
    assert(window.find("2u * h.campBudgetMs") != std::string::npos &&
           "AL-06: thresholdMs must be computed as 2u * campBudgetMs, "
           "the same multiplier LinkHealth.h's DropPeerReset verdict "
           "actually uses — printing idleTimeoutMs alone (the prior "
           "shape) showed a threshold the verdict never compared "
           "against");
    std::cout << "  PASS (thresholdMs field present, computed as "
                 "2u * campBudgetMs)\n";
}

// AL-07 (runtime): a BREAK storm vetoes the next preserving
// reset's P3 camp.
void test_break_storm_vetoes_next_camp() {
    std::cout << "\n=== AL-07: BREAK storm vetoes the next preserving "
                 "reset's P3 camp ===\n";
    NullArqCache cacheA, cacheB;
    AutoLinkConfig cfg;
    for (int i = 0; i < kNumBauds; i++)
        cfg.allowedBauds[i] = (uint32_t)kBauds[i];
    cfg.allowedBaudsCount = kNumBauds;
    cfg.pingSamplesPerBaud = 1;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link a(mHal, cacheA, /*isMaster=*/true, cfg);
    Link b(sHal, cacheB, /*isMaster=*/false, cfg);
    bringToOk(a, b, mHal, sHal);

    LinkTestAccessor acc(a);
    acc.setPreferredBaudForTest(0);

    // Baseline: a preserving reset with no storm yet must enter
    // the P3 camp (resweepPrefPending_ true).
    acc.resetLink(true, /*preserve=*/true, ResetReason::GbnMaxRetx);
    assert(acc.resweepPrefPendingForTest() &&
           "precondition: a preserving reset enters the P3 camp when "
           "no BREAK storm has fired this session");

    // Relock, fire a BREAK storm (consumed on the next onTimer()
    // tick, same as production), then try another preserving
    // reset.
    acc.forceState(State::OK);
    a.onBreakStorm();
    a.onTimer();
    acc.setPreferredBaudForTest(0);
    acc.resetLink(true, /*preserve=*/true, ResetReason::GbnMaxRetx);
    assert(!acc.resweepPrefPendingForTest() &&
           "AL-07: a preserving reset after a BREAK storm must NOT "
           "enter the P3 camp — the storm already proved the "
           "preserved baud wrong, re-arming it would re-arm the "
           "master-walks/slave-camps mismatch");
    std::cout << "  PASS (camp entered before the storm, vetoed after)\n";
}

// AL-10 (runtime): gbnBaseStrForLog returns "N/A" when GBN is
// inactive.
void test_gbn_base_str_for_log_na_when_inactive() {
    std::cout << "\n=== AL-10: gbnBaseStrForLog returns N/A when GBN is "
                 "inactive ===\n";
    LinkArq arq;
    assert(!arq.gbnActive() && "precondition: gbnActive_ defaults to false");
    char buf[8];
    const char *s = arq.gbnBaseStrForLog(buf, sizeof(buf));
    assert(std::strcmp(s, "N/A") == 0 &&
           "AL-10: inactive GBN (SYNC mode) must format as N/A, not a "
           "constant 0 that every per-ACK/per-NAK line in a SYNC "
           "session would otherwise repeat");

    arq.setGbnActive(true);
    arq.setGbnBase(42);
    s = arq.gbnBaseStrForLog(buf, sizeof(buf));
    assert(std::strcmp(s, "42") == 0 &&
           "AL-10: active GBN must format the real base value");
    std::cout << "  PASS (N/A when inactive, real value when active)\n";
}

// AL-14 (source-grep): the field-observed flood trio in
// LinkRx.cpp is at verbose, not debug.
void test_linkrx_flood_lines_are_verbose() {
    std::cout << "\n=== AL-14: LinkRx.cpp wire ACK / GBN base / wire NAK "
                 "are verbose, not debug ===\n";
    std::string src = readFile(testRepoPath("src/al/link/io/LinkRx.cpp"));
    assert(!src.empty());

    auto checkVerbose = [&](const char *needle, const char *label) {
        auto pos = src.find(needle);
        assert(pos != std::string::npos && label);
        auto callPos = src.rfind("Log::log().", pos);
        assert(callPos != std::string::npos);
        std::string callLine = src.substr(callPos, pos - callPos);
        assert(callLine.find("verbose") != std::string::npos &&
               callLine.find("debug") == std::string::npos && label);
    };
    checkVerbose("\"wire ACK seq=%u (base=%s, pending=%d, bytesRecvd=%u)\"",
                 "AL-14: per-ACK success line must be verbose — this was "
                 "one leg of the field's 3-line-per-ACK flood at 512000 "
                 "baud (~600/s)");
    checkVerbose("\"GBN base %u -> %u (acked=%u freed=%d pending=%d)\"",
                 "AL-14: GBN-base move line must be verbose — the second "
                 "leg of the same flood");
    checkVerbose("\"wire NAK missing=%u (base=%s, pending=%d)\"",
                 "AL-14: per-NAK success line must be verbose");
    std::cout << "  PASS (all three flood lines are verbose)\n";
}

} // namespace

int main() {
    std::cout << "=== Post-Soak Field-Fix Batch Regression Pins ===\n";
    test_peer_reset_watchdog_threshold_source_and_math();
    test_break_storm_vetoes_next_camp();
    test_gbn_base_str_for_log_na_when_inactive();
    test_linkrx_flood_lines_are_verbose();
    std::cout << "\n=== All 4 pins PASS ===\n";
    return 0;
}

#endif
