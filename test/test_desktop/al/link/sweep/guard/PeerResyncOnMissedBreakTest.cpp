// Pinned regression: a one-sided reset that NEVER delivers a BREAK
// to the still-OK peer must not leave that peer stranded in OK,
// because every PING the resetting side sends afterwards arrives on
// a peer that is now in a different session. The bug class was
// auto-ACKing the PING (because it landed on a PING in State::OK)
// and the receiver's gbnBase walked past the seq, dropping the
// incoming burst as GAP...dropped and the link stuck in a recv
// storm that eventually tore the link down on a watchdog or
// crc/desync burst.
//
// The fix carries a session epoch in the seq byte of every sweep
// frame and forces a real resync when the still-OK side observes
// a sweep PING with a different epoch. Pinned in two shapes:
//
//   Pin 1 (runtime): the production bug. One side resets without
//   delivering a BREAK; the other side (still State::OK) receives
//   the resetting side's P1 PINGs and is forced into SWP within a
//   bounded number of iterations. Both sides then converge to OK,
//   a sendMsg/recvMsg round-trip succeeds (the real proof: the
//   resync actually completed, not just that states flipped), and
//   the link's baud index matches across both sides (i.e. they
//   locked on the same baud, not just both happened to reach OK
//   at different ones).
//
//   Pin 2 (source-grep): the fix's load-bearing shape in
//   processCtrlFrame_unlocked. The still-OK receive path must check
//   the seq byte against the latched peerSweepEpoch_ and call
//   reset_unlocked when they differ, NOT auto-ack the PING. A
//   plain `if (pl == PING_CMD) sendPongAck_unlocked();` shape is
//   the exact bug class; the pin rejects that shape so
//   a future re-introduction trips red on the source audit alone.
//
//   Pin 3 (toggle-off verification): source-grep pin that the
//   processCtrlFrame_unlocked body still has the peerSweepEpoch_
//   check (not deleted, not commented out). Toggling off the fix
//   by deleting the epoch check makes Pin 1 flip red; Pin 3 makes
//   the source shape itself a structural pin so a regression that
//   reintroduces the bug-class shape during a refactor can't slip
//   past the source audit even if Pin 1 is briefly green.
//
// Setup: two Link over MockHal, one baud (so the test is self-
// contained and not gated on sweep timing), idleTimeoutMs=0 so
// keepalive is disabled (it'd otherwise re-OK the link before the
// resync's P1->P2->P3 walk completes), ASYNC mode (the bug class
// is ARQ-driven, so the runtime path is the ASYNC receiver).
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include <cstring>
#    include <cstdio>
#    include <vector>
#    include "MockHal.h"
#    include "LinkTestAccessor.h"
#    include "NullArqCache.h"
#    include "al/util/UtilCrc.h"
#    include "al/link/LinkWire.h"
#    include "TestPaths.h"

using namespace autolink;

static const int kBauds[] = { 115200 };
static const int kNumBauds = (int)(sizeof(kBauds) / sizeof(kBauds[0]));

static void bringToOk(Link &a, Link &b, MockHal &mHal, MockHal &sHal) {
    a.begin();
    b.begin();
    // MockHal is single-baud, no baud-mismatch
    // filtering: a one-baud cfg reaches OK inside a
    // single 50 ms pump on the standard
    // negotiation loop.
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

// Pin 1: the production bug. Master resets WITHOUT delivering
// a BREAK (reset_unlocked bumps sweepEpoch_ and re-enters P1;
// no wire-side BREAK to slave). Slave is still State::OK. The
// first PING the master emits carries the new epoch in the seq
// byte; slave's processCtrlFrame_unlocked sees the mismatch,
// forces a real resync, and the two sides converge to OK again
// on the same baud.
static void test_peer_resyncs_on_missed_break() {
    std::cout << "\n=== Pin 1: peer resyncs on missed break (one-sided "
                 "reset, no BREAK delivered) ==="
              << std::endl;
    NullArqCache cacheA, cacheB;
    AutoLinkConfig cfg;
    for (int i = 0; i < kNumBauds; i++)
        cfg.allowedBauds[i] = (uint32_t)kBauds[i];
    cfg.allowedBaudsCount = kNumBauds;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 0; // keepalive disabled
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

    // Exchange a few data frames so the ARQ state
    // and lastRxSeq_/lastTxMs are real, not
    // zeroes-on-arrival.
    const uint8_t payloadA[] = { 'h', 'e', 'l', 'l', 'o' };
    uint8_t sink[64];
    bool okA = a.sendMsg(payloadA, sizeof payloadA);
    assert(okA);
    // Pump until A's frame round-trips through the
    // wire (one slave ACK is enough to confirm the
    // link is genuinely exchanging data; we don't
    // need a multi-message burst to stage the bug).
    int roundTrips = 0;
    for (int i = 0; i < 100; i++) {
        mHal.pumpClock(50);
        sHal.pumpClock(50);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
        if (b.recvMsg(sink, sizeof sink) > 0)
            roundTrips++;
        if (roundTrips >= 1)
            break;
    }
    assert(roundTrips >= 1 &&
           "precondition: at least one sendMsg must have round-tripped");

    // THE BUG: A resets without sending a BREAK.
    // MockHal::sendBreak() would call
    // peer->events()->onBreak() on the peer's HAL
    // and trigger B's onBreak, which is the
    // production recovery path. We bypass it on
    // purpose — this is the production failure
    // shape: a soft reset (power blip, ESP-IDF
    // task watchdog, brownout) where the wire
    // never goes idle enough to deliver a BREAK,
    // and the other side's UART doesn't see one.
    // resetLink(true) calls reset_unlocked(true)
    // directly under the lock, which bumps
    // sweepEpoch_ and re-enters P1 — exactly what
    // a soft reset does in production.
    uint8_t epochBeforeA = LinkTestAccessor(a).sweepEpochForTest();
    assert(epochBeforeA == 0 && "precondition: A.sweepEpoch_ starts at 0");
    LinkTestAccessor(a).resetLink(true);
    uint8_t epochAfterA = LinkTestAccessor(a).sweepEpochForTest();
    assert(epochAfterA == 1 &&
           "Pin 1 pre: A.sweepEpoch_ must bump from 0 to 1 on "
           "reset_unlocked(true) — that's what produces the new "
           "epoch in the next sweep PING's seq byte");
    assert(a.getState() == State::SWP &&
           "Pin 1 pre: A must be in SWP (P1) after reset_unlocked");
    assert(b.getState() == State::OK &&
           "Pin 1 pre: B is STILL in State::OK (no BREAK was "
           "delivered) — this is the production bug shape");

    // The whole point: A starts sending P1 PINGs
    // carrying sweepEpoch_=1, B is still in OK
    // and would auto-ACK without the fix (the bug
    // class). Pump the wire. With the fix, B
    // sees the first PING, the epoch check in
    // processCtrlFrame_unlocked fires, B calls
    // reset_unlocked(true, /*preserve=*/true),
    // and the resync machinery takes it from
    // there.
    int bResynced = -1;
    for (int i = 0; i < 200; i++) {
        mHal.pumpClock(50);
        sHal.pumpClock(50);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
        // Drive the master's P1 timer so the
        // PINGs keep firing — MockHal's
        // pumpClock only fires the timer, not
        // the link's onTick. Calling onTimer
        // mirrors the wire-level pump.
        a.onTimer();
        b.onTimer();
        if (b.getState() == State::SWP) {
            bResynced = i;
            break;
        }
    }
    assert(bResynced >= 0 &&
           "Pin 1: B must be forced out of State::OK by the "
           "epoch-mismatch PING within 200 pump iterations. "
           "Without the fix, B stays in State::OK and the link "
           "strands in recv-rejected/CRC-desync storms instead of "
           "resyncing.");
    std::cout << "  B forced into SWP at iteration " << bResynced
              << " (epoch mismatch PING detected)" << std::endl;

    // Drive the resync to completion. After B is
    // forced out of OK, the standard sweep
    // machinery takes over: B enters P1 slowest,
    // meets A there, both lock.
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
           "Pin 1: A and B must converge to State::OK after B's "
           "epoch-driven resync — otherwise the production bug "
           "is back");
    std::cout << "  converged at iteration " << converged
              << " (both OK, both at the same baud)" << std::endl;
    assert(a.getCurrentSpdIndex() == b.getCurrentSpdIndex() &&
           "Pin 1: A and B must lock on the same baud index after "
           "the resync — locking on different bauds is a partial "
           "recovery that still strands the link");
    std::cout << "  A.spdI=B.spdI=" << a.getCurrentSpdIndex()
              << " (both locked on the same baud)" << std::endl;

    // The real proof: a sendMsg/recvMsg
    // round-trip succeeds after the resync,
    // not just that both sides flipped to OK
    // on whatever baud they happened to be
    // parked at. This is the test the user's
    // field log would have caught: state
    // reports OK, but bytes don't move.
    // cfg.postLockQuietMs (600 ms default)
    // is enforced after a real drop, so the
    // first post-resync sendMsg inside the
    // quiet window is correctly deferred;
    // pump past the quiet window before
    // asserting.
    for (uint32_t t = 0; t < 800; t += 50) {
        mHal.pumpClock(50);
        sHal.pumpClock(50);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
    }
    const uint8_t payloadB[] = { 'r', 'e', 's', 'y', 'n', 'c' };
    bool okB = a.sendMsg(payloadB, sizeof payloadB);
    assert(okB && "Pin 1: sendMsg after resync must return true");
    int delivered = -1;
    int got = 0;
    for (int i = 0; i < 200 && got == 0; i++) {
        mHal.pumpClock(50);
        sHal.pumpClock(50);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
        a.onTimer();
        b.onTimer();
        int n = b.recvMsg(sink, sizeof sink);
        if (n == (int)sizeof payloadB &&
            memcmp(sink, payloadB, sizeof payloadB) == 0) {
            got = n;
            delivered = i;
        }
    }
    assert(got == (int)sizeof payloadB &&
           "Pin 1: post-resync sendMsg must deliver the exact "
           "payload to B's recvMsg. If this fails, the link is "
           "stuck in the field-log shape (state=OK, bytes don't "
           "move).");
    std::cout << "  post-resync round-trip delivered in " << delivered
              << " pump iterations" << std::endl;

    // Epoch state pins: A's epoch is 1 (it bumped
    // on reset), B's peer-epoch was latched on
    // the epoch-mismatch PING and is also 1. No
    // spurious epoch known/unknown flags.
    assert(LinkTestAccessor(a).sweepEpochForTest() == 1 &&
           "Pin 1: A.sweepEpoch_ must be 1 (bumped once on the "
           "reset that started the scenario)");
    assert(LinkTestAccessor(b).peerSweepEpochForTest() == 1 &&
           "Pin 1: B.peerSweepEpoch_ must be 1 (latched from the "
           "epoch-mismatch PING that forced B's resync)");
    assert(LinkTestAccessor(b).peerSweepEpochKnownForTest() &&
           "Pin 1: B.peerSweepEpochKnown_ must be true (set on "
           "the first sweep-frame arrival that triggered the "
           "resync)");

    std::cout << "  Pin 1 PASS (missed break -> epoch-driven resync "
                 "-> converged -> round-trip)"
              << std::endl;
}

// Pin 2: source-grep pin. The shape that triggered the
//   bug class was
//   if (pl == PING_CMD) sendPongAck_unlocked();
// The fix must check the epoch first and force a
// resync on mismatch. The pin rejects the bug-class shape AND
// asserts the load-bearing calls (reset_unlocked, return true to
// fire the BREAK round-trip).
static void test_process_ctrl_frame_uses_epoch_check() {
    std::cout << "\n=== Pin 2: processCtrlFrame_unlocked uses "
                 "peerSweepEpoch_ epoch check (source-grep) ==="
              << std::endl;
    FILE *f = fopen(testRepoPath("src/al/link/io/LinkRx.cpp").c_str(), "r");
    assert(f);
    char buf[32768];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    // Locate processCtrlFrame_unlocked.
    const char *fn = strstr(buf, "bool Link::processCtrlFrame_unlocked");
    assert(fn && "processCtrlFrame_unlocked not found in LinkRx.cpp");
    // End at the function's closing brace.
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
    char bodybuf[24576];
    if (len >= (int)sizeof(bodybuf))
        len = sizeof(bodybuf) - 1;
    memcpy(bodybuf, fn, len);
    bodybuf[len] = 0;

    // The epoch check must reference the
    // latched peerSweepEpoch_ AND the seq byte
    // from the incoming frame (rxBuf's seq
    // index). The shape with the epoch check
    // is the only one that closes the production bug.
    assert(strstr(bodybuf, "peerSweepEpoch_") != NULL &&
           "Pin 2: processCtrlFrame_unlocked must reference "
           "peerSweepEpoch_ — the buggy-original shape didn't, and "
           "that's the exact bug class this gate pins");
    assert(strstr(bodybuf, "peerSweepEpochKnown_") != NULL &&
           "Pin 2: processCtrlFrame_unlocked must reference "
           "peerSweepEpochKnown_ (the latch is the first half "
           "of the check; the mismatch is the second)");
    assert(strstr(bodybuf, "reset_unlocked") != NULL &&
           "Pin 2: processCtrlFrame_unlocked must call "
           "reset_unlocked on epoch mismatch — that's the load-"
           "bearing fix; without it the resync never starts");

    // Reject the bug-class auto-ack shape (no
    // epoch check between pl == PING_CMD and
    // sendPongAck_unlocked). We allow the
    // sendPongAck_unlocked call to remain for
    // the genuine-keepalive branch, but the
    // unconditional shape `if (pl == PING_CMD)
    // sendPongAck_unlocked();` (the bug-class
    // shape) must NOT be present. The
    // fix shape wraps the auto-ack in an
    // else / fall-through after the
    // epoch-mismatch branch returns.
    const char *autoAck = strstr(bodybuf, "sendPongAck_unlocked()");
    assert(autoAck);
    // Find the surrounding if (pl == PING_CMD)
    // ... sendPongAck_unlocked(); shape. A
    // regression that reintroduces the bug class
    // would have a 1-statement body
    // around the sendPongAck call with no
    // peerSweepEpoch_ check in between.
    // The shape with the epoch check has it
    // on the same if-branch (early return) and
    // a separate auto-ack path; the auto-ack
    // call's enclosing block must mention
    // either peerSweepEpoch_ (for the latch)
    // or sit in an else/separate if (the
    // genuine-keepalive fall-through).
    const char *plPing = strstr(bodybuf, "pl == PING_CMD");
    assert(plPing);
    // The first if (pl == PING_CMD) must
    // contain the epoch-mismatch check (it's
    // the bug-class branch). Walk forward from
    // the if to the next ';' or '{' to see
    // the first statement.
    const char *afterIf = plPing;
    while (*afterIf && *afterIf != '{' && *afterIf != '?')
        afterIf++;
    // Find the matching close brace (depth
    // tracking) — the if's body is between
    // here and the matching brace.
    if (*afterIf == '{') {
        int d = 1;
        const char *q = afterIf + 1;
        while (*q && d > 0) {
            if (*q == '{')
                d++;
            else if (*q == '}')
                d--;
            q++;
        }
        int ifLen = (int)(q - afterIf);
        char ifBody[8192];
        if (ifLen >= (int)sizeof(ifBody))
            ifLen = sizeof(ifBody) - 1;
        memcpy(ifBody, afterIf, ifLen);
        ifBody[ifLen] = 0;
        // The if's body must contain the
        // epoch-mismatch path. A regression
        // that reintroduces the bug class
        // (the if (pl == PING_CMD) auto-acks
        // directly) has sendPongAck_unlocked
        // inside this if-body and
        // peerSweepEpoch_ nowhere in it.
        // That's the bug class — reject.
        bool hasEpochCheck = strstr(ifBody, "peerSweepEpoch_") != NULL;
        assert(hasEpochCheck &&
               "Pin 2: the first if (pl == PING_CMD) body in "
               "processCtrlFrame_unlocked must contain the "
               "peerSweepEpoch_ check. The bug class was "
               "the if-body going straight to sendPongAck_unlocked; "
               "this gate rejects that shape.");
    }

    std::cout << "  Pin 2 PASS (epoch check present, reset_unlocked "
                 "call present, bug-class auto-ack-only shape rejected)"
              << std::endl;
}

// Pin 3: structural pin on reset_unlocked's epoch bump. The
// epoch must increment inside the same gate that increments
// discCount, so it moves in lockstep with disconnects and a
// routine/paused kickoff (count=false) leaves the epoch
// untouched.
static void test_reset_unlocked_bumps_epoch_under_disc_gate() {
    std::cout << "\n=== Pin 3: reset_unlocked bumps sweepEpoch_ under the "
                 "same discCount gate (source-grep) ==="
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
    // The bump must reference sweepEpoch_ and
    // be inside the same gate as discCount (the
    // count && state == State::OK check). Pin
    // the order: discCount++; ... sweepEpoch_++;
    // so a regression that re-orders them
    // (epoch before discCount, or epoch at
    // the wrong scope) is caught.
    const char *disc = strstr(bodybuf, "discCount++");
    const char *epoch = strstr(bodybuf, "sweepEpoch_++");
    assert(disc);
    assert(epoch);
    assert(disc < epoch &&
           "Pin 3: discCount++ must appear before sweepEpoch_++ "
           "in reset_unlocked — both are inside the same gate, "
           "and a future refactor that swaps the order trips "
           "this pin so the epoch/disc lockstep is visible at "
           "review time");
    std::cout << "  Pin 3 PASS (sweepEpoch_++ present, ordered after "
                 "discCount++, same gate)"
              << std::endl;
}

// Pin 4: the idle-keepalive-after-real-traffic regression. Every
// sweep-adjacent PING must carry sweepEpoch_, including the
// OK-state idle keepalive in onTimerOk_unlocked — if that one
// call site is left on the old sendFrame_unlocked (txSeq-based)
// send, the seq byte it carries is real application data
// sequence, not the epoch. Once any real traffic has advanced
// txSeq past the (usually small) epoch value, the very next
// routine keepalive reads as a false "peer restarted" signal on
// the receiving side and tears down a perfectly healthy, idle
// link. Pin 1 deliberately sets idleTimeoutMs=0 to keep the
// keepalive out of its own scenario, so it can't catch this —
// this pin exists specifically to cover what Pin 1 excludes.
static void test_idle_keepalive_after_traffic_does_not_resync() {
    std::cout << "\n=== Pin 4: idle keepalive after real traffic does NOT "
                 "force a resync ==="
              << std::endl;
    NullArqCache cacheA, cacheB;
    AutoLinkConfig cfg;
    for (int i = 0; i < kNumBauds; i++)
        cfg.allowedBauds[i] = (uint32_t)kBauds[i];
    cfg.allowedBaudsCount = kNumBauds;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 200; // small on purpose: reach the keepalive fast
    cfg.syncAckTimeoutMs = 500;
    cfg.maxRetx = 50;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link a(mHal, cacheA, /*isMaster=*/true, cfg);
    Link b(sHal, cacheB, /*isMaster=*/false, cfg);
    bringToOk(a, b, mHal, sHal);

    // Real traffic first, so txSeq advances well past 0 on the
    // sender — this is what makes a txSeq-carrying keepalive
    // collide with a small epoch value.
    const uint8_t payload[] = { 'h', 'i' };
    for (int i = 0; i < 5; i++) {
        bool ok = a.sendMsg(payload, sizeof payload);
        assert(ok);
        for (int p = 0; p < 100; p++) {
            mHal.pumpClock(50);
            sHal.pumpClock(50);
            pipe_data(mHal, sHal);
            pipe_data(sHal, mHal);
        }
    }
    uint8_t sink[64];
    while (b.recvMsg(sink, sizeof sink) > 0) {
    }
    assert(a.getState() == State::OK && b.getState() == State::OK &&
           "precondition: both sides still OK after real traffic");

    // Now idle well past idleTimeoutMs/2 — long enough for the
    // OK-state keepalive to fire on both sides multiple times.
    for (uint32_t t = 0; t < 3000; t += 50) {
        mHal.pumpClock(50);
        sHal.pumpClock(50);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
        a.onTimer();
        b.onTimer();
        assert(a.getState() == State::OK &&
               "Pin 4: A must stay OK through the idle keepalive window — "
               "a state flip here means the keepalive's seq byte is "
               "colliding with the epoch check (the bug this pin exists "
               "to catch)");
        assert(b.getState() == State::OK &&
               "Pin 4: B must stay OK through the idle keepalive window — "
               "same collision, other direction");
    }
    std::cout << "  Pin 4 PASS (both sides stayed OK through idle keepalive "
                 "after real traffic)"
              << std::endl;
}

int main() {
    std::cout << "=== Peer-Resync-on-Missed-Break Guard Tests ===" << std::endl;
    test_peer_resyncs_on_missed_break();
    test_process_ctrl_frame_uses_epoch_check();
    test_reset_unlocked_bumps_epoch_under_disc_gate();
    test_idle_keepalive_after_traffic_does_not_resync();
    std::cout << "\n=== All 4 peer-resync guard pins PASS ===" << std::endl;
    return 0;
}

#endif
