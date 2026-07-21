// Pinned regression: after the the prior release release the field
// log showed the death-spiral going permanent — master
// stuck at 9600, peer BREAK-cycling at 512000, every
// "re-lock" producing zero application frames. The
// errThreshold(100)/errRateWindow(30) paths were burning
// purely on relock noise, never on real CRC data errors,
// and neither side detected "I keep re-locking but never
// getting an echo" as a distinct condition from generic
// line noise. The new DropPeerBaudMismatch health action
// escalates after kPeerBaudMismatchThreshold consecutive
// successful locks that produced zero valid application
// frames before the next reset.
//
//   Pin 1: locksWithoutRecv_ increments on every reset
//   that follows a real (wasEverOk_) link drop. A fresh
//   link's first lock (wasEverOk_=false) doesn't increment.
//   Pin 2: a successful CRC-validated recvMsg clears
//   locksWithoutRecv_ to 0 (on the link that received).
//   Pin 3: source-grep on the escalation: locksWithoutRecv_
//   must be in the same decideHealth() call (or its
//   shouldEscalatePeerBaudMismatch helper), and the
//   threshold must come from the const static
//   kPeerBaudMismatchThreshold in Link.h (not a magic
//   number in LinkTimersOk.cpp).
//   Pin 4: when the escalation fires (locksWithoutRecv_
//   has hit the threshold), the resulting reset_unlocked
//   call MUST fall through to a full P1 walk, not a P3
//   re-lock at preferredBaud_ — the escalation is
//   specifically saying "the preserved baud is wrong,
//   walk the whole ladder", and the reset must honor
//   that. The prior release had a hole here: the
//   escalation's `reset_unlocked(preserve=true)` re-lit
//   the fast-path condition (preferredBaud_ still set,
//   recentDiscs_ still below DISC_STORM_THRESHOLD on
//   cycles >10 s) and entered P3 instead of P1. Fix:
//   the mismatch verdict in applyHealth_unlocked clears
//   preferredBaud_ before calling reset_unlocked, AND
//   reset_unlocked's fast-path gate adds a
//   locksWithoutRecv_ < kPeerBaudMismatchThreshold
//   second-line defense.
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

static AutoLinkConfig cfgCommon() {
    AutoLinkConfig cfg;
    for (int i = 0; i < kNumBauds; i++)
        cfg.allowedBauds[i] = (uint32_t)kBauds[i];
    cfg.allowedBaudsCount = kNumBauds;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 10000;
    cfg.syncAckTimeoutMs = 1000;
    cfg.maxRetx = 50;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    return cfg;
}

// Pin 1: locksWithoutRecv_ increments on every reset
// that follows a real (wasEverOk_) link drop.
static void test_locksWithoutRecv_increments_on_real_reset() {
    std::cout << "\n=== Pin 1: locksWithoutRecv_ increments on every "
                 "real (wasEverOk_) reset ==="
              << std::endl;
    NullArqCache cacheA, cacheB;
    AutoLinkConfig cfg = cfgCommon();
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link a(mHal, cacheA, /*isMaster=*/true, cfg);
    Link b(sHal, cacheB, /*isMaster=*/false, cfg);
    bringToOk(a, b, mHal, sHal);
    LinkTestAccessor accA(a);
    assert(accA.locksWithoutRecvForTest() == 0 &&
           "Pin 1 pre: locksWithoutRecv_ starts at 0 on a fresh OK link");

    // CONTRACT UPDATE (fair-chance horizon): a session torn down
    // before postLockQuietMs + 2x syncAckTimeoutMs could not have
    // produced the ACK/recv that clears this counter no matter how
    // good the baud is — counting those made the escalation
    // self-sustaining (it killed each relock inside the quiet
    // window, which prevented the crossing, which kept the counter
    // high, forever — fieldsoak relock storm). An IMMEDIATE reset
    // therefore must NOT increment; a reset after the horizon,
    // with still no valid crossing, MUST.
    accA.resetLink(true, /*preserve=*/true, ResetReason::HealthWatchdog);
    assert(accA.locksWithoutRecvForTest() == 0 &&
           "Pin 1a: a reset before the fair-chance horizon must NOT "
           "increment locksWithoutRecv_");
    std::cout << "  Pin 1a PASS (immediate reset: no increment — no fair "
                 "chance)"
              << std::endl;
    bringToOk(a, b, mHal, sHal);
    // Age the session past quiet + 2 RTO (600 + 2000 ms with this
    // cfg) with zero valid crossings, then drop it.
    uint32_t fair = (uint32_t)cfgCommon().postLockQuietMs +
        2u * (uint32_t)cfgCommon().syncAckTimeoutMs + 100u;
    mHal.pumpClock(fair);
    sHal.pumpClock(fair);
    accA.resetLink(true, /*preserve=*/true, ResetReason::HealthWatchdog);
    assert(accA.locksWithoutRecvForTest() == 1 &&
           "Pin 1b: a reset AFTER the fair-chance horizon with no valid "
           "crossing must increment locksWithoutRecv_ to 1");
    std::cout << "  Pin 1b PASS (post-horizon reset with no crossing: "
                 "increments to 1)"
              << std::endl;
}

// Pin 2: a successful CRC-validated recvMsg clears
// locksWithoutRecv_ to 0 (on the link that received).
static void test_recv_clears_locksWithoutRecv() {
    std::cout << "\n=== Pin 2: a successful CRC-validated recvMsg clears "
                 "locksWithoutRecv_ to 0 on the link that received ==="
              << std::endl;
    NullArqCache cacheA, cacheB;
    AutoLinkConfig cfg = cfgCommon();
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link a(mHal, cacheA, /*isMaster=*/true, cfg);
    Link b(sHal, cacheB, /*isMaster=*/false, cfg);
    bringToOk(a, b, mHal, sHal);
    LinkTestAccessor accB(b);
    // Plant a non-zero locksWithoutRecv_ on b via a
    // post-OK reset, then re-bringToOk and have a
    // send a message that b receives. b's recv
    // success must clear b's counter.
    // Fair-chance horizon (see Pin 1): age the session past
    // quiet + 2 RTO with no valid crossings on b before the
    // drop, so the reset legitimately counts.
    {
        uint32_t fair2 = (uint32_t)cfgCommon().postLockQuietMs +
            2u * (uint32_t)cfgCommon().syncAckTimeoutMs + 100u;
        // Pump WITHOUT piping, so no keepalive/ACK crossing
        // reaches b and clears its counter mid-aging.
        mHal.pumpClock(fair2);
        sHal.pumpClock(fair2);
    }
    accB.resetLink(true, /*preserve=*/true, ResetReason::HealthWatchdog);
    // Drive the re-convergence via pumpClock so the
    // slave's PHASE3 budget is honored on real timer
    // fires, not artificial onTimer calls. The slave's
    // PHASE3 re-arm timer is the P3 dwell (~275 ms at
    // N=1 baud); after RESWEEP_PREF_MAX_ATTEMPTS=2
    // re-arms the slave falls to P1 and re-sweeps.
    // Re-convergence therefore takes 1 P3 budget +
    // P1/P2/P3 lock cycle (master keepalive every
    // idleTimeoutMs/2 = 5 s, slave needs 3 acks); 800
    // iterations gives 40 s of wall-clock, which is
    // enough for the full cycle in the worst case.
    for (int i = 0; i < 800; i++) {
        mHal.pumpClock(50);
        sHal.pumpClock(50);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
        if (a.getState() == State::OK && b.getState() == State::OK)
            break;
    }
    assert(a.getState() == State::OK && b.getState() == State::OK &&
           "Pin 2 pre: link must be OK after a reset+bringToOk");
    assert(accB.locksWithoutRecvForTest() >= 1 &&
           "Pin 2 pre: locksWithoutRecv_ >= 1 after the post-OK reset");
    // Pump past postLockQuietMs (600 ms default) so
    // the first post-lock sendMsg is not deferred.
    for (uint32_t t = 0; t < 1500; t += 50) {
        mHal.pumpClock(50);
        sHal.pumpClock(50);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
    }
    const uint8_t payload[] = { 'h', 'i' };
    bool ok = a.sendMsg(payload, sizeof payload);
    assert(ok &&
           "Pin 2 pre: sendMsg after a real reset must succeed "
           "(postLockQuietMs=600 ms default has elapsed by now)");
    int delivered = -1;
    for (int i = 0; i < 200; i++) {
        mHal.pumpClock(50);
        sHal.pumpClock(50);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
        uint8_t sink[64];
        int n = b.recvMsg(sink, sizeof sink);
        if (n == (int)sizeof payload) {
            delivered = i;
            break;
        }
    }
    assert(delivered >= 0 &&
           "Pin 2 pre: a real message must round-trip after a reset");
    // After the recv, b's locksWithoutRecv_ must be
    // 0 (the recv success branch cleared it).
    assert(accB.locksWithoutRecvForTest() == 0 &&
           "Pin 2: a successful CRC-validated recvMsg must clear "
           "locksWithoutRecv_ to 0 on the link that received — "
           "the counter only ticks up on real (wasEverOk_) "
           "resets, and a recv success proves the link is "
           "actually delivering at the locked baud, so the "
           "escalation signal is no longer warranted");
    std::cout << "  Pin 2 PASS (locksWithoutRecv_ cleared to 0 by "
                 "a real recv in "
              << delivered << " iterations)" << std::endl;
}

// Pin 3: source-grep on the escalation: the threshold
// must come from the const static kPeerBaudMismatchThreshold
// in Link.h (not a magic number), and the decision
// helper must reference locksWithoutRecv_.
static void test_escalation_source_grep() {
    std::cout << "\n=== Pin 3: escalation threshold source-grep ==="
              << std::endl;
    // Link.h: kPeerBaudMismatchThreshold constant
    {
        FILE *f = fopen(testRepoPath("src/al/link/Link.h").c_str(), "r");
        assert(f);
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = (char *)malloc((size_t)sz + 1);
        assert(buf);
        size_t got = fread(buf, 1, (size_t)sz, f);
        buf[got] = 0;
        fclose(f);
        const char *k = strstr(buf, "kPeerBaudMismatchThreshold");
        assert(k);
        const char *field = strstr(buf, "int locksWithoutRecv_");
        assert(field);
        free(buf);
    }
    // LinkHealth.h: shouldEscalatePeerBaudMismatch helper
    {
        FILE *f = fopen(testRepoPath("src/al/link/timers/LinkHealth.h").c_str(), "r");
        assert(f);
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = (char *)malloc((size_t)sz + 1);
        assert(buf);
        size_t got = fread(buf, 1, (size_t)sz, f);
        buf[got] = 0;
        fclose(f);
        const char *helper = strstr(buf, "shouldEscalatePeerBaudMismatch");
        assert(helper &&
               "Pin 3: LinkHealth.h must define "
               "shouldEscalatePeerBaudMismatch — the decision "
               "helper for the peer-baud-mismatch escalation");
        const char *enumVal = strstr(buf, "DropPeerBaudMismatch");
        assert(enumVal &&
               "Pin 3: LinkHealth.h HealthAction enum must include "
               "DropPeerBaudMismatch");
        free(buf);
    }
    // LinkTimersOk.cpp: the call site uses
    // kPeerBaudMismatchThreshold (not a magic number).
    {
        FILE *f = fopen(testRepoPath("src/al/link/timers/LinkTimersOk.cpp").c_str(), "r");
        assert(f);
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = (char *)malloc((size_t)sz + 1);
        assert(buf);
        size_t got = fread(buf, 1, (size_t)sz, f);
        buf[got] = 0;
        fclose(f);
        const char *kThresh = strstr(buf, "kPeerBaudMismatchThreshold");
        assert(kThresh &&
               "Pin 3: LinkTimersOk.cpp must reference "
               "kPeerBaudMismatchThreshold (not a magic number) — "
               "the threshold lives in Link.h as a const static "
               "so the test can read the same value");
        const char *magic = strstr(buf, "locksWithoutRecv_ >= 3");
        assert(!magic &&
               "Pin 3: no magic number `locksWithoutRecv_ >= 3` "
               "in LinkTimersOk.cpp — the threshold must come "
               "from the const static kPeerBaudMismatchThreshold");
        free(buf);
    }
    std::cout << "  Pin 3 PASS (threshold source-grep OK)" << std::endl;
}

// Pin 4: when the escalation fires, the resulting
// reset MUST fall through to a full P1 walk, not
// a P3 re-lock at preferredBaud_. The prior release
// escalation called reset_unlocked(preserve=true)
// unconditionally, which re-lit the fast-path
// condition (preferredBaud_ still set,
// recentDiscs_ still below DISC_STORM_THRESHOLD on
// cycles >10 s) and entered P3 instead of P1. Fix:
// the mismatch verdict in applyHealth_unlocked
// clears preferredBaud_ AND the reset_unlocked
// fast-path gate adds
// locksWithoutRecv_ < kPeerBaudMismatchThreshold.
static void test_escalation_falls_through_to_phase1() {
    std::cout << "\n=== Pin 4: escalation falls through to PHASE1, "
                 "not a P3 preferredBaud_ re-lock ==="
              << std::endl;
    NullArqCache cacheA, cacheB;
    AutoLinkConfig cfg = cfgCommon();
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link a(mHal, cacheA, /*isMaster=*/true, cfg);
    Link b(sHal, cacheB, /*isMaster=*/false, cfg);
    bringToOk(a, b, mHal, sHal);
    LinkTestAccessor accA(a);
    accA.setLocksWithoutRecvForTest(accA.peerBaudMismatchThresholdForTest());
    // Fair-chance horizon (see Pin 1 / applyHealth): the
    // escalation never fires before the CURRENT session has been
    // up quiet + 2 RTO — age past it (no piping, so no crossing
    // clears the counter) before asking for the verdict.
    {
        uint32_t fair4 = (uint32_t)cfgCommon().postLockQuietMs +
            2u * (uint32_t)cfgCommon().syncAckTimeoutMs + 100u;
        // Advance the clock WITHOUT timer dispatch (pumpClock
        // would fire the OK tick and the escalation itself
        // mid-aging); the pin wants to observe the verdict from
        // its own applyHealth call below.
        mHal.now += fair4;
        sHal.now += fair4;
    }
    assert(accA.preferredBaudForTest() != 0xFF &&
           "Pin 4 pre: a fresh OK link has a "
           "preferredBaud_ set (not NO_PREFERRED_BAUD)");
    assert(accA.recentDiscsForTest() == 0 &&
           "Pin 4 pre: a fresh OK link has recentDiscs_=0");
    int aCode = accA.applyHealth(mHal.nowMs());
    assert(aCode == (int)HealthAction::DropPeerBaudMismatch &&
           "Pin 4: applyHealth_unlocked must return "
           "DropPeerBaudMismatch when locksWithoutRecv_ "
           "is at the threshold");
    assert(a.getState() == State::SWP &&
           "Pin 4: the escalation reset must leave the "
           "link in SWP (a normal reset, not a re-lock)");
    assert(accA.sweepPhase() == SweepPhase::PHASE1 &&
           "Pin 4: the escalation reset MUST enter "
           "PHASE1 (full P1 walk), not PHASE3 "
           "(preferredBaud_ re-lock). The prior release "
           "release had a hole here: the escalation's "
           "reset_unlocked(preserve=true) re-lit the "
           "fast-path condition and entered P3, which "
           "is exactly what the escalation's log said "
           "it would NOT do");
    assert(accA.preferredBaudForTest() == 0xFF &&
           "Pin 4: the escalation must clear "
           "preferredBaud_ before reset_unlocked so "
           "the fast-path condition (preferredBaud_ "
           "!= NO_PREFERRED_BAUD) cannot engage even "
           "if a future caller forgot to honor the "
           "escalation's intent");
    assert(accA.locksWithoutRecvForTest() >=
               accA.peerBaudMismatchThresholdForTest() &&
           "Pin 4: the reset_unlocked call inside the "
           "escalation must have incremented "
           "locksWithoutRecv_ (wasEverOk_=true here)");
    std::cout << "  Pin 4 PASS (escalation entered PHASE1, "
                 "preferredBaud_ cleared, locksWithoutRecv_="
              << accA.locksWithoutRecvForTest() << ")" << std::endl;
}

int main() {
    std::cout << "=== Peer-Baud-Mismatch Escalation Tests ===" << std::endl;
    test_locksWithoutRecv_increments_on_real_reset();
    test_recv_clears_locksWithoutRecv();
    test_escalation_source_grep();
    test_escalation_falls_through_to_phase1();
    std::cout << "\n=== All 4 peer-baud-mismatch pins PASS ===" << std::endl;
    return 0;
}

#endif
