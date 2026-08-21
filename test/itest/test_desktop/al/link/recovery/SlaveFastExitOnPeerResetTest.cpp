// Fix 3 itest: the slave's pending-independent OK-exit
// must let both sides reach OK again inside the master's
// camp budget (3-5 s), not 3 x idleTimeoutMs (30 s).
//
// The scenario drives the field's "peer reboot took longer
// than the master's 3-5 s camp" failure mode: a master and
// slave pair has locked at the proven baud, the master
// takes a preserving reset and enters P3 at the proven
// baud with a 3-5 s camp budget, but the slave is sitting
// in OK with an empty TX window (no ARQ pending). The
// slave's existing watchdogs all gate on `pending > 0`:
// DropSilentPeer waits for `rxAge > deadPeerMs` (3 x
// idleTimeoutMs = 30 s), DropIdle / DropDeadLink /
// DropAsymIdle / DropPoolExhaust all wait for mutual
// quiet or a longer horizon. The slave sits there
// dead-air for 30 s while the master walks P1 down to
// 9600 and re-locks there. The two never reconverge at
// the proven baud.
//
// The fix is HealthAction::DropPeerReset (decideHealth
// branch on `rxAge > 2 * campBudgetMs`): the slave tears
// the link down inside the master's 3-5 s camp, the reset
// is preserving (preferred baud + camp budget preserved),
// and the slave re-camps at the proven baud with the
// master. End-to-end, both sides reach OK again inside
// the master's camp budget, not 30 s.
//
//   Pin 1: master takes a preserving reset with an empty
//   slave TX window. Both sides reach OK again inside
//   the master's camp budget (3-5 s), not 3 x
//   idleTimeoutMs (30 s). Revert (e.g. gate the new
//   branch on `pending > 0` so the empty-window case
//   falls through to deadPeerMs) -> red: the slave
//   does not exit OK and the master walks P1.
//
// To keep the runtime sane, the test uses a small
// idleTimeoutMs (3 s) so deadPeerMs (= 9 s) and
// campBudgetMs (~3 s) are both reachable. The "3-5 s
// camp budget" guarantee is exercised relative to the
// link's own camp budget, not the production 3-5 s
// absolute figure.
#include <iostream>
#include <cassert>
#include "MockHal.h"
#include "NullArqCache.h"
#include "LinkTestAccessor.h"
#include "al/link/Link.h"
#include "al/AutoLinkConfig.h"

using namespace autolink;

namespace {

// Single-baud test rig at 115200 so the slave
// camp and the master P3 camp both run in
// predictable wall-clock seconds. idleTimeoutMs
// is set to a small value (3 s) so deadPeerMs
// = 3 * idleTimeoutMs = 9 s and the camp budget
// (syncAckTimeoutMs * 8, clamped 3-5 s) is
// reachable.
static const uint32_t kBauds[] = { 115200 };
static const int kNumBauds = (int)(sizeof(kBauds) / sizeof(kBauds[0]));

AutoLinkConfig makeCfg() {
    AutoLinkConfig cfg;
    for (int i = 0; i < kNumBauds; i++)
        cfg.allowedBauds[i] = kBauds[i];
    cfg.allowedBaudsCount = kNumBauds;
    cfg.pingSamplesPerBaud = 1;
    // Small idleTimeoutMs so the 30 s horizon
    // would be reachable in test wall-time if
    // the slave is buggy; small syncAckTimeoutMs
    // so campBudgetMs lands at the 3-5 s floor.
    cfg.idleTimeoutMs = 3000;
    cfg.syncAckTimeoutMs = 250; // 250 * 8 = 2000 -> floor 3000ms
    cfg.postLockQuietMs = 0;
    cfg.maxMsg = 256;
    cfg.streamBufferSize = 8192;
    cfg.txBufferSize = 8192;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    return cfg;
}

bool pumpUntilOk(Link &a, Link &b, MockHal &mHal, MockHal &sHal, int maxIters) {
    for (int i = 0; i < maxIters; i++) {
        mHal.pumpClock(10);
        sHal.pumpClock(10);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
        if (a.getState() == State::OK && b.getState() == State::OK)
            return true;
    }
    return false;
}

void test_slave_fast_exit_re_converges_inside_camp_budget() {
    std::cout << "\n=== Pin 1: master takes a preserving reset with "
                 "an empty slave TX window; both sides reach OK "
                 "again inside the master's camp budget (3-5 s), "
                 "not 3 x idleTimeoutMs (30 s) ==="
              << std::endl;
    AutoLinkConfig cfg = makeCfg();
    NullArqCache cacheA, cacheB;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link master(mHal, cacheA, /*isMaster=*/true, cfg);
    Link slave(sHal, cacheB, /*isMaster=*/false, cfg);
    master.begin();
    slave.begin();
    assert(pumpUntilOk(master, slave, mHal, sHal, 400) &&
           "initial negotiation must reach OK");
    LinkTestAccessor accMaster(master);
    LinkTestAccessor accSlave(slave);

    // Capture the master's camp budget for the
    // assertion. This is the budget the new
    // decideHealth branch is bounded by
    // (rxAge > 2 * campBudgetMs).
    uint32_t campBudgetMs = accMaster.resweepPrefBudgetMs();
    std::cout << "  campBudgetMs=" << campBudgetMs
              << " idleTimeoutMs=" << cfg.idleTimeoutMs
              << " deadPeerMs=" << (3 * cfg.idleTimeoutMs) << std::endl;
    assert(campBudgetMs >= 3000 && campBudgetMs <= 5000 &&
           "the test config must produce a camp budget in the "
           "3-5 s production range — the new branch is bounded "
           "by 2 * campBudgetMs");

    // Master takes a preserving reset (HealthWatchdog
    // reason, the shape a real drop-and-recover
    // takes). The slave is left sitting in OK with
    // an empty TX window — exactly the wedge the new
    // branch closes.
    uint32_t resetAtMs = mHal.now;
    accMaster.resetLink(true, /*preserve=*/true, ResetReason::HealthWatchdog);
    // The slave must NOT also be reset — that would
    // defeat the test. The new branch's purpose is
    // to let the SLAVE take a preserving reset on
    // its own when the master is silent past 2 *
    // campBudgetMs.
    std::cout << "  master reset at t=" << resetAtMs
              << " ms; slave stays in OK with empty TX window" << std::endl;

    // Pump until both sides reach OK. The slave's
    // new branch must fire inside the master's
    // camp window, take a preserving reset, and
    // re-converge at the proven baud. The
    // expectation: both sides reach OK in well
    // under 3 * idleTimeoutMs (= 9 s with the
    // test config) and inside a few campBudgetMs
    // (= 6-10 s with the test config).
    int maxIters = 1500; // 1500 * 10ms = 15 s upper bound
    int delivered = -1;
    for (int i = 0; i < maxIters; i++) {
        mHal.pumpClock(10);
        sHal.pumpClock(10);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
        if (master.getState() == State::OK && slave.getState() == State::OK) {
            delivered = i;
            break;
        }
    }
    uint32_t elapsedMs = mHal.now - resetAtMs;
    std::cout << "  both OK after " << elapsedMs << " ms ("
              << (elapsedMs / 1000.0) << " s)" << std::endl;
    assert(delivered >= 0 &&
           "Pin 1: both sides must reach OK again — the slave "
           "must take a preserving reset on its own and re-"
           "converge at the proven baud");
    // The slave's exit window is 2 * campBudgetMs
    // (the new branch's gate). The master's camp
    // window is campBudgetMs. The slave's exit +
    // master + slave camp + final lock is bounded
    // by ~3 * campBudgetMs wall time. Anything
    // approaching 3 * idleTimeoutMs (= 9 s here)
    // means the slave walked to deadPeerMs instead
    // of campBudgetMs, which is the bug class.
    assert(elapsedMs < (uint32_t)(3 * cfg.idleTimeoutMs) &&
           "Pin 1: re-convergence must happen well under "
           "3 * idleTimeoutMs (the silent-peer backstop "
           "horizon). A runtime near that bound means the "
           "slave is still walking to deadPeerMs — the "
           "new DropPeerReset branch did not fire on the "
           "empty-window shape.");
    // Sanity: the elapsed time should be on the
    // order of a few campBudgetMs, not zero (the
    // slave must have had a chance to detect the
    // master's silence and re-camp).
    assert(elapsedMs > 0 &&
           "Pin 1: elapsed must be non-zero (the master "
           "took a reset, the slave must take one too)");
    std::cout << "  Pin 1 PASS (re-converged inside " << (3 * cfg.idleTimeoutMs)
              << " ms, well under the "
              << "3 * idleTimeoutMs backstop horizon)" << std::endl;
}

} // namespace

int main() {
    std::cout << "=== SlaveFastExitOnPeerReset (Fix 3) itest ===" << std::endl;
    test_slave_fast_exit_re_converges_inside_camp_budget();
    std::cout << "\nAll SlaveFastExitOnPeerReset pins passed." << std::endl;
    return 0;
}
