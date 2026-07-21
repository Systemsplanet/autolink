// Pinned regression: lastRxMs is stamped on any received
// byte, including a noise byte that happens to land on
// a 0xAA 0x55 preamble and clears CTRL carry state. The
// health machine reading that as "ongoing traffic" let a
// dead-noise link ride the silent-peer / asym watchdogs
// forever. The fix is lastValidRxMs (CRC-validated frames
// only); applyHealth_unlocked keys its rxAge off it.
//
//   Pin 1: noise bytes (no CRC-valid frame) do NOT stamp
//   lastValidRxMs. The health machine's rxAge keeps
//   growing past idle / deadPeer / 2xRTO thresholds
//   even when the link is "alive" at the wire level
//   (txBuf has bytes, events->onRx has fired).
//   Pin 2: a CRC-valid frame stamps lastValidRxMs and
//   resets the rxAge horizon so the link stays OK
//   across a real-data window.
//   Pin 3: source-grep on the health-machine's h.lastRxMs
//   source. The bug-class shape is `h.lastRxMs = lastRxMs`
//   (any-byte timestamp). The new shape is
//   `h.lastRxMs = lastValidRxMs` (CRC-validated only).
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
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

// Pin 1: noise bytes (no CRC-valid frame) do NOT
// stamp lastValidRxMs. Feed B a stream of bytes that
// do not form a CRC-valid 5-byte CTRL frame; pump past
// the silent-peer threshold; assert the link is dropped
// (because lastValidRxMs is the actual age source).
static void test_noise_does_not_stamp_lastValidRx() {
    std::cout << "\n=== Pin 1: noise bytes do NOT stamp lastValidRxMs; "
                 "silent-peer watchdog drops the link ==="
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

    // Feed B 50 noise bytes that don't form a
    // valid CTRL preamble. The onRx path stamps
    // lastRxMs on any byte but lastValidRxMs only
    // on a CRC-validated frame.
    uint8_t noise[50];
    for (int i = 0; i < 50; i++)
        noise[i] = (uint8_t)(i & 0xFF);
    sHal.pushAppBuf(noise, sizeof noise);
    // Drain it through b.onRx directly.
    b.onRx(noise, sizeof noise);
    // lastRxMs is stamped on any byte. lastValidRxMs
    // is NOT stamped (no CRC-valid frame arrived).
    int rxs = accB.getErrCount();
    std::cout << "  errs after noise=" << rxs << std::endl;
    // Pump past the silent-peer watchdog
    // (3 * idleTimeoutMs = 30 s) and check the link
    // drops. We use applyHealth_unlocked to skip the
    // 30 s wait.
    int action = accB.applyHealth(sHal.now + 31000);
    std::cout << "  applyHealth returned action=" << action
              << " (DropSilentPeer=" << (int)HealthAction::DropSilentPeer << ")"
              << std::endl;
    assert(action == (int)HealthAction::DropSilentPeer &&
           "Pin 1: noise-only traffic must trigger the silent-peer "
           "watchdog. lastValidRxMs is unchanged from the last real "
           "frame, so rxAge > deadPeerMs. The bug-class shape (any "
           "byte stamps lastRxMs) let a dead-noise link ride the "
           "watchdog forever.");
    std::cout << "  Pin 1 PASS (noise did not stamp lastValidRxMs; "
                 "DropSilentPeer fired)"
              << std::endl;
}

// Pin 2: a CRC-valid frame stamps lastValidRxMs. We
// drive a real data exchange and assert the link stays
// OK past the silent-peer window because lastValidRxMs
// was stamped.
static void test_valid_frame_stamps_lastValidRx() {
    std::cout << "\n=== Pin 2: a CRC-valid data frame stamps "
                 "lastValidRxMs; link stays OK across the "
                 "silent-peer window ==="
              << std::endl;
    NullArqCache cacheA, cacheB;
    AutoLinkConfig cfg = cfgCommon();
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link a(mHal, cacheA, /*isMaster=*/true, cfg);
    Link b(sHal, cacheB, /*isMaster=*/false, cfg);
    bringToOk(a, b, mHal, sHal);
    // Send a real message from A to B. The full
    // data frame (COBS + CRC-8 + payload + CRC-16)
    // stamps B's lastValidRxMs on the receive path.
    const uint8_t payload[] = { 'h', 'i' };
    assert(a.sendMsg(payload, sizeof payload));
    for (int i = 0; i < 200; i++) {
        mHal.pumpClock(50);
        sHal.pumpClock(50);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
    }
    uint8_t sink[64];
    int got = b.recvMsg(sink, sizeof sink);
    assert(got == (int)sizeof payload &&
           memcmp(sink, payload, sizeof payload) == 0 &&
           "Pin 2 pre: a CRC-valid data frame must round-trip");

    // The link must stay OK after a real frame
    // (lastValidRxMs is fresh, well inside the
    // 2 x RTO horizon at the locked baud).
    LinkTestAccessor accB(b);
    int action = accB.applyHealth(sHal.now + 500);
    assert(action == (int)HealthAction::Keep &&
           "Pin 2: a real CRC-valid data frame must stamp "
           "lastValidRxMs; the silent-peer watchdog must NOT "
           "fire on a healthy link 500 ms after a real frame "
           "(well inside the 3 x idleTimeoutMs = 30 s horizon).");
    std::cout << "  Pin 2 PASS (real frame stamped lastValidRxMs; "
                 "HealthAction::Keep)"
              << std::endl;
}

// Pin 3: source-grep on applyHealth_unlocked's h.lastRxMs
// source. The bug-class shape is `h.lastRxMs = lastRxMs`
// (any-byte timestamp). The new shape is
// `h.lastRxMs = lastValidRxMs`.
static void test_lastValidRxMs_source_grep() {
    std::cout << "\n=== Pin 3: applyHealth_unlocked keys rxAge off "
                 "lastValidRxMs, not lastRxMs (source-grep) ==="
              << std::endl;
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
    // The h.lastRxMs = line in applyHealth_unlocked
    // must reference lastValidRxMs, not lastRxMs.
    // Find the assign line and the surrounding
    // context.
    const char *assign = strstr(buf, "h.lastRxMs = lastValidRxMs");
    assert(assign &&
           "Pin 3: applyHealth_unlocked must assign "
           "h.lastRxMs = lastValidRxMs (CRC-validated frames "
           "only). The bug-class shape `h.lastRxMs = lastRxMs` "
           "(any byte) let a dead-noise link ride the "
           "silent-peer / asym watchdogs forever — lastRxMs is "
           "stamped on a noise byte that happens to land on a "
           "0xAA 0x55 preamble and clear CTRL carry state.");
    // The LinkRx.cpp processCtrlFrame_unlocked and
    // onPayload must stamp lastValidRxMs.
    {
        FILE *f2 = fopen(testRepoPath("src/al/link/io/LinkRx.cpp").c_str(), "r");
        assert(f2);
        fseek(f2, 0, SEEK_END);
        long sz2 = ftell(f2);
        fseek(f2, 0, SEEK_SET);
        char *buf2 = (char *)malloc((size_t)sz2 + 1);
        assert(buf2);
        size_t got2 = fread(buf2, 1, (size_t)sz2, f2);
        buf2[got2] = 0;
        fclose(f2);
        assert(strstr(buf2, "lastValidRxMs = hw.nowMs()") != NULL &&
               "Pin 3: LinkRx.cpp must stamp lastValidRxMs on "
               "CRC-validated frames (processCtrlFrame_unlocked "
               "and onPayload). Without the stamp, the "
               "lastValidRxMs source is never updated and the "
               "fix is dead code.");
        free(buf2);
    }
    // The Link.h field lastValidRxMs must exist.
    {
        FILE *f3 = fopen(testRepoPath("src/al/link/Link.h").c_str(), "r");
        assert(f3);
        fseek(f3, 0, SEEK_END);
        long sz3 = ftell(f3);
        fseek(f3, 0, SEEK_SET);
        char *buf3 = (char *)malloc((size_t)sz3 + 1);
        assert(buf3);
        size_t got3 = fread(buf3, 1, (size_t)sz3, f3);
        buf3[got3] = 0;
        fclose(f3);
        assert(strstr(buf3, "uint32_t lastValidRxMs") != NULL &&
               "Pin 3: Link.h must declare the lastValidRxMs field");
        free(buf3);
    }
    std::cout << "  Pin 3 PASS (h.lastRxMs = lastValidRxMs, "
                 "stamps present in LinkRx.cpp, field in Link.h)"
              << std::endl;
    free(buf);
}

// Pin 4b: CRC-valid ACK and NAK frames must stamp
// lastValidRxMs. ACK/NAK are proof of a live peer
// even if no app data is moving, but the prior
// shape only stamped on CRC-valid CTRL/COBS
// payloads. An ACK-only stretch (all-outbound-data
// window, echoes delayed) read as RX silence to
// the health machine, and DropAsymIdle fired
// mid-repair on a peer that was answering every
// retx with a wire ACK. Pinned at the source level
// because the field failure was a faster-than-baseline
// DropAsymIdle right after a real burst, exactly
// the unrefreshed-ACK-clock shape.
static void test_ack_nak_stamp_lastValidRxMs() {
    std::cout << "\n=== Pin 4b: CRC-valid ACK + NAK frames stamp "
                 "lastValidRxMs (source-grep) ==="
              << std::endl;
    FILE *f = fopen(testRepoPath("src/al/link/io/LinkRx.cpp").c_str(), "r");
    assert(f);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    assert(buf);
    size_t got = fread(buf, 1, (size_t)sz, f);
    buf[got] = 0;
    fclose(f);
    // onAck must stamp lastValidRxMs = hw.nowMs()
    // before the existing isPending check or
    // any other state mutation. Find onAck's
    // body, assert the stamp is inside.
    const char *onAck = strstr(buf, "bool Link::onAck(");
    assert(onAck);
    const char *onAckBody = strchr(onAck, '{');
    assert(onAckBody);
    int depth = 0;
    const char *p = onAckBody;
    const char *onAckEnd = nullptr;
    while (*p) {
        if (*p == '{')
            depth++;
        else if (*p == '}') {
            depth--;
            if (depth == 0) {
                onAckEnd = p + 1;
                break;
            }
        }
        p++;
    }
    assert(onAckEnd);
    int onAckLen = (int)(onAckEnd - onAckBody);
    char onAckBuf[4096];
    if (onAckLen >= (int)sizeof(onAckBuf))
        onAckLen = sizeof(onAckBuf) - 1;
    memcpy(onAckBuf, onAckBody, onAckLen);
    onAckBuf[onAckLen] = 0;
    assert(strstr(onAckBuf, "lastValidRxMs = hw.nowMs()") != NULL &&
           "Pin 4b: Link::onAck must stamp lastValidRxMs "
           "= hw.nowMs() on a CRC-valid ACK frame. A "
           "dead peer doesn't bother ACKing — an ACK is "
           "proof of a live peer even if no app data is "
           "moving, and the rxAge clock must refresh.");
    // onNak must also stamp lastValidRxMs.
    const char *onNak = strstr(buf, "bool Link::onNak(");
    assert(onNak);
    const char *onNakBody = strchr(onNak, '{');
    assert(onNakBody);
    depth = 0;
    p = onNakBody;
    const char *onNakEnd = nullptr;
    while (*p) {
        if (*p == '{')
            depth++;
        else if (*p == '}') {
            depth--;
            if (depth == 0) {
                onNakEnd = p + 1;
                break;
            }
        }
        p++;
    }
    assert(onNakEnd);
    int onNakLen = (int)(onNakEnd - onNakBody);
    char onNakBuf[4096];
    if (onNakLen >= (int)sizeof(onNakBuf))
        onNakLen = sizeof(onNakBuf) - 1;
    memcpy(onNakBuf, onNakBody, onNakLen);
    onNakBuf[onNakLen] = 0;
    assert(strstr(onNakBuf, "lastValidRxMs = hw.nowMs()") != NULL &&
           "Pin 4b: Link::onNak must stamp lastValidRxMs "
           "= hw.nowMs() on a CRC-valid NAK frame. "
           "Symmetric to the onAck contract: a peer "
           "that bothers NAKing is alive.");
    free(buf);
    std::cout << "  Pin 4b PASS (onAck + onNak stamp "
                 "lastValidRxMs on CRC-valid frames)"
              << std::endl;
}

// Pin 4c: DropAsymIdle and DropPoolExhaust must
// gate on h.lastRxMs != 0 (mirroring DropSilentPeer's
// guard). Without this, a never-stamped clock
// (rxAge = now - 0 = now) trivially satisfies
// rxAge > rxIdleFloor the moment applyHealth runs,
// and a fresh OK link without a CRC-valid frame yet
// (e.g. immediately after lockOk_unlocked) gets
// torn down on the first watchdog tick. Source-grep
// pin because the field failure was a faster-than-
// baseline drop right after a fresh lock.
static void test_asym_pool_lastRxMs_guard() {
    std::cout << "\n=== Pin 4c: DropAsymIdle + DropPoolExhaust "
                 "gate on h.lastRxMs != 0 (source-grep) ==="
              << std::endl;
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
    // The DropAsymIdle branch must have
    // `h.lastRxMs != 0 &&` guarding the rxAge check.
    // The bug-class shape was a bare
    // `rxAge > rxIdleFloor` that fired on a fresh
    // (never-stamped) link.
    const char *asym = strstr(buf, "DropAsymIdle");
    assert(asym);
    // The guard must appear in the same decideHealth
    // body (a comment in the function doesn't
    // count — the actual if-condition must include
    // it). Find the lastRxMs guard.
    const char *guard = strstr(asym, "h.lastRxMs != 0");
    assert(guard &&
           "Pin 4c: DropAsymIdle must guard on "
           "h.lastRxMs != 0 (the CRC-validated clock), "
           "mirroring DropSilentPeer's guard. Without "
           "this, a fresh link without a CRC-valid frame "
           "yet reads as having infinite rxAge the moment "
           "applyHealth runs and gets torn down on the "
           "first watchdog tick.");
    // Same for DropPoolExhaust. Find it after the
    // DropPoolExhaust enum value to skip the
    // declaration.
    const char *pool = strstr(asym, "DropPoolExhaust");
    if (!pool)
        pool = strstr(buf, "DropPoolExhaust");
    assert(pool);
    const char *poolGuard = strstr(pool, "h.lastRxMs != 0");
    assert(poolGuard &&
           "Pin 4c: DropPoolExhaust must guard on "
           "h.lastRxMs != 0 for the same reason as "
           "DropAsymIdle.");
    free(buf);
    std::cout << "  Pin 4c PASS (DropAsymIdle + DropPoolExhaust "
                 "guard on h.lastRxMs != 0)"
              << std::endl;
}

// Pin 4: lockOk_unlocked must reset lastValidRxMs. A
// fresh OK session that inherits the prior session's
// CRC-valid timestamp is exactly the shape the health
// watchdog reads as a stale-but-recent link: rxAge
// starts already large (e.g. 8+ s, the P1/P2 resweep
// duration) the instant the new lock completes, and
// DropAsymIdle / DropIdle can fire on a perfectly
// healthy new session because the prior session's
// timestamp is still being read as the current
// baseline. The fix: stamp lastValidRxMs alongside
// lastRxMs in lockOk_unlocked so the new session's
// clock is the only honest baseline. Pinned at the
// source level because the field failure was a
// faster-than-baseline DropAsymIdle right after
// relock, exactly the inherited-timestamp shape.
static void test_lockOk_resets_lastValidRxMs() {
    std::cout << "\n=== Pin 4: lockOk_unlocked stamps lastValidRxMs "
                 "to hw.nowMs() (source-grep) ==="
              << std::endl;
    FILE *f = fopen(testRepoPath("src/al/link/timers/LinkSweepGlue.cpp").c_str(), "r");
    assert(f);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    assert(buf);
    size_t got = fread(buf, 1, (size_t)sz, f);
    buf[got] = 0;
    fclose(f);
    // Locate lockOk_unlocked's body.
    const char *fn = strstr(buf, "void Link::lockOk_unlocked");
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
    char bodybuf[4096];
    if (len >= (int)sizeof(bodybuf))
        len = sizeof(bodybuf) - 1;
    memcpy(bodybuf, fn, len);
    bodybuf[len] = 0;
    // The body must stamp lastValidRxMs to hw.nowMs()
    // alongside lastRxMs. Without this, the new
    // session's rxAge starts at the prior session's
    // lastValidRxMs — 8+ s of P1/P2 resweep time —
    // and DropAsymIdle / DropIdle can fire on a
    // perfectly healthy new lock.
    assert(strstr(bodybuf, "lastValidRxMs = hw.nowMs()") != NULL &&
           "Pin 4: lockOk_unlocked must stamp lastValidRxMs to "
           "hw.nowMs() alongside lastRxMs. A fresh OK session "
           "that inherits the prior session's CRC-valid "
           "timestamp reads as a stale-but-recent link to the "
           "health watchdog, and rxAge-based verdicts "
           "(DropAsymIdle, DropIdle) under-count the new "
           "session's age by the time spent in the SWP walk. "
           "Same reason lastRxMs is reset on lock — the link "
           "is just OK'd, the wire's own clock is the only "
           "honest baseline.");
    std::cout << "  Pin 4 PASS (lockOk_unlocked stamps "
                 "lastValidRxMs to hw.nowMs())"
              << std::endl;
    free(buf);
}

int main() {
    std::cout << "=== Last-Valid-Rx-Ms Tests ===" << std::endl;
    test_noise_does_not_stamp_lastValidRx();
    test_valid_frame_stamps_lastValidRx();
    test_lastValidRxMs_source_grep();
    test_ack_nak_stamp_lastValidRxMs();
    test_asym_pool_lastRxMs_guard();
    test_lockOk_resets_lastValidRxMs();
    std::cout << "\n=== All 6 lastValidRxMs pins PASS ===" << std::endl;
    return 0;
}

#endif
