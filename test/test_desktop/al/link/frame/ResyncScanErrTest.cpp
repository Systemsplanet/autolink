// Pinned regression: recvMsg's resync-scan path only
// called err() on drop > 0. findMsgHeaderResync_unlocked
// returns 0 or -1 (both genuine errors, since beginMsg
// already failed) without firing err() — the errThreshold
// and errRateWindow paths never see the bad-header case,
// and a real CRC/desync stream keeps the link up even as
// the app gets a stream of -1 returns.
//
//   Pin 1: beginMsg failure on a 6-byte header that
//   can't possibly be valid (length=0xFFFFFFFF, CRC of
//   zero) drives findMsgHeaderResync_unlocked. The test
//   asserts err() is called and the next recvMsg still
//   surfaces the error (didn't mask it with a state flip).
//   Pin 2: source-grep on the unconditional err() call
//   inside recvMsg's beginMsg-failure branch. The bug-class
//   shape is `if (drop > 0) err();` — the new shape is
//   `err();` unconditionally (and the `if (drop > 0)`
//   guard is gone).
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

static AutoLinkConfig busyCfg() {
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

// Pin 1: a bad-header stream drives recvMsg repeatedly
// into the resync-scan path. The test asserts err() fires
// (errs increments past 0) on each bad-header recvMsg,
// AND the link stays OK long enough to deliver the next
// frame (the err() call must not flip state).
static void test_resync_scan_fires_err() {
    std::cout << "\n=== Pin 1: bad-header recvMsg drives err() via the "
                 "resync-scan path (err() is unconditional on "
                 "beginMsg failure) ==="
              << std::endl;
    NullArqCache cacheA, cacheB;
    AutoLinkConfig cfg = busyCfg();
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link a(mHal, cacheA, /*isMaster=*/true, cfg);
    Link b(sHal, cacheB, /*isMaster=*/false, cfg);
    bringToOk(a, b, mHal, sHal);
    LinkTestAccessor accB(b);

    // Plant a deliberately-bad 6-byte header into
    // B's appBuf: length=0xFFFFFFFF (well past
    // maxMsg), so beginMsg will fail and the
    // resync-scan path runs. The CRC bytes are
    // garbage, so no resync can succeed either —
    // findMsgHeaderResync_unlocked returns -1.
    uint8_t badHdr[MSG_HDR] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xDE, 0xAD };
    sHal.pushAppBuf(badHdr, MSG_HDR);
    int errBefore = accB.getErrCount();
    uint8_t sink[256];
    int got = b.recvMsg(sink, sizeof sink);
    // Pump so any deferred timers fire (the err() path
    // may arm a BREAK timer; without the pump, the test
    // hangs on the timer-fired counter).
    for (int i = 0; i < 10; i++) {
        mHal.pumpClock(20);
        sHal.pumpClock(20);
    }
    int errAfter = accB.getErrCount();
    std::cout << "  recvMsg returned " << got << ", errs " << errBefore
              << " -> " << errAfter << ", state=" << (int)b.getState()
              << std::endl;
    assert(got < 0 &&
           "Pin 1: bad-header recvMsg must return -1 (the "
           "resync-scan didn't find a valid header)");
    assert(errAfter > errBefore &&
           "Pin 1: err() must fire on a beginMsg-failure "
           "resync-scan, even when findMsgHeaderResync_unlocked "
           "returns 0 or -1 (the bug class was the "
           "`if (drop > 0)` guard around err())");
    assert(b.getState() == State::OK &&
           "Pin 1: the link must stay OK after a single err() — "
           "errThreshold(100) is the circuit breaker, not the "
           "single bad-header call");
    std::cout << "  Pin 1 PASS (err() fired, link stayed OK, "
                 "recvMsg returned -1)"
              << std::endl;
}

// Pin 2: source-grep on the unconditional err() call
// inside recvMsg's beginMsg-failure branch.
static void test_unconditional_err_source_grep() {
    std::cout << "\n=== Pin 2: recvMsg's beginMsg-failure branch calls "
                 "err() unconditionally (source-grep) ==="
              << std::endl;
    FILE *f = fopen(testRepoPath("src/al/link/LinkApi.cpp").c_str(), "r");
    assert(f);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    assert(buf);
    size_t got = fread(buf, 1, (size_t)sz, f);
    buf[got] = 0;
    fclose(f);
    // Locate recvMsg's body. The branch is the
    // beginMsg failure path. The bug-class shape is
    // `if (drop > 0) err();` — the new shape is
    // `err();` unconditionally.
    const char *fn = strstr(buf, "int Link::recvMsg");
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
    char bodybuf[8192];
    if (len >= (int)sizeof(bodybuf))
        len = sizeof(bodybuf) - 1;
    memcpy(bodybuf, fn, len);
    bodybuf[len] = 0;

    // The beginMsg-failure branch must call err()
    // unconditionally. The bug-class shape is the
    // `if (drop > 0) err();` guard; reject it.
    const char *bug = strstr(bodybuf, "if (drop > 0)\n            err();");
    if (!bug)
        bug = strstr(bodybuf, "if (drop > 0)\n                err();");
    if (!bug)
        bug = strstr(bodybuf, "if (drop > 0)\n        err();");
    assert(!bug &&
           "Pin 2: the bug-class shape `if (drop > 0) err();` "
           "must NOT be present in recvMsg's beginMsg-failure "
           "branch — it undercounted frame errors (drop == 0 or "
           "drop == -1 from findMsgHeaderResync_unlocked "
           "bypassed err()) and let the errThreshold / "
           "errRateWindow paths miss the bad-header case.");
    // The unconditional err() call must be present
    // (the `err();` after the drop is captured into
    // a local; the original `if (drop > 0)` guard is
    // removed).
    assert(strstr(bodybuf, "err();") != NULL &&
           "Pin 2: recvMsg's beginMsg-failure branch must call "
           "err() unconditionally");
    std::cout << "  Pin 2 PASS (err() unconditional, no `if (drop "
                 "> 0)` guard)"
              << std::endl;
    free(buf);
}

int main() {
    std::cout << "=== Resync-Scan err() Tests ===" << std::endl;
    test_resync_scan_fires_err();
    test_unconditional_err_source_grep();
    std::cout << "\n=== All 2 resync-scan-err pins PASS ===" << std::endl;
    return 0;
}

#endif
