// Pins the TX-backpressure stall watchdog (third
// watchdog input in LinkTimers.cpp onTimerOk).
// The wedge shape: ASYNC sendMsg rejects while
// arq_.pendingCount()==0 and frameErrs==0 — the
// asymmetric stuck-send check needs pending>0
// and the symmetric idle check no-ops on a clean
// quiet link, so pre-fix the link hung silently
// forever. Three pins:
//
//   1. The wedge recovers: GBN window full (32
//      pending, admission gate rejects every new
//      send) held past idleTimeoutMs → the
//      OK-state timer drops (state leaves OK,
//      discCount++, BREAK sent). Toggle off
//      (revert the LinkTimers.cpp branch) → red.
//   2. A successful send clears the streak: a
//      healthy link that rejected once, then
//      sent fine, does not drop past
//      idleTimeoutMs.
//   3. A stale streak does not fire: one
//      transient reject with no follow-up past
//      idleTimeoutMs leaves a quiet clean link
//      up (the still-live gate).
//
// Pre-GBN this was driven by filling the ArqCache
// pool directly (hasRoom()==false with pending==0
// — a cache/table split the old three-gate
// admission could hit). Under GBN the admission
// gate is pending-count-vs-window only and the
// pool is sized 2x the window by construction, so
// that split can't happen anymore; the wedge is
// now driven by holding the window itself full.
#ifndef AUTOLINK_HOST_TEST
#    error "Build with -DAUTOLINK_HOST_TEST (see Makefile)"
#endif

#include <cassert>
#include <iostream>
#include "MockHal.h"
#include "LinkTestAccessor.h"
#include "al/AutoLinkConfig.h"
#include "al/link/Link.h"
#include "al/link/arq/ArqCache.h"
#include "al/util/Log.h"

using namespace autolink;

namespace {

// Same shape as LinkSeqSpaceGuardTest's Driver:
// ctor leaves state=OK, no begin() so kickoff()
// doesn't tear OK down to SWP.
struct Driver {
    MockHal hal;
    ArqCache cache{ AUTOLINK_ARQ_PIPELINE_WINDOW };
    AutoLinkConfig cfg;
    Link link;

    explicit Driver(int idleMs) : link(hal, cache, true, mkCfg(idleMs)) {
        assert(link.getState() == State::OK);
        hal.now = 5000;
    }
    AutoLinkConfig mkCfg(int idleMs) {
        cfg.idleTimeoutMs = idleMs;
        cfg.mode = AutoLinkConfig::Mode::ASYNC;
        return cfg;
    }

    // Fill the GBN window: window pending seqs, admission
    // gate rejects any further chunk.
    void fillWindow() {
        LinkTestAccessor acc(link);
        for (uint8_t s = 0; s < (uint8_t)AUTOLINK_ARQ_PIPELINE_WINDOW; s++)
            acc.markAckedPending(s);
    }
};

constexpr int IDLE_MS = 1000;

bool rejectedSend(Link &l) {
    uint8_t buf[32] = {};
    uint8_t seq = 0;
    return !l.sendMsg(buf, sizeof buf, &seq);
}

void test_wedge_drops_and_resweeps() {
    std::cout << "\n=== Pin 1: sustained reject streak -> drop + resweep ==="
              << std::endl;
    Driver d(IDLE_MS);
    d.fillWindow();
    assert(d.link.arqPendingCount() == AUTOLINK_ARQ_PIPELINE_WINDOW);

    // Reject repeatedly across > idleTimeoutMs,
    // ticking the OK-state timer each step —
    // the streak stays live the whole time.
    int breaksBefore = d.hal.sendBreakCalls;
    for (int step = 0; step < 8; step++) {
        assert(rejectedSend(d.link));
        if (d.link.getState() != State::OK)
            break;
        d.hal.now += IDLE_MS / 4;
        d.link.onTimer();
    }
    assert(d.link.getState() != State::OK &&
           "watchdog must drop a wedged sender");
    assert(d.hal.sendBreakCalls > breaksBefore && "drop must BREAK");
    Stats s;
    d.link.getStats(s);
    assert(s.discCount == 1 && "drop must count as a disc");
    std::cout << "  PASS (dropped after " << (unsigned)(d.hal.now - 5000)
              << " ms, disc=1, BREAK sent)" << std::endl;
}

void test_success_clears_streak() {
    std::cout << "\n=== Pin 2: successful send clears the streak ==="
              << std::endl;
    Driver d(IDLE_MS);
    d.fillWindow();
    assert(rejectedSend(d.link));
    // Retire the window (as a peer's cumulative ACK
    // would) so the next send succeeds and the
    // asymmetric stuck-send check stays quiet — this
    // pin isolates the streak-clear path.
    LinkTestAccessor acc(d.link);
    acc.arq().clearAll();
    assert(!rejectedSend(d.link) && "send must succeed with window room");
    acc.arq().clearAll();

    for (int step = 0; step < 8; step++) {
        d.hal.now += IDLE_MS / 4;
        d.link.onTimer();
    }
    assert(d.link.getState() == State::OK &&
           "cleared streak must not drop a healthy link");
    std::cout << "  PASS (link stayed OK past idleTimeoutMs)" << std::endl;
}

void test_stale_streak_does_not_fire() {
    std::cout << "\n=== Pin 3: stale single reject does not drop ==="
              << std::endl;
    Driver d(IDLE_MS);
    d.fillWindow();
    assert(rejectedSend(d.link));
    LinkTestAccessor acc(d.link);
    acc.arq().clearAll();

    // No further sends. Advance well past
    // idleTimeoutMs so the latest reject is
    // itself stale — the still-live gate must
    // keep the quiet clean link up.
    for (int step = 0; step < 12; step++) {
        d.hal.now += IDLE_MS / 2;
        d.link.onTimer();
    }
    assert(d.link.getState() == State::OK &&
           "a stale transient reject must not drop a quiet link");
    std::cout << "  PASS (link stayed OK; stale streak ignored)" << std::endl;
}

} // namespace

int main() {
    std::cout << "=== LinkTxStallWatchdogTest ===" << std::endl;
    test_wedge_drops_and_resweeps();
    test_success_clears_streak();
    test_stale_streak_does_not_fire();
    std::cout << "\nAll TX-stall watchdog pins PASS" << std::endl;
    return 0;
}
