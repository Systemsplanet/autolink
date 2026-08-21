// Regression: slave's pending-independent exit from OK.
// The existing watchdogs all gate on `pending > 0` (so the
// empty-window shape — no ARQ in flight, no peer frames,
// but the link is "OK" on paper) walks all the way to
// deadPeerMs (= 3 * idleTimeoutMs = 30 s) before
// DropSilentPeer fires. A slave in that shape is the
// field's "peer reboot took longer than the master's
// 3-5 s camp budget" failure mode: the master is sitting
// in P3 at preferredBaud_ for `resweepPrefBudgetMs_unlocked`
// (3-5 s) and the slave is sitting in OK with an empty TX
// window waiting for the master. The slave's 30 s horizon
// is dead air — the master has already walked P1 down
// and re-locked at 9600 by then.
//
// The fix is a new HealthAction::DropPeerReset branch
// in decideHealth, gated on `rxAge > 2 * campBudgetMs`
// (the master's camp duration in ms, exposed via
// HealthState::campBudgetMs). The slave tears the link
// down at 2 * 5 s = 10 s in the worst case, well inside
// the master's 3-5 s camp. The reset is preserving
// (preferred baud + camp budget preserved) so the slave
// re-camps at the proven baud instead of walking P1.
//
// Pinned by table test (this file, unit) and by the two-
// node itest under itest/test_desktop/al/link/recovery/
// (SlaveFastExitOnPeerResetTest.cpp) that drives a real
// reset and asserts both sides reach OK again inside
// the master's camp budget (3-5 s), not 3 * idleTimeoutMs
// (30 s).
//
//   Pin 1: decideHealth returns DropPeerReset for an
//   ASYNC, lastRxMs != 0, rxAge > 2 * campBudgetMs
//   shape — no pending, no frameErrs. The exact slave
//   wedge.
//
//   Pin 2: rxAge just under 2 * campBudgetMs -> Keep.
//   One-budget grace period, not immediate fire.
//
//   Pin 3: rxAge past 2 * campBudgetMs but campBudgetMs
//   == 0 -> Keep. Caller gate (matches the rest of the
//   helper's contract).
//
//   Pin 4: SYNC mode with rxAge past 2 * campBudgetMs ->
//   Keep. The new branch is ASYNC-only (SYNC keeps
//   waitForAck semantics; the SYNC mid-message timeout
//   in onSyncAckTimeout is the SYNC peer-gone path).
//
//   Pin 5: rxAge past deadPeerMs -> DropSilentPeer wins
//   over DropPeerReset. The backstop must still trip on
//   a long silence — the new branch is a fast path, not
//   a replacement.
//
//   Pin 6: source-grep on the campBudgetMs field in
//   LinkHealth.h, the DropPeerReset enum value, the
//   branch in decideHealth, the campBudgetMs population
//   in LinkTimersOk.cpp, and the case label in the
//   switch.
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include <cstring>
#    include <cstdio>
#    include "al/link/timers/LinkHealth.h"
#    include "TestPaths.h"

using namespace autolink;

int main() {
    constexpr int idle = 10000;
    constexpr int deadPeer = 30000;
    constexpr uint32_t now = 100000;
    constexpr uint32_t rto = 500;

    // Common scaffold for a HealthState table row. The
    // new branch gates on (1) lastRxMs != 0, (2) !sync,
    // (3) !isMaster (the branch is slave-only — a master
    // on the same shape is doing the camp, not waiting
    // for a camp), (4) campBudgetMs > 0, (5) rxAge > 2 *
    // campBudgetMs, (6) NOT past deadPeerMs (silent-peer
    // backstop wins). Defaults below pick a pre-
    // DropPeerReset shape so each pin sets one field.
    // isMaster defaults to false (slave) — Pin 1b flips
    // it to true to pin the master-on-the-same-shape
    // false-positive.
    auto st = [](uint32_t rx, int camp, bool sync, bool master) {
        HealthState h;
        h.lastRxMs = rx;
        h.campBudgetMs = (uint32_t)camp;
        h.sync = sync;
        h.isMaster = master;
        h.rtoMs = rto;
        return h;
    };

    struct Row {
        HealthState h;
        HealthAction expect;
        const char *why;
    };
    const Row rows[] = {
        // Pin 1: the exact slave wedge. lastRxMs=1
        // (link has been alive), campBudgetMs=5000
        // (master's max camp), rxAge=10001 (just past
        // 2*5000=10000), no pending, no frameErrs,
        // ASYNC, slave. -> DropPeerReset.
        { st(now - 10001, 5000, false, false), HealthAction::DropPeerReset,
          "Pin 1: ASYNC slave lastRxMs!=0 rxAge>2*campBudgetMs no pending "
          "-> DropPeerReset (the slave wedge)" },
        // Pin 1b: the same shape but master -> Keep
        // (the new branch is slave-only; a master on
        // this shape is the side doing the camp, not
        // waiting for a camp, and a master exit would
        // false-positive on a healthy bidirectional
        // pause).
        { st(now - 10001, 5000, false, true), HealthAction::Keep,
          "Pin 1b: master on the same shape -> Keep "
          "(the new branch is slave-only — a master on "
          "this shape is the side doing the camp, not "
          "waiting for a camp)" },
        // Pin 2: rxAge just under 2*campBudgetMs -> Keep.
        { st(now - 9999, 5000, false, false), HealthAction::Keep,
          "Pin 2: rxAge just under 2*campBudgetMs -> Keep "
          "(one-budget grace period)" },
        // Pin 3: campBudgetMs == 0 -> the new branch is
        // disabled (caller gate). rxAge past the implied
        // budget is irrelevant, BUT the rxAge here must
        // also be under deadPeerMs so DropSilentPeer
        // doesn't fire first (we're testing the
        // campBudgetMs==0 disable, not the silent-peer
        // backstop priority).
        { st(now - 15000, 0, false, false), HealthAction::Keep,
          "Pin 3: campBudgetMs==0 disables the watchdog "
          "(caller gate; matches peerStalledMs shape)" },
        // Pin 4: SYNC mode -> the new branch is ASYNC-
        // only. SYNC's mid-message timeout is the SYNC
        // peer-gone path, not this watchdog. rxAge past
        // 2*campBudgetMs in SYNC must NOT fire
        // DropPeerReset.
        { st(now - 10001, 5000, true, false), HealthAction::Keep,
          "Pin 4: SYNC mode rxAge>2*campBudgetMs -> Keep "
          "(ASYNC-only branch; SYNC keeps waitForAck)" },
        // Pin 5: rxAge past deadPeerMs -> DropSilentPeer
        // wins over DropPeerReset. The new branch is a
        // FAST exit, not a replacement. deadPeerMs =
        // 3 * idleTimeoutMs = 30000, so rxAge=31000
        // (past 2*5000 AND past 30000) must still fire
        // the existing silent-peer backstop.
        { st(now - 31000, 5000, false, false), HealthAction::DropSilentPeer,
          "Pin 5: rxAge>deadPeerMs wins over DropPeerReset "
          "(backstop must still fire on long silence)" },
    };
    int n = 0;
    for (const Row &r : rows) {
        HealthAction got = decideHealth(r.h, now, idle, deadPeer, r.h.rtoMs);
        if (got != r.expect) {
            std::cout << "FAIL: " << r.why << " got=" << (int)got
                      << " expect=" << (int)r.expect << std::endl;
            assert(false);
        }
        n++;
    }
    std::cout << "PASS: DropPeerReset table (" << n << " rows)" << std::endl;

    // Pin 6: source-grep on the load-bearing shapes in
    // LinkHealth.h (campBudgetMs field + DropPeerReset
    // enum value + decideHealth branch) and in
    // LinkTimersOk.cpp (h.campBudgetMs population +
    // switch case label).
    {
        FILE *f =
            fopen(testRepoPath("src/al/link/timers/LinkHealth.h").c_str(), "r");
        assert(f);
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = (char *)malloc((size_t)sz + 1);
        assert(buf);
        size_t got = fread(buf, 1, (size_t)sz, f);
        buf[got] = 0;
        fclose(f);
        // campBudgetMs field on HealthState.
        assert(strstr(buf, "uint32_t campBudgetMs") != NULL &&
               "Pin 6: LinkHealth.h must declare "
               "HealthState::campBudgetMs");
        // DropPeerReset enum value.
        assert(strstr(buf, "DropPeerReset") != NULL &&
               "Pin 6: LinkHealth.h must declare "
               "HealthAction::DropPeerReset");
        // The branch in decideHealth: 2 * campBudgetMs
        // (not 1 * campBudgetMs — the one-budget grace
        // period is part of the contract).
        const char *branch = strstr(buf, "rxAge > 2u * h.campBudgetMs");
        if (!branch)
            branch = strstr(buf, "rxAge > 2 * h.campBudgetMs");
        assert(branch &&
               "Pin 6: LinkHealth.h decideHealth must gate on "
               "`rxAge > 2 * h.campBudgetMs` (the one-budget "
               "grace period is part of the contract — a "
               "single-budget fire would false-positive on a "
               "single budget's worth of asymmetric jitter)");
        // Branch must come AFTER the DropSilentPeer
        // check and BEFORE the `if (h.sync) return Keep;`
        // short-circuit (the spec calls for a fast exit
        // in the ASYNC-only path; the SYNC peer-gone
        // path is onSyncAckTimeout, not this branch).
        const char *silentPeerCheck =
            strstr(buf, "return HealthAction::DropSilentPeer");
        assert(silentPeerCheck);
        const char *peerResetBranch = strstr(silentPeerCheck, "DropPeerReset");
        assert(peerResetBranch &&
               "Pin 6: DropPeerReset branch must come AFTER the "
               "DropSilentPeer check (DropSilentPeer wins on a "
               "long silence — see Pin 5)");
        const char *syncKeep = strstr(peerResetBranch, "if (h.sync)");
        assert(syncKeep &&
               "Pin 6: DropPeerReset branch must come BEFORE the "
               "`if (h.sync)` short-circuit (the new branch "
               "is ASYNC-only; SYNC keeps waitForAck semantics)");
        free(buf);
    }
    {
        FILE *f = fopen(
            testRepoPath("src/al/link/timers/LinkTimersOk.cpp").c_str(), "r");
        assert(f);
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = (char *)malloc((size_t)sz + 1);
        assert(buf);
        size_t got = fread(buf, 1, (size_t)sz, f);
        buf[got] = 0;
        fclose(f);
        // h.campBudgetMs must be populated from
        // resweepPrefBudgetMs_unlocked so the watchdog
        // and the camp share one source of truth.
        assert(strstr(buf, "h.campBudgetMs = resweepPrefBudgetMs_unlocked()") !=
                   NULL &&
               "Pin 6: LinkTimersOk.cpp applyHealth_unlocked must "
               "populate h.campBudgetMs from "
               "resweepPrefBudgetMs_unlocked()");
        // The switch case must log a distinct warning
        // so the operator can tell DropPeerReset from
        // HealthWatchdog at a glance.
        assert(strstr(buf, "case HealthAction::DropPeerReset:") != NULL &&
               "Pin 6: LinkTimersOk.cpp must handle the new "
               "DropPeerReset verdict in applyHealth_unlocked's "
               "switch");
        free(buf);
    }
    std::cout << "PASS: DropPeerReset source pin" << std::endl;

    std::cout << "\n=== All SlaveFastExitOnPeerReset pins PASS ==="
              << std::endl;
    return 0;
}

#endif
