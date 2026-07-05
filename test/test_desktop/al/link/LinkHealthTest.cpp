// Pins decideHealth (LinkHealth.h): every OK-state
// keep/drop watchdog in one truth table. Toggle-off
// (re-gate any check by mode, or return Keep for a
// drop row) turns the matching row red. Rows cover
// the three shipped wedges: the ASYNC pool-reject
// stall, the SYNC lost-mid-message-ACK desync (both
// DropTxStall), the asymmetric peer-gone drop, the
// clean-quiet-link false positive (Keep), and pool
// exhaustion.
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
                 uint64_t errs, bool full, bool sync) {
        HealthState h;
        h.rejFirstMs = rf;
        h.rejLastMs = rl;
        h.lastRxMs = rx;
        h.lastTxMs = tx;
        h.pending = pend;
        h.frameErrs = errs;
        h.poolFull = full;
        h.sync = sync;
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
        { st(0, 0, now, now, 0, 0, false, true),
          HealthAction::Keep, "SYNC sees only the streak: quiet keeps" },
        // asymmetric idle (ASYNC only)
        { st(0, 0, now - 400, now - 100, 1, 0, false, false),
          HealthAction::DropAsymIdle, "TX pending, RX silent -> drop" },
        { st(0, 0, now - 400, now - 100, 0, 0, false, false),
          HealthAction::Keep, "RX silent but nothing pending holds" },
        { st(0, 0, now - 400, now - 2000, 1, 0, false, false),
          HealthAction::Keep, "TX not recent: asym gate holds" },
        { st(0, 0, now - 200, now - 100, 1, 0, false, false),
          HealthAction::Keep, "RX fresh: asym gate holds" },
        { st(0, 0, now - 400, now - 100, 1, 0, false, true),
          HealthAction::Keep, "SYNC skips asym (inline waitForAck)" },
        // symmetric idle (ASYNC only)
        { st(0, 0, now - 11000, now - 11000, 1, 0, false, false),
          HealthAction::DropIdle, "quiet past idle with pending -> drop" },
        { st(0, 0, now - 11000, now - 11000, 0, 3, false, false),
          HealthAction::DropIdle, "quiet past idle with frameErrs -> drop" },
        { st(0, 0, now - 11000, now - 11000, 0, 0, false, false),
          HealthAction::Keep, "clean quiet link is just idle" },
        { st(0, 0, now - 11000, now - 9000, 1, 0, false, false),
          HealthAction::Keep, "TX inside idle window holds" },
        { st(0, 0, now - 11000, now - 11000, 1, 0, false, true),
          HealthAction::Keep, "SYNC skips symmetric idle" },
        // pool exhaustion (ASYNC only)
        { st(0, 0, now, now, 1, 0, true, false),
          HealthAction::DropPoolExhaust, "pool full + pending -> drop" },
        { st(0, 0, now, now, 0, 0, true, false),
          HealthAction::Keep, "pool full, nothing pending holds" },
        { st(0, 0, now, now, 1, 0, true, true),
          HealthAction::Keep, "SYNC never populates the pool" },
        // priority: stall outranks the ASYNC checks
        { st(now - 11000, now - 500, now - 400, now - 100, 1, 0, true, false),
          HealthAction::DropTxStall, "stall wins over asym/pool" },
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
