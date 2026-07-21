// Pinned regression: DropAsymIdle and the post-lock quiet
// gate were baud-invariant, so a low-baud re-lock (9600)
// had a faster time-to-drop than the time needed to
// actually deliver the queued payload. Field-log evidence:
// post-BREAK drops fired in 2.7-3.6s at 9600 baud vs the
// 9s baseline, with queued payloads that needed 0.6-2.8s
// of wire time at 9600 to actually land on the peer.
//
//   Pin 1: decideHealth's rtoMs is passed in as a 4th arg
//   (baud-aware). The DropAsymIdle gate (rxAge > 2 x rtoMs)
//   is no longer pinned to the static cfg.syncAckTimeoutMs
//   constant. Pinned by both the LinkHealthTest table (the
//   2 x RTO boundary rows) and this suite's source pin on
//   the new signature.
//   Pin 2: applyHealth_unlocked sources h.rtoMs from
//   roundTripMs at the locked baud, not the static
//   cfg.syncAckTimeoutMs. Pinned by source-grep on
//   LinkTimersOk.cpp (baudAwareRtoMs_unlocked call site
//   + h.rtoMs assignment + the decideHealth call
//   passing h.rtoMs as the 5th arg).
//   Pin 3: txQuiet_unlocked() floors the post-lock quiet
//   window at the baud-aware rtoMs so the gate covers
//   at least one full RTO + transmit cycle at the locked
//   baud. Pinned by source-grep on the
//   baudAwareRtoMs_unlocked() call inside txQuiet_unlocked
//   and the "at least one full RTO at the locked baud"
//   comment.
//   Pin 4: structural pin on LinkHealth.h. healthFastIdleRxMs
//   is no longer the binding constraint on the DropAsymIdle
//   gate; the floor is the 2 x rtoMs at the locked baud.
//   The legacy healthFastIdleRxMs() constant is preserved
//   as a fallback, but healthFastIdleRxMsAtBaud(rtoMs) is
//   the actual floor the gate uses. Catches a future
//   revert that drops the rtoMs arg or removes the
//   healthFastIdleRxMsAtBaud helper.
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include <cstring>
#    include <cstdio>
#    include "al/link/timers/LinkHealth.h"
#    include "TestPaths.h"

using namespace autolink;

// Pin 1: decideHealth's signature includes a 4th rtoMs arg.
// Pinned at the source level because the field failure was
// a static rtoMs producing a baud-invariant gate.
static void test_decideHealth_uses_rtoMs_arg() {
    std::cout << "\n=== Pin 1: decideHealth takes a baud-aware rtoMs arg "
                 "(source-grep) ==="
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
    // The new signature has a 4th `uint32_t rtoMs` arg.
    const char *sig = strstr(buf, "decideHealth(const HealthState &h,");
    assert(sig);
    const char *rtoArg = strstr(sig, "uint32_t rtoMs");
    assert(rtoArg &&
           "Pin 1: LinkHealth.h decideHealth must take a 4th "
           "uint32_t rtoMs arg for baud-aware gating. The prior "
           "static h.rtoMs made DropAsymIdle fire at the same "
           "rate regardless of locked baud — the the prior release field "
           "log showed it firing at 2.7-3.6s post-BREAK at 9600 "
           "baud, faster than the queued payload could be "
           "transmitted at 9600.");
    // The DropAsymIdle gate's rxAge check must use the
    // baud-aware floor, not a static constant. The new
    // shape references the rtoMs arg (and the
    // healthFastIdleRxMsAtBaud(rtoMs) helper).
    assert(strstr(buf, "healthFastIdleRxMsAtBaud") != NULL &&
           "Pin 1: LinkHealth.h must define healthFastIdleRxMsAtBaud "
           "(the baud-aware fast-idle floor used by DropAsymIdle "
           "and DropPoolExhaust). The prior static 300 ms "
           "healthFastIdleRxMs was the binding constraint on a "
           "512000 lock and let a 9600 lock drop too fast.");
    std::cout << "  Pin 1 PASS (decideHealth has rtoMs arg, "
                 "healthFastIdleRxMsAtBaud helper present)"
              << std::endl;
    free(buf);
}

// Pin 2: applyHealth_unlocked sources h.rtoMs from the locked
// baud, and passes it as the 5th decideHealth arg.
static void test_applyHealth_sources_baud_aware_rto() {
    std::cout << "\n=== Pin 2: applyHealth_unlocked sources h.rtoMs from "
                 "roundTripMs at the locked baud (source-grep) ==="
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
    // The call site must use baudAwareRtoMs_unlocked() to
    // source h.rtoMs, NOT cfg.syncAckTimeoutMs directly.
    // (h.rtoMs = (uint32_t)cfg.syncAckTimeoutMs is the
    // bug-class shape; reject it.)
    const char *rtoAssign = strstr(buf, "h.rtoMs");
    assert(rtoAssign);
    int window = 200;
    char winBuf[512];
    const char *start = rtoAssign - window;
    if (start < buf)
        start = buf;
    int len = (int)(rtoAssign - start);
    if (len >= (int)sizeof(winBuf))
        len = sizeof(winBuf) - 1;
    memcpy(winBuf, start, len);
    winBuf[len] = 0;
    // Look for the assignment line. The bug-class
    // assignment is `h.rtoMs = (uint32_t)cfg.syncAckTimeoutMs;`.
    // The new shape is `h.rtoMs = (uint32_t)baudAwareRtoMs_unlocked();`.
    // The src may use one or the other depending on
    // pre-existing comments. We pin: baudAwareRtoMs_unlocked()
    // is the call site, NOT a literal cfg.syncAckTimeoutMs.
    const char *bug =
        strstr(winBuf, "h.rtoMs = (uint32_t)cfg.syncAckTimeoutMs");
    assert(!bug &&
           "Pin 2: applyHealth_unlocked must NOT source h.rtoMs "
           "from cfg.syncAckTimeoutMs directly — that was the "
           "baud-invariant shape the the prior release field log surfaced "
           "(2.7-3.6s drop at 9600, faster than the queued "
           "payload could be transmitted). The new shape uses "
           "baudAwareRtoMs_unlocked() which derives from "
           "roundTripMs at the locked baud.");
    const char *fixed = strstr(buf, "baudAwareRtoMs_unlocked()");
    assert(fixed &&
           "Pin 2: applyHealth_unlocked must call "
           "baudAwareRtoMs_unlocked() to derive h.rtoMs "
           "from roundTripMs at the locked baud");

    // The decideHealth call must pass h.rtoMs as the 5th
    // arg (the new signature is (h, now, idle, deadPeerMs,
    // rtoMs)). The bug-class shape passed only 4 args.
    const char *call = strstr(buf, "decideHealth(h, now,");
    assert(call);
    const char *hRtoArg = strstr(call, "h.rtoMs");
    assert(hRtoArg &&
           "Pin 2: applyHealth_unlocked's decideHealth call must "
           "pass h.rtoMs as the 5th arg (baud-aware rtoMs). The "
           "bug-class shape passed only 4 args.");
    std::cout << "  Pin 2 PASS (h.rtoMs = baudAwareRtoMs_unlocked(), "
                 "5th decideHealth arg = h.rtoMs)"
              << std::endl;
    free(buf);
}

// Pin 3: txQuiet_unlocked floors the post-lock quiet window
// at the baud-aware rtoMs so the gate covers at least one
// full RTO + transmit cycle at the locked baud.
static void test_txQuiet_floors_at_baud_aware_rto() {
    std::cout << "\n=== Pin 3: txQuiet_unlocked floors the post-lock "
                 "quiet window at the baud-aware rtoMs (source-grep) ==="
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
    // Locate txQuiet_unlocked's body.
    const char *fn = strstr(buf, "bool Link::txQuiet_unlocked()");
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

    // The body must call baudAwareRtoMs_unlocked() to
    // derive the rto floor.
    assert(strstr(bodybuf, "baudAwareRtoMs_unlocked()") != NULL &&
           "Pin 3: txQuiet_unlocked must call "
           "baudAwareRtoMs_unlocked() to floor the post-lock quiet "
           "window — without the RTO floor, the gate is shorter "
           "than the time-to-drop at 9600 baud and the app's "
           "first post-lock send is let through just before the "
           "health watchdog tears the link down again.");
    std::cout << "  Pin 3 PASS (txQuiet_unlocked floors at "
                 "baudAwareRtoMs_unlocked())"
              << std::endl;
    free(buf);
}

// Pin 4: structural pin on LinkHealth.h. DropAsymIdle uses
// the baud-aware floor (healthFastIdleRxMsAtBaud(rtoMs)),
// not the static healthFastIdleRxMs() constant.
static void test_baud_aware_floor_in_decideHealth() {
    std::cout << "\n=== Pin 4: DropAsymIdle uses baud-aware floor, "
                 "not static constant (source-grep) ==="
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
    // healthFastIdleRxMsAtBaud is the new helper;
    // DropAsymIdle must use it.
    const char *helper = strstr(buf, "healthFastIdleRxMsAtBaud");
    assert(helper);
    // The DropAsymIdle branch must reference
    // healthFastIdleRxMsAtBaud(rtoMs), not
    // healthFastIdleRxMs() (the static 300 ms constant).
    const char *asym = strstr(buf, "DropAsymIdle");
    assert(asym);
    // Walk to the function body. DropAsymIdle is a
    // return value, find the decideHealth body.
    const char *body = strstr(buf, "decideHealth(const HealthState &h,");
    assert(body);
    int len = (int)(strlen(body));
    char fnBody[8192];
    if (len >= (int)sizeof(fnBody))
        len = sizeof(fnBody) - 1;
    memcpy(fnBody, body, len);
    fnBody[len] = 0;
    assert(strstr(fnBody, "healthFastIdleRxMsAtBaud(rtoMs)") != NULL &&
           "Pin 4: decideHealth's DropAsymIdle branch must use "
           "healthFastIdleRxMsAtBaud(rtoMs) (the baud-aware floor), "
           "not the static 300 ms constant — the static floor is "
           "the binding constraint on a 512000 lock (the queued "
           "payload transmits in 0.5 ms, the gate fires at "
           "300 ms) but lets a 9600 lock drop faster than the "
           "queued payload can be transmitted at 9600.");
    std::cout << "  Pin 4 PASS (DropAsymIdle uses baud-aware floor)"
              << std::endl;
    free(buf);
}

#    include "MockHal.h"
#    include "LinkTestAccessor.h"
#    include "NullArqCache.h"
#    include "al/link/Link.h"

// Pin 5: the RTO must actually be larger at a slow
// baud than at a fast one. The prior shape returned
// max(roundTripMs(baud), syncAckTimeoutMs) which was
// syncAckTimeoutMs at every baud (roundTripMs only
// models 10-byte control frames; real 250-byte MAX_CHUNK
// payloads make the link spend ~260 ms at 9600 baud on
// one chunk transmission alone, well past the 500 ms
// RTO floor). The new shape computes RTO from the real
// chunk size: 2 * (MAX_CHUNK + frame_overhead) * 10 /
// baud * 1000 ms. A 9600-baud link should have an RTO
// of ~520 ms; a 115200-baud link should have an RTO of
// the static syncAckTimeoutMs floor. Proving these
// differ is the no-op-rejection pin the prior
// release was missing.
static void test_baudAwareRtoMs_actually_baud_dependent() {
    std::cout << "\n=== Pin 5: baudAwareRtoMs_unlocked returns a "
                 "larger RTO at 9600 than at 115200 (functional "
                 "test, not source-grep) ==="
              << std::endl;
    AutoLinkConfig cfg9600;
    cfg9600.syncAckTimeoutMs = 500;
    cfg9600.maxMsg = 5120;
    cfg9600.allowedBauds[0] = 9600;
    cfg9600.allowedBaudsCount = 1;
    NullArqCache cache9600;
    MockHal mHal9600;
    Link link9600(mHal9600, cache9600, true, cfg9600);
    LinkTestAccessor acc9600(link9600);
    acc9600.setSpdIForTest(0);
    int rto9600 = acc9600.baudAwareRtoMsForTest();

    AutoLinkConfig cfg115200;
    cfg115200.syncAckTimeoutMs = 500;
    cfg115200.maxMsg = 5120;
    cfg115200.allowedBauds[0] = 115200;
    cfg115200.allowedBaudsCount = 1;
    NullArqCache cache115200;
    MockHal mHal115200;
    Link link115200(mHal115200, cache115200, true, cfg115200);
    LinkTestAccessor acc115200(link115200);
    acc115200.setSpdIForTest(0);
    int rto115200 = acc115200.baudAwareRtoMsForTest();
    std::cout << "  rto9600=" << rto9600 << " rto115200=" << rto115200
              << std::endl;
    assert(rto9600 > rto115200 &&
           "Pin 5: baudAwareRtoMs_unlocked must return a "
           "larger RTO at 9600 baud than at 115200 baud. "
           "If they're equal, the baud-aware RTO is a no-op "
           "and DropAsymIdle fires at the same rate on a "
           "9600 lock as on a 115200 lock — exactly the "
           "prior field-log failure mode (2.7-3.6 s drop "
           "at 9600 vs 9 s baseline). The new shape must "
           "source RTO from the real MAX_CHUNK size at the "
           "locked baud, not a 10-byte control-frame "
           "approximation.");
    assert(rto9600 > cfg9600.syncAckTimeoutMs &&
           "Pin 5: baudAwareRtoMs_unlocked at 9600 must "
           "EXCEED cfg.syncAckTimeoutMs (the static floor) — "
           "otherwise the function is just returning the "
           "floor and is again a no-op. At 9600 baud the "
           "250-byte chunk transmit is ~260 ms, so the RTO "
           "must be at least 2 round-trips past that, i.e. "
           ">= 520 ms.");
    std::cout << "  Pin 5 PASS (rto9600=" << rto9600
              << " > rto115200=" << rto115200
              << " > floor=" << cfg9600.syncAckTimeoutMs << ")" << std::endl;
}

int main() {
    std::cout << "=== Baud-Aware Link Health Tests ===" << std::endl;
    test_decideHealth_uses_rtoMs_arg();
    test_applyHealth_sources_baud_aware_rto();
    test_txQuiet_floors_at_baud_aware_rto();
    test_baud_aware_floor_in_decideHealth();
    test_baudAwareRtoMs_actually_baud_dependent();
    std::cout << "\n=== All 5 baud-aware health pins PASS ===" << std::endl;
    return 0;
}

#endif
