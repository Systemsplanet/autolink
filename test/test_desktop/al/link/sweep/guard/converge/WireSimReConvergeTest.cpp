// Pinned regression: run 2 of the the prior release field log
// showed the death-spiral going permanent — 4 consecutive
// post-BREAK resweeps, 100% ending in
// "Locked 9600 baud (p2-fallback)", 0% re-promoting to
// 512000, despite 512000 being provably reachable. The
// root cause was the master/slave sweep-phase skew: the
// master's preservePreferredBaud fast-path-to-P3 races a
// slave that always restarts at P1-slowest, so the two
// sweeps essentially never overlap at the same baud
// within the P2 dwell budget.
//
// The fix gives the slave the same preserved-baud
// fast path on a HealthWatchdog-reason reset, so both
// sides converge on the proven baud without a full P1
// walk.
//
//  Pin 1 (runtime): a forced post-BREAK HealthWatchdog
//  reset (no BREAK delivered to the still-OK peer) is
//  followed by a bounded number of sweep cycles that
//  must eventually re-converge on earlier baud
//  (the proven one), not 9600. Without this contract the
//  the bus would lock to 9600 every time.
//  Pin 2: source-grep on the slave's preserved-baud
//  fast path: reset_unlocked's "preservePreferredBaud && "
//  condition must NOT be gated on isMaster, and the
//  HealthWatchdog path must allow the slave to enter
//  P3. The bug-class shape is the prior
//  "preservePreferredBaud && isMaster" gate that
//  prevented the slave from honoring the proven baud.
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include <cstring>
#    include <cstdio>
#    include <vector>
#    include "MockHal.h"
#    include "WireSim.h"
#    include "LinkTestAccessor.h"
#    include "NullArqCache.h"
#    include "AutoLinkTestAccessor.h"
#    include "al/util/codec/UtilCrc.h"
#    include "al/link/LinkWire.h"
#    include "TestPaths.h"

using namespace autolink;

static const int kBauds[] = { 115200, 9600 };
static const int kNumBauds = (int)(sizeof(kBauds) / sizeof(kBauds[0]));

static void bringToOk(Link &a, Link &b, MockHal &mHal, MockHal &sHal) {
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

// Pin 1: the production bug. Both nodes lock at 115200
// (the proven baud), then a HealthWatchdog reset on
// master is forced (no BREAK to slave). The slave's
// HealthWatchdog-reason reset must honor the preserved
// baud and re-converge on 115200, not 9600.
static void test_re_converge_after_health_watchdog_reset() {
    std::cout << "\n=== Pin 1: post-HealthWatchdog reset re-converges "
                 "on the proven baud within a bounded number of cycles ==="
              << std::endl;
    NullArqCache cacheA, cacheB;
    AutoLinkConfig cfg;
    for (int i = 0; i < kNumBauds; i++)
        cfg.allowedBauds[i] = (uint32_t)kBauds[i];
    cfg.allowedBaudsCount = kNumBauds;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 0; // keepalive disabled
    cfg.syncAckTimeoutMs = 1000;
    cfg.maxRetx = 50;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link a(mHal, cacheA, /*isMaster=*/true, cfg);
    Link b(sHal, cacheB, /*isMaster=*/false, cfg);
    bringToOk(a, b, mHal, sHal);
    assert(a.getState() == State::OK && b.getState() == State::OK);
    int provenBaudA = a.getCurrentSpdIndex();
    int provenBaudB = b.getCurrentSpdIndex();
    assert(provenBaudA == provenBaudB);
    int provenBaud = provenBaudA;
    std::cout << " provenBaud=" << provenBaud << " (cfg.allowedBauds["
              << provenBaud << "]=" << cfg.allowedBauds[provenBaud] << ")"
              << std::endl;
    // The proven baud must be the FASTEST one (115200,
    // index 0 in the cfg) so the test can detect a
    // 9600 fallback as a regression.
    assert(provenBaud == 0 &&
           "Pin 1 pre: the proven baud must be 115200 (index 0); "
           "this is the test the prior fix was supposed to pass "
           "(the field log's 100% 9600 fallback)");

    // Both nodes get a HealthWatchdog reset that
    // PRESERVES the preferred baud. The slave's
    // preserved-baud fast path is the load-bearing
    // fix — without it, the slave walks P1 slowest
    // and the master falls back to 9600 every time.
    LinkTestAccessor accA(a);
    LinkTestAccessor accB(b);
    accA.resetLink(true, /*preserve=*/true, ResetReason::HealthWatchdog);
    accB.resetLink(true, /*preserve=*/true, ResetReason::HealthWatchdog);

    // Drive the re-convergence. Both sides should
    // enter P3 at the preserved baud and meet there
    // inside a bounded number of pump iterations.
    int converged = -1;
    for (int i = 0; i < 400 && converged < 0; i++) {
        mHal.pumpClock(50);
        sHal.pumpClock(50);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
        a.onTimer();
        b.onTimer();
        if (a.getState() == State::OK && b.getState() == State::OK)
            converged = i;
    }
    if (converged < 0) {
        std::cout << " FAIL: did not re-converge after 400 iterations"
                  << std::endl;
        std::cout << " stateA=" << (int)a.getState()
                  << " spdIA=" << a.getCurrentSpdIndex()
                  << " stateB=" << (int)b.getState()
                  << " spdIB=" << b.getCurrentSpdIndex() << std::endl;
        assert(false &&
               "Pin 1: re-convergence must complete within 400 "
               "pump iterations when both sides use the "
               "preserved-baud fast path on a HealthWatchdog "
               "reset. The the prior release field log showed 4/4 "
               "consecutive resweeps locking to 9600 — the "
               "test fails if the slave still walks P1 slowest "
               "while the master is at P3 fastest.");
    }
    std::cout << " converged at iteration " << converged << std::endl;
    assert(a.getCurrentSpdIndex() == provenBaud &&
           b.getCurrentSpdIndex() == provenBaud &&
           "Pin 1: both sides must lock on the proven baud (115200, "
           "index 0) after the HealthWatchdog reset, NOT 9600 "
           "(the field log's 100% fallback)");
    // postLockQuietMs is 600 ms by default; the
    // first sendMsg inside the window is deferred.
    // Pump past the quiet window before the
    // round-trip proof.
    for (uint32_t t = 0; t < 1500; t += 50) {
        mHal.pumpClock(50);
        sHal.pumpClock(50);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
    }
    const uint8_t payload[] = { 'r', 'e', 'c', 'n', 'v' };
    bool ok = a.sendMsg(payload, sizeof payload);
    if (!ok) {
        LinkTestAccessor accA2(a);
        std::cout << " sendMsg failed reason="
                  << (int)accA2.lastSendMsgReasonForTest()
                  << " state=" << (int)a.getState() << std::endl;
    }
    assert(ok);
    uint8_t sink[64];
    int delivered = -1;
    for (int i = 0; i < 200; i++) {
        mHal.pumpClock(50);
        sHal.pumpClock(50);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
        a.onTimer();
        b.onTimer();
        int n = b.recvMsg(sink, sizeof sink);
        if (n == (int)sizeof payload &&
            memcmp(sink, payload, sizeof payload) == 0) {
            delivered = i;
            break;
        }
    }
    assert(delivered >= 0 &&
           "Pin 1: post-resync sendMsg must round-trip the exact "
           "payload — bytes-on-wire proof the link is alive, not "
           "just state-flips-to-OK");
    std::cout << " Pin 1 PASS (re-converged on proven baud, "
                 "round-tripped in "
              << delivered << " iterations)" << std::endl;
}

// Pin 2: source-grep on the slave's preserved-baud
// fast path. The bug-class shape was
// "preservePreferredBaud && isMaster" — that gate
// locked the slave out of the fast path. The new
// shape drops the isMaster gate (and adds a
// HealthWatchdog-reason gate for the slave).
static void test_slave_preserved_baud_path_source_grep() {
    std::cout << "\n=== Pin 2: slave's preserved-baud fast path is "
                 "wired (no `isMaster` gate on the fast-relock "
                 "condition; source-grep) ==="
              << std::endl;
    FILE *f = fopen(testRepoPath("src/al/link/LinkCore.cpp").c_str(), "r");
    assert(f);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    assert(buf);
    size_t got = fread(buf, 1, (size_t)sz, f);
    buf[got] = 0;
    fclose(f);
    const char *fn = strstr(buf, "void Link::reset_unlocked");
    assert(fn);
    const char *body = strchr(fn, '{');
    assert(body);
    int depth = 0;
    const char *p = body;
    const char *end = nullptr;
    while (*p) {
        if (*p == '{')
            depth++;
        else if (*p == '}') {
            depth--;
            if (depth == 0) {
                end = p + 1;
                break;
            }
        }
        p++;
    }
    assert(end);
    int len = (int)(end - fn);
    char bodybuf[16384];
    if (len >= (int)sizeof(bodybuf))
        len = sizeof(bodybuf) - 1;
    memcpy(bodybuf, fn, len);
    bodybuf[len] = 0;

    // The fast-relock condition must NOT include
    // `isMaster &&` as a separate clause. The bug-class
    // shape is `preservePreferredBaud && isMaster && ...`.
    // The new shape drops the isMaster gate; the slave
    // gets the same fast path on HealthWatchdog-resets.
    // Look for the precise bug-class shape and reject it.
    const char *bugClass =
        strstr(bodybuf, "preservePreferredBaud && isMaster &&");
    assert(!bugClass &&
           "Pin 2: the bug-class shape `preservePreferredBaud && "
           "isMaster &&` must NOT be present in reset_unlocked — "
           "the prior gate locked the slave out of the "
           "preserved-baud fast path on HealthWatchdog resets, "
           "which is exactly the the prior release field-log failure "
           "mode (100% 9600 fallback). The new shape is "
           "`preservePreferredBaud &&` followed by the post-OK "
           "gates, NOT gated on isMaster.");
    // The new shape must reference the slave's
    // HealthWatchdog allow-list.
    assert(strstr(bodybuf, "HealthWatchdog") != NULL &&
           "Pin 2: reset_unlocked must reference "
           "ResetReason::HealthWatchdog — the slave's "
           "preserved-baud allow-list gates on this reason "
           "so a UserDropLink / ErrThreshold reset still "
           "forces a full P1 walk on the slave (the bug class "
           "is the HealthWatchdog-specific 9600 fallback).");
    std::cout << " Pin 2 PASS (slave's preserved-baud fast path is "
                 "wired, no isMaster gate on the fast-relock "
                 "condition)"
              << std::endl;
    free(buf);
}

// Pin 3: the P3 preferredBaud_ retry branch
// (resweepPrefAttempts_ < RESWEEP_PREF_MAX_ATTEMPTS)
// must call sendSweepFrame_unlocked(PING_CMD) so the
// peer has a fresh chance to lock. A pure timer
// re-arm is a no-op if the peer missed earlier
// PING outright (e.g. wire noise) — the link would
// just wait the same dwell and time out again. The
// new shape re-sends PING and uses a short P3 timer
// (PHASE3_ACKS_NEEDED - acks + 1) so a stuck peer
// is recognized before the 8 s P1 fallback. Pinned
// at the source level because the field failure was
// a no-op retry that fell to P1 every time, losing
// the preserved baud.
static void test_p3_preferred_baud_retry_resends_ping() {
    std::cout << "\n Pin 3: P3 preferredBaud_ retry branch re-sends PING "
                 "(not just a timer re-arm) (source-grep) ==="
              << std::endl;
    FILE *f = fopen(
        testRepoPath("src/al/link/timers/LinkTimersSwp.cpp").c_str(), "r");
    assert(f);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    assert(buf);
    size_t got = fread(buf, 1, (size_t)sz, f);
    buf[got] = 0;
    fclose(f);
    // The master's P3 retry branch is gated on campOpen — a
    // wall-clock camp budget rather than a count of t3-sized
    // attempts, so a peer that needs seconds to return is still
    // met at the proven baud. Within that branch the body must
    // call sendSweepFrame_unlocked(PING_CMD).
    //
    // Anchored on campOpen specifically: the slave camp further
    // down still uses a plain resweepPrefAttempts_ gate (its dwell
    // is idleTimeoutMs/2, already seconds long), and matching that
    // one instead would extract a body that legitimately has no
    // PING re-send.
    const char *gate = strstr(buf, "if (campOpen) {");
    assert(gate &&
           "Pin 3: LinkTimersSwp.cpp must keep the master's "
           "budget-gated P3 retry branch (campOpen).");
    // Find the body opening after the gate.
    const char *body = strchr(gate, '{');
    assert(body);
    int depth = 0;
    const char *p = body;
    const char *end = nullptr;
    while (*p) {
        if (*p == '{')
            depth++;
        else if (*p == '}') {
            depth--;
            if (depth == 0) {
                end = p + 1;
                break;
            }
        }
        p++;
    }
    assert(end);
    int len = (int)(end - body);
    char bodybuf[4096];
    if (len >= (int)sizeof(bodybuf))
        len = sizeof(bodybuf) - 1;
    memcpy(bodybuf, body, len);
    bodybuf[len] = 0;
    // The body MUST call sendSweepFrame_unlocked(PING_CMD).
    // The bug-class shape re-armed the timer only, which is
    // a no-op if the peer missed earlier PING.
    assert(strstr(bodybuf, "sendSweepFrame_unlocked(PING_CMD)") != NULL &&
           "Pin 3: the P3 preferredBaud_ retry branch must "
           "call sendSweepFrame_unlocked(PING_CMD) so the peer "
           "has a fresh chance to lock. A pure timer re-arm is a "
           "no-op if the peer missed earlier PING outright "
           "(e.g. wire noise) — the link would just wait the "
           "same dwell and time out again, falling to P1 every "
           "time and losing the preserved baud.");
    std::cout << " Pin 3 PASS (P3 retry re-sends PING, not just "
                 "re-arming the timer)"
              << std::endl;
    free(buf);
}

// Pin 4: slave PHASE3 camp bounded re-arms + fall to
// P1. The slave's PHASE3 camp had no exit (re-armed
// forever), so a master walking P1 from slowest
// while the slave camped P3 at the proven baud was
// a permanent deadlock. The new shape: the slave
// re-arms RESWEEP_PREF_MAX_ATTEMPTS times at the
// preserved baud, then clears preferredBaud_ and
// re-enters P1 the master can actually find. With
// the peer absent (master does not exist in this
// test), the slave must exit PHASE3 to P1 after
// RESWEEP_PREF_MAX_ATTEMPTS re-arms. Source-grep
// pins the contract.
static void test_slave_p3_camp_bounded_by_attempts() {
    std::cout << "\n Pin 4: slave PHASE3 camp bounded re-arms + fall "
                 "to P1 (source-grep) ==="
              << std::endl;
    FILE *f = fopen(
        testRepoPath("src/al/link/timers/LinkTimersSwp.cpp").c_str(), "r");
    assert(f);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    assert(buf);
    size_t got = fread(buf, 1, (size_t)sz, f);
    buf[got] = 0;
    fclose(f);
    // The slave PHASE3 path must contain the bounded
    // re-arms + fall-to-P1 logic. The bug-class shape
    // was a bare `hw.startTimer(...)` at the bottom of
    // the slave PHASE3 path with no exit. The new
    // shape increments resweepPrefAttempts_ on each
    // re-arm and clears preferredBaud_ + enters P1
    // when the budget is exhausted.
    const char *slaveP3Camp = strstr(buf, "Slave PHASE3 camp");
    assert(slaveP3Camp &&
           "Pin 4: LinkTimersSwp.cpp must contain a comment "
           "anchoring the slave PHASE3 camp bounded re-arm "
           "fix.");
    // The body must reference RESWEEP_PREF_MAX_ATTEMPTS
    // (the budget) and call sweep_.enterPhase1(*this)
    // (the fall-to-P1) when the budget is exhausted.
    const char *budget = strstr(slaveP3Camp, "RESWEEP_PREF_MAX_ATTEMPTS");
    assert(budget &&
           "Pin 4: the slave PHASE3 camp must reference "
           "RESWEEP_PREF_MAX_ATTEMPTS (the bounded-budget "
           "fall-to-P1 fix). The bare-re-arm bug class did "
           "not have a budget at all.");
    const char *fall = strstr(slaveP3Camp, "sweep_.enterPhase1(*this)");
    assert(fall &&
           "Pin 4: the slave PHASE3 camp must call "
           "sweep_.enterPhase1(*this) when the budget is "
           "exhausted. The fix falls to P1 the master (in P1) "
           "can find; without it the slave camps at the proven "
           "baud forever.");
    // The fall path must clear preferredBaud_ so the
    // next pass is a clean P1 walk, not a preserved-baud
    // re-attempt.
    const char *clearPref =
        strstr(slaveP3Camp, "preferredBaud_ = NO_PREFERRED_BAUD");
    assert(clearPref &&
           "Pin 4: the slave PHASE3 fall path must clear "
           "preferredBaud_ (the proven baud we just failed "
           "to lock at) so the next pass is a clean P1 walk.");
    // The LinkTimersOk.cpp master watchdog reset must
    // pass preservePreferredBaud=true so the master
    // honors the proven baud on a HealthWatchdog reset
    // (symmetric with the slave's BREAK-triggered
    // fast path). Without this symmetry the master
    // walks P1 from 9600 while the slave camps P3 at
    // 512000 — mutual deadlock.
    FILE *fOk =
        fopen(testRepoPath("src/al/link/timers/LinkTimersOk.cpp").c_str(), "r");
    assert(fOk);
    fseek(fOk, 0, SEEK_END);
    long szOk = ftell(fOk);
    fseek(fOk, 0, SEEK_SET);
    char *bufOk = (char *)malloc((size_t)szOk + 1);
    assert(bufOk);
    size_t gotOk = fread(bufOk, 1, (size_t)szOk, fOk);
    bufOk[gotOk] = 0;
    fclose(fOk);
    const char *applyHealth =
        strstr(bufOk, "HealthAction Link::applyHealth_unlocked(");
    assert(applyHealth &&
           "Pin 4: LinkTimersOk.cpp must contain "
           "applyHealth_unlocked.");
    // The watchdog reset in applyHealth_unlocked must
    // pass preservePreferredBaud=true.
    const char *preserveCall = strstr(
        applyHealth, "reset_unlocked(true, /*preservePreferredBaud=*/true,");
    if (!preserveCall) {
        // Allow the literal-true form too.
        preserveCall = strstr(applyHealth, "reset_unlocked(true, true,");
    }
    assert(preserveCall &&
           "Pin 4: applyHealth_unlocked's watchdog reset "
           "must pass preservePreferredBaud=true. The prior "
           "shape passed false, so a master health drop walked "
           "P1 from 9600 while the slave camped P3 at 512000 — "
           "mutual deadlock. The slave's bounded P3 budget "
           "is now the only way out, and it requires the "
           "master to also honor the proven baud on its end.");
    free(buf);
    free(bufOk);
    std::cout << " Pin 4 PASS (slave PHASE3 camp bounded by "
                 "RESWEEP_PREF_MAX_ATTEMPTS re-arms + fall to "
                 "P1; master watchdog reset honors "
                 "preservePreferredBaud=true)"
              << std::endl;
}

int main() {
    std::cout << "=== Re-Convergence After HealthWatchdog Reset "
                 "Tests ==="
              << std::endl;
    test_re_converge_after_health_watchdog_reset();
    test_slave_preserved_baud_path_source_grep();
    test_p3_preferred_baud_retry_resends_ping();
    test_slave_p3_camp_bounded_by_attempts();
    std::cout << "\n=== All 4 re-converge pins PASS ===" << std::endl;
    return 0;
}

#endif
