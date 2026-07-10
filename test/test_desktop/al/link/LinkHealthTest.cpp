// Pins decideHealth (LinkHealth.h): every OK-state
// keep/drop watchdog in one truth table. Toggle-off
// (re-gate any check by mode, or return Keep for a
// drop row) turns the matching row red. Rows cover
// the four shipped wedges: the ASYNC pool-reject
// stall, the SYNC lost-mid-message-ACK desync (both
// DropTxStall), the asymmetric peer-gone drop, the
// clean-quiet-link false positive (Keep), pool
// exhaustion, and the dead-link backstop (a
// mutually-quiet link with an in-flight op — the
// SYNC-mode recovery path the bench log showed
// was missing).
#ifndef ARDUINO

#    include <cassert>
#    include <iostream>
#    include "al/link/LinkHealth.h"

using namespace autolink;

int main() {
    constexpr int idle = 10000;
    constexpr uint32_t now = 100000;
    struct Row {
        HealthState h;
        HealthAction expect;
        const char *why;
    };
    auto st = [](uint32_t rf, uint32_t rl, uint32_t rx, uint32_t tx, int pend,
                 uint64_t errs, bool full, bool sync, uint32_t rto = 0) {
        HealthState h;
        h.rejFirstMs = rf;
        h.rejLastMs = rl;
        h.lastRxMs = rx;
        h.lastTxMs = tx;
        h.pending = pend;
        h.frameErrs = errs;
        h.poolFull = full;
        h.sync = sync;
        h.rtoMs = rto;
        return h;
    };
    const Row rows[] = {
        // tx-reject streak (both modes)
        { st(now - 11000, now - 500, now, now, 0, 0, false, false),
          HealthAction::DropTxStall, "ASYNC live streak past idle -> drop" },
        { st(now - 11000, now - 500, now, now, 0, 0, false, true),
          HealthAction::DropTxStall, "SYNC live streak past idle -> drop" },
        { st(now - 11000, now - 11000, now, now, 0, 0, false, true),
          HealthAction::Keep, "stale streak (no recent reject) holds" },
        { st(now - 9000, now - 500, now, now, 0, 0, false, false),
          HealthAction::Keep, "young streak holds" },
        { st(0, 0, now, now, 0, 0, false, true), HealthAction::Keep,
          "SYNC sees only the streak: quiet keeps" },
        // asymmetric idle (ASYNC only)
        { st(0, 0, now - 400, now - 100, 1, 0, false, false),
          HealthAction::DropAsymIdle, "TX pending, RX silent -> drop" },
        { st(0, 0, now - 400, now - 100, 0, 0, false, false),
          HealthAction::Keep, "RX silent but nothing pending holds" },
        { st(0, 0, now - 400, now - 2000, 1, 0, false, false),
          HealthAction::Keep, "TX not recent: asym gate holds" },
        { st(0, 0, now - 200, now - 100, 1, 0, false, false),
          HealthAction::Keep, "RX fresh: asym gate holds" },
        { st(0, 0, now - 400, now - 100, 1, 0, false, true), HealthAction::Keep,
          "SYNC skips asym (inline waitForAck)" },
        // asym retx horizon: silence shorter than
        // 2 x RTO is a backpressured sender waiting
        // on its own sweep, not a gone peer.
        { st(0, 0, now - 400, now - 100, 1, 0, false, false, 500),
          HealthAction::Keep, "RX silence inside 2xRTO holds" },
        { st(0, 0, now - 1100, now - 100, 1, 0, false, false, 500),
          HealthAction::DropAsymIdle, "RX silence past 2xRTO -> drop" },
        // dead-link backstop. The new check runs
        // BEFORE the SYNC short-circuit so SYNC
        // gets a mutual-quiet drop path; for ASYNC
        // it outranks DropIdle (both fire on
        // pending>0 + mutual quiet, but DropDeadLink
        // is narrower: pending>0 only, no frameErrs
        // alternative). Clean mutual quiet (no
        // pending) is still Keep — the "link is
        // idle" case the prior symmetric-idle
        // removal preserved.
        { st(0, 0, now - 11000, now - 11000, 1, 0, false, false),
          HealthAction::DropDeadLink,
          "ASYNC quiet+pending -> DropDeadLink (outranks DropIdle)" },
        { st(0, 0, now - 11000, now - 11000, 1, 0, false, true),
          HealthAction::DropDeadLink,
          "SYNC quiet+pending -> DropDeadLink (was Keep pre-fix)" },
        { st(0, 0, now - 11000, now - 11000, 0, 0, false, true),
          HealthAction::Keep, "SYNC clean mutual quiet (no pending) -> Keep" },
        { st(0, 0, now - 11000, now - 11000, 0, 0, false, false),
          HealthAction::Keep, "ASYNC clean mutual quiet (no pending) -> Keep" },
        { st(0, 0, now - 11000, now - 9000, 1, 0, false, false),
          HealthAction::Keep, "TX inside idle window holds (dead-link off)" },
        { st(0, 0, now - 9000, now - 11000, 1, 0, false, true),
          HealthAction::Keep, "SYNC RX inside idle window holds" },
        { st(0, 0, now, now, 1, 0, false, true), HealthAction::Keep,
          "SYNC fresh rx+tx+pending holds" },
        // DropIdle (ASYNC) — still reachable for
        // frameErrs without pending. The frameErrs
        // case is the only one DropDeadLink doesn't
        // subsume, so this row pins the surviving
        // DropIdle contract.
        { st(0, 0, now - 11000, now - 11000, 0, 3, false, false),
          HealthAction::DropIdle,
          "quiet past idle with frameErrs (no pending) -> DropIdle" },
        // pool exhaustion (ASYNC only). A full pool
        // with a live receiver is routine flood
        // backpressure; the drop needs RX silence
        // past the retx horizon on top. rxAge=now-400
        // is well inside idleTimeoutMs=10000, so
        // DropDeadLink (rxAge > idle) doesn't fire
        // first — the rows still pin DropPoolExhaust.
        { st(0, 0, now, now, 1, 0, true, false), HealthAction::Keep,
          "pool full + RX fresh = backpressure, holds" },
        { st(0, 0, now - 400, now - 1500, 1, 0, true, false),
          HealthAction::DropPoolExhaust, "pool full + RX silent -> drop" },
        { st(0, 0, now - 400, now - 1500, 1, 0, true, false, 500),
          HealthAction::Keep, "pool full, silence inside 2xRTO holds" },
        { st(0, 0, now - 1100, now - 1500, 1, 0, true, false, 500),
          HealthAction::DropPoolExhaust,
          "pool full, silence past 2xRTO -> drop" },
        { st(0, 0, now, now, 0, 0, true, false), HealthAction::Keep,
          "pool full, nothing pending holds" },
        { st(0, 0, now - 400, now, 1, 0, true, true), HealthAction::Keep,
          "SYNC never populates the pool" },
        // priority: stall outranks the dead-link /
        // ASYNC checks. Live reject streak is more
        // urgent than a quiet-pending wedge.
        { st(now - 11000, now - 500, now - 400, now - 100, 1, 0, true, false),
          HealthAction::DropTxStall, "stall wins over asym/pool" },
        { st(now - 11000, now - 500, now - 11000, now - 11000, 1, 0, false,
             true),
          HealthAction::DropTxStall, "stall wins over dead-link (SYNC)" },
    };
    int n = 0;
    for (const Row &r : rows) {
        HealthAction got = decideHealth(r.h, now, idle);
        if (got != r.expect) {
            std::cout << "FAIL: " << r.why << " got=" << (int)got
                      << " expect=" << (int)r.expect << std::endl;
            assert(false);
        }
        n++;
    }
    std::cout << "PASS: decideHealth table (" << n << " rows)" << std::endl;
    return 0;
}

#endif
