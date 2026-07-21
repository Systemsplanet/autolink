// Pinned regression: a reset triggered by discovering the PEER's
// epoch changed must not mint a new epoch of our own. The bug
// class (found from a field log, not a design review): both
// Link instances run the identical OK-state epoch-mismatch check,
// and reset_unlocked bumped sweepEpoch_ on every counted reset
// regardless of reason. So: master's real (hardware-seeded)
// disconnect bumps its epoch and sends a PING with the new value
// -> pong (still OK) sees the mismatch and correctly resets — but
// that reset ALSO bumped pong's own sweepEpoch_, even though pong
// didn't restart anything, it was just syncing to what master
// already announced. Pong's next PING now carries a "new" epoch
// -> master (already back OK) sees that as a mismatch and resets
// again, bumping its epoch again. Two correctly-functioning
// detectors hand one glitch back and forth forever — the field
// log's epoch slide (peer 0->1->2->3->4 on BOTH sides, never
// converging).
//
//  Pin 1 (unit): reset_unlocked(true, ..., PeerEpochMismatch)
//  from OK must NOT bump sweepEpoch_. reset_unlocked(true, ...,
//  HealthWatchdog) from OK must still bump it — the exemption is
//  reason-specific, not a blanket freeze.
//
//  Pin 2 (source-grep): the exemption's load-bearing shape in
//  LinkCore.cpp — sweepEpoch_++ must be conditioned on
//  `reason != ResetReason::PeerEpochMismatch`, not unconditional.
//
//  Pin 3 (runtime, the real regression): two-node scenario
//  reproducing the field storm. Master takes one genuine
//  HealthWatchdog reset (epoch bumps once). Pong, still OK, sees
//  the mismatch and resets in response. The two must converge to
//  OK WITHOUT a second round: master's epoch must still read 1
//  (not 2), and pong's PeerEpochMismatch reset count must stay
//  at 1, not cascade.
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include <cstring>
#    include <cstdio>
#    include "MockHal.h"
#    include "LinkTestAccessor.h"
#    include "NullArqCache.h"
#    include "TestPaths.h"

using namespace autolink;

static const int kBauds[] = { 115200 };
static const int kNumBauds = (int)(sizeof(kBauds) / sizeof(kBauds[0]));

static void bringToOk(Link &a, Link &b, MockHal &mHal, MockHal &sHal) {
    a.begin();
    b.begin();
    for (int i = 0; i < 200; i++) {
        mHal.pumpClock(50);
        sHal.pumpClock(50);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
        if (a.getState() == State::OK && b.getState() == State::OK)
            return;
    }
    assert(false && "failed to bring two single-baud nodes to OK");
}

// Pin 1: unit-level exemption check on a single node.
static void test_peer_epoch_mismatch_reset_does_not_bump_epoch() {
    std::cout << "\n=== Pin 1: PeerEpochMismatch reset does not bump "
                 "sweepEpoch_ (HealthWatchdog still does) ==="
              << std::endl;
    NullArqCache cacheA;
    AutoLinkConfig cfg;
    for (int i = 0; i < kNumBauds; i++)
        cfg.allowedBauds[i] = (uint32_t)kBauds[i];
    cfg.allowedBaudsCount = kNumBauds;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 0;
    cfg.syncAckTimeoutMs = 500;
    cfg.maxRetx = 50;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link a(mHal, cacheA, /*isMaster=*/true, cfg);
    NullArqCache cacheB;
    Link b(sHal, cacheB, /*isMaster=*/false, cfg);
    bringToOk(a, b, mHal, sHal);
    LinkTestAccessor acc(a);
    assert(acc.sweepEpochForTest() == 0 &&
           "precondition: A.sweepEpoch_ starts at 0");

    acc.resetLink(true, /*preserve=*/true, ResetReason::PeerEpochMismatch);
    assert(acc.sweepEpochForTest() == 0 &&
           "Pin 1: PeerEpochMismatch reset must NOT bump sweepEpoch_ — "
           "we're syncing to a restart the peer already announced, not "
           "restarting our own session");
    assert(acc.recentDiscsForTest() >= 1 &&
           "Pin 1: discCount/recentDiscs_ bookkeeping must still run for "
           "a PeerEpochMismatch reset — only the epoch bump is exempted");

    // Relock so the next reset starts from OK again.
    LinkTestAccessor(a).forceState(State::OK);
    acc.resetLink(true, /*preserve=*/true, ResetReason::HealthWatchdog);
    assert(acc.sweepEpochForTest() == 1 &&
           "Pin 1: HealthWatchdog reset must still bump sweepEpoch_ — the "
           "exemption is specific to PeerEpochMismatch, not a blanket "
           "freeze on every reset reason");
    std::cout << " Pin 1 PASS (PeerEpochMismatch exempted, HealthWatchdog "
                 "still bumps)"
              << std::endl;
}

// Pin 2: source-grep on the exemption's load-bearing shape.
static void test_epoch_bump_is_reason_conditioned_source_grep() {
    std::cout << "\n=== Pin 2: sweepEpoch_++ conditioned on reason != "
                 "PeerEpochMismatch (source-grep) ==="
              << std::endl;
    FILE *f = fopen(testRepoPath("src/al/link/LinkCore.cpp").c_str(), "r");
    assert(f);
    char buf[65536]; // grown: LinkCore.cpp passed 16 KB and reset_unlocked was
                     // truncated mid-body
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
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
    char bodybuf[65536];
    if (len >= (int)sizeof(bodybuf))
        len = sizeof(bodybuf) - 1;
    memcpy(bodybuf, fn, len);
    bodybuf[len] = 0;

    const char *epoch = strstr(bodybuf, "sweepEpoch_++");
    assert(epoch &&
           "Pin 2: reset_unlocked must still bump sweepEpoch_ "
           "somewhere — the bug is an unconditional bump, not a "
           "missing one");
    // The guard must be within ~200 bytes before the increment (the
    // immediately-enclosing if), and must name PeerEpochMismatch.
    const char *searchStart = (epoch - bodybuf > 200) ? epoch - 200 : bodybuf;
    char window[512];
    int wlen = (int)(epoch - searchStart);
    if (wlen >= (int)sizeof(window))
        wlen = sizeof(window) - 1;
    memcpy(window, searchStart, wlen);
    window[wlen] = 0;
    assert(strstr(window, "PeerEpochMismatch") != NULL &&
           strstr(window, "!=") != NULL &&
           "Pin 2: sweepEpoch_++ must be guarded by a "
           "`reason != ResetReason::PeerEpochMismatch` check immediately "
           "before it — an unconditional sweepEpoch_++ is the exact bug "
           "class this pin exists to catch");
    std::cout << " Pin 2 PASS (sweepEpoch_++ guarded by reason != "
                 "PeerEpochMismatch)"
              << std::endl;
}

// Pin 3: the real regression — two nodes, one seeded disconnect,
// must NOT cascade into a second round.
static void test_epoch_storm_does_not_cascade() {
    std::cout << "\n=== Pin 3: one seeded disconnect converges without a "
                 "second epoch round (two-node) ==="
              << std::endl;
    NullArqCache cacheA, cacheB;
    AutoLinkConfig cfg;
    for (int i = 0; i < kNumBauds; i++)
        cfg.allowedBauds[i] = (uint32_t)kBauds[i];
    cfg.allowedBaudsCount = kNumBauds;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 0; // keepalive disabled: isolate the epoch path
    cfg.syncAckTimeoutMs = 500;
    cfg.maxRetx = 50;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link a(mHal, cacheA, /*isMaster=*/true, cfg);
    Link b(sHal, cacheB, /*isMaster=*/false, cfg);
    bringToOk(a, b, mHal, sHal);
    assert(a.getState() == State::OK);
    assert(b.getState() == State::OK);

    // Seed ONE genuine disconnect on master, exactly like the field's
    // spurious UART_BREAK: onBreak() while OK, delivered directly to
    // A's own driver (mirrors a real framing glitch reported locally,
    // not a break the peer sent).
    mHal.deliver_break_to_self();
    // : a single OK-state BREAK is debounced
    // (the confirm window arms; the link stays OK
    // until the window expires). Pump past
    // BREAK_CONFIRM_MS (150 ms) so the confirm
    // deadline fires and reset_unlocked runs.
    for (int i = 0; i < 10; i++) {
        mHal.pumpClock(20);
        sHal.pumpClock(20);
    }
    assert(LinkTestAccessor(a).sweepEpochForTest() == 1 &&
           "precondition: A's own break must bump its epoch once");

    // Pump until both sides converge back to OK.
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
    assert(converged >= 0 &&
           "Pin 3: A and B must converge to OK after the seeded disconnect");

    // Pump further — long enough that a cascading bounce (the bug)
    // would have fired a second round by now, but not so long that
    // legitimate idle-keepalive noise (disabled here) could confuse
    // the read.
    for (int i = 0; i < 200; i++) {
        mHal.pumpClock(50);
        sHal.pumpClock(50);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
        a.onTimer();
        b.onTimer();
    }

    uint8_t epochA = LinkTestAccessor(a).sweepEpochForTest();
    assert(epochA == 1 &&
           "Pin 3: A's epoch must still read 1 — a second bump means B's "
           "recovery reset re-triggered A's mismatch check (the bounce "
           "this pin exists to catch)");
    assert(a.getState() == State::OK && b.getState() == State::OK &&
           "Pin 3: both sides must still be OK — a bounce would have "
           "torn at least one back into SWP");
    std::cout << " Pin 3 PASS (epoch settled at 1, no cascade, both sides "
                 "OK)"
              << std::endl;
}

int main() {
    std::cout << "=== Epoch-Bounce Guard Tests ===" << std::endl;
    test_peer_epoch_mismatch_reset_does_not_bump_epoch();
    test_epoch_bump_is_reason_conditioned_source_grep();
    test_epoch_storm_does_not_cascade();
    std::cout << "\n=== All 3 epoch-bounce guard pins PASS ===" << std::endl;
    return 0;
}

#endif
