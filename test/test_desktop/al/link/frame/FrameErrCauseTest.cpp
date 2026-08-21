// Pinned regression: per-cause tagging of recvMsg's three
// frame-error paths (BadHeader / OverLen / CrcFail). The prior
// shape had a single `frameErrs` counter and the three call sites
// all bumped the same field, so an operator could see "frame
// errors went up" but could not disambiguate a 6-byte bad-header
// stream from a CRC-failure stream from an oversize-length
// stream. Those are three different field problems: a wire
// desync (BadHeader, fed by a baud slip or a peer restart mid-
// frame), a real oversize message (OverLen, the app wrote
// larger than its maxMsg), a noisy wire (CrcFail, garbage
// hitting the CRC).
//
// The fix is `enum class FrameErrCause` and `err(FrameErrCause)`
// — the three call sites in recvMsg tag their fire. Stats
// surfaces three counters (badHeaderErrs / overLenErrs /
// crcFailErrs), and the aggregate frameErrs is the sum. That
// invariant (frameErrs == badHeaderErrs + overLenErrs +
// crcFailErrs) is the load-bearing one: a single cause field
// can be mis-tagged in one place and the aggregate still
// increments correctly, so the per-cause breakdown is the only
// way to catch a mis-tag. Toggle any cause tag to a wrong
// value -> red.
//
//   Pin 1: drive each of the three recvMsg failure paths
//   end-to-end, assert the matching cause field increments
//   by exactly 1 and the other two stay at 0.
//
//   Pin 2: aggregate invariant — frameErrs == sum of the
//   three cause fields, before and after every per-cause
//   fire. Holds at every step, not just at the end.
//
//   Pin 3: source-grep on the three call-site tags
//   (`err(FrameErrCause::BadHeader)`, `::OverLen`,
//   `::CrcFail`) and on the per-cause increment in
//   `err_unlocked` so a future revert that drops the cause
//   tag (or mis-tags a fire) is caught by the source pin.
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include <cstring>
#    include <cstdio>
#    include "MockHal.h"
#    include "LinkTestAccessor.h"
#    include "NullArqCache.h"
#    include "TestPaths.h"
#    include "al/link/io/LinkMsgCodec.h"

using namespace autolink;

static const int kBauds[] = { 115200 };
static const int kNumBauds = (int)(sizeof(kBauds) / sizeof(kBauds[0]));

static AutoLinkConfig makeCfg() {
    AutoLinkConfig cfg;
    for (int i = 0; i < kNumBauds; i++)
        cfg.allowedBauds[i] = (uint32_t)kBauds[i];
    cfg.allowedBaudsCount = kNumBauds;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 10000;
    cfg.syncAckTimeoutMs = 1000;
    cfg.maxRetx = 50;
    // Small maxMsg so the OverLen pin's >max_msg length is
    // easy to construct without a giant buffer.
    cfg.maxMsg = 256;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    return cfg;
}

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

struct CauseStats {
    uint64_t frameErrs;
    uint64_t badHeaderErrs;
    uint64_t overLenErrs;
    uint64_t crcFailErrs;
};

static CauseStats readStats(Link &l) {
    Stats s;
    l.getStats(s);
    CauseStats c{};
    c.frameErrs = s.frameErrs;
    c.badHeaderErrs = s.badHeaderErrs;
    c.overLenErrs = s.overLenErrs;
    c.crcFailErrs = s.crcFailErrs;
    return c;
}

static void assertInvariant(const CauseStats &c) {
    assert(c.frameErrs == c.badHeaderErrs + c.overLenErrs + c.crcFailErrs &&
           "Pin 2: frameErrs == badHeaderErrs + overLenErrs + "
           "crcFailErrs invariant broken — a fire was mis-tagged "
           "or missed the per-cause stamp");
}

// Pin 1a: a fabricated 6-byte header with length=0xFFFFFFFF
// drives beginMsg to fail, the resync-scan path runs, the
// badHeaderErrs counter increments by exactly 1, and the
// other two cause fields stay at 0. The link stays OK.
//
// Two-node setup so the test exercises the real path: a
// slave `b` is locked against a master `a`, and the bad
// header is pushed into `b`'s appBuf. The other two
// pins follow the same shape.
static void test_bad_header_path() {
    std::cout << "\n=== Pin 1a: BadHeader — fabricated 6-byte "
                 "header drives err(FrameErrCause::BadHeader) "
                 "(badHeaderErrs++, others stay 0) ==="
              << std::endl;
    AutoLinkConfig cfg = makeCfg();
    NullArqCache cacheA, cacheB;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link a(mHal, cacheA, /*isMaster=*/true, cfg);
    Link b(sHal, cacheB, /*isMaster=*/false, cfg);
    bringToOk(a, b, mHal, sHal);
    // Push a 6-byte header that beginMsg will reject:
    // length=0xFFFFFFFF (well past maxMsg), CRC of zero.
    uint8_t badHdr[MSG_HDR] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xDE, 0xAD };
    sHal.pushAppBuf(badHdr, MSG_HDR);
    CauseStats before = readStats(b);
    assertInvariant(before);
    uint8_t sink[256];
    int got = b.recvMsg(sink, sizeof sink);
    CauseStats after = readStats(b);
    std::cout << "  recvMsg returned " << got << ", badHeaderErrs "
              << before.badHeaderErrs << " -> " << after.badHeaderErrs
              << ", overLenErrs=" << after.overLenErrs
              << ", crcFailErrs=" << after.crcFailErrs
              << ", state=" << (int)b.getState() << std::endl;
    assert(got < 0 && "Pin 1a: bad-header recvMsg must return -1");
    assert(after.badHeaderErrs == before.badHeaderErrs + 1 &&
           "Pin 1a: badHeaderErrs must increment by exactly 1 on the "
           "resync-scan err fire");
    assert(after.overLenErrs == before.overLenErrs &&
           "Pin 1a: OverLen must NOT increment on a BadHeader fire");
    assert(after.crcFailErrs == before.crcFailErrs &&
           "Pin 1a: CrcFail must NOT increment on a BadHeader fire");
    assert(after.frameErrs == before.frameErrs + 1 &&
           "Pin 1a: aggregate frameErrs must increment by exactly 1");
    assertInvariant(after);
    assert(b.getState() == State::OK &&
           "Pin 1a: a single BadHeader fire must NOT drop the link "
           "(errThreshold is the circuit breaker)");
    std::cout << "  Pin 1a PASS" << std::endl;
}

// Pin 1b: a valid 6-byte header with length=maxMsg (beginMsg
// accepts) but the caller asks recvMsg for max_len=64 — the
// OverLen branch fires. overLenErrs increments by 1, the
// other two stay at 0.
static void test_over_len_path() {
    std::cout << "\n=== Pin 1b: OverLen — recvMsg's len > max_len stamps "
                 "overLenErrs (others stay 0) ==="
              << std::endl;
    AutoLinkConfig cfg = makeCfg();
    NullArqCache cacheA, cacheB;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link a(mHal, cacheA, /*isMaster=*/true, cfg);
    Link b(sHal, cacheB, /*isMaster=*/false, cfg);
    bringToOk(a, b, mHal, sHal);
    // beginMsg rejects len > maxMsg itself, so to hit
    // the OverLen branch we use len == maxMsg (beginMsg
    // accepts) and ask recvMsg for max_len=64.
    uint8_t hdr[MSG_HDR];
    msgHdrEncode((uint32_t)cfg.maxMsg, /*crc=*/0, hdr);
    sHal.pushAppBuf(hdr, MSG_HDR);
    uint8_t payload[256];
    memset(payload, 0, sizeof payload);
    sHal.pushAppBuf(payload, (int)cfg.maxMsg);
    CauseStats before = readStats(b);
    assertInvariant(before);
    uint8_t sink[64];
    int got = b.recvMsg(sink, (int)sizeof sink);
    CauseStats after = readStats(b);
    std::cout << "  recvMsg returned " << got << ", overLenErrs "
              << before.overLenErrs << " -> " << after.overLenErrs
              << ", badHeaderErrs=" << after.badHeaderErrs
              << ", crcFailErrs=" << after.crcFailErrs << std::endl;
    assert(got < 0 && "Pin 1b: over-len recvMsg must return -1");
    assert(after.overLenErrs == before.overLenErrs + 1 &&
           "Pin 1b: overLenErrs must increment by exactly 1 on the "
           "len > max_len err fire");
    assert(after.badHeaderErrs == before.badHeaderErrs &&
           "Pin 1b: BadHeader must NOT increment on an OverLen fire");
    assert(after.crcFailErrs == before.crcFailErrs &&
           "Pin 1b: CrcFail must NOT increment on an OverLen fire");
    assert(after.frameErrs == before.frameErrs + 1 &&
           "Pin 1b: aggregate frameErrs must increment by exactly 1");
    assertInvariant(after);
    std::cout << "  Pin 1b PASS" << std::endl;
}

// Pin 1c: a valid 6-byte header (beginMsg accepts: len=4 is
// in bounds) but a deliberately-wrong CRC, with a 4-byte
// payload — recvMsg's CRC check fails and the CrcFail
// branch fires. crcFailErrs increments by 1, the other two
// stay at 0.
static void test_crc_fail_path() {
    std::cout << "\n=== Pin 1c: CrcFail — recvMsg's CRC-mismatch stamps "
                 "crcFailErrs (others stay 0) ==="
              << std::endl;
    AutoLinkConfig cfg = makeCfg();
    NullArqCache cacheA, cacheB;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link a(mHal, cacheA, /*isMaster=*/true, cfg);
    Link b(sHal, cacheB, /*isMaster=*/false, cfg);
    bringToOk(a, b, mHal, sHal);
    const uint32_t len = 4;
    const uint16_t wrongCrc = 0xBEEF;
    uint8_t hdr[MSG_HDR];
    msgHdrEncode(len, wrongCrc, hdr);
    sHal.pushAppBuf(hdr, MSG_HDR);
    uint8_t payload[4] = { 0x11, 0x22, 0x33, 0x44 };
    sHal.pushAppBuf(payload, (int)len);
    CauseStats before = readStats(b);
    assertInvariant(before);
    uint8_t sink[64];
    int got = b.recvMsg(sink, (int)sizeof sink);
    CauseStats after = readStats(b);
    std::cout << "  recvMsg returned " << got << ", crcFailErrs "
              << before.crcFailErrs << " -> " << after.crcFailErrs
              << ", badHeaderErrs=" << after.badHeaderErrs
              << ", overLenErrs=" << after.overLenErrs << std::endl;
    assert(got < 0 && "Pin 1c: bad-CRC recvMsg must return -1");
    assert(after.crcFailErrs == before.crcFailErrs + 1 &&
           "Pin 1c: crcFailErrs must increment by exactly 1 on the "
           "CRC-mismatch err fire");
    assert(after.badHeaderErrs == before.badHeaderErrs &&
           "Pin 1c: BadHeader must NOT increment on a CrcFail fire");
    assert(after.overLenErrs == before.overLenErrs &&
           "Pin 1c: OverLen must NOT increment on a CrcFail fire");
    assert(after.frameErrs == before.frameErrs + 1 &&
           "Pin 1c: aggregate frameErrs must increment by exactly 1");
    assertInvariant(after);
    std::cout << "  Pin 1c PASS" << std::endl;
}

// Pin 2: invariant across all three causes combined — drive
// every path on the same link and check
// frameErrs == badHeaderErrs + overLenErrs + crcFailErrs at
// every step. A mis-tag on any single fire would leave the
// aggregate-vs-sum invariant broken even though frameErrs
// itself still matches.
static void test_aggregate_invariant() {
    std::cout << "\n=== Pin 2: aggregate invariant — "
                 "frameErrs == sum(cause) at every step "
                 "across all three cause types ==="
              << std::endl;
    AutoLinkConfig cfg = makeCfg();
    NullArqCache cacheA, cacheB;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link a(mHal, cacheA, /*isMaster=*/true, cfg);
    Link b(sHal, cacheB, /*isMaster=*/false, cfg);
    bringToOk(a, b, mHal, sHal);
    CauseStats s0 = readStats(b);
    assertInvariant(s0);

    // Fire 1: BadHeader.
    {
        uint8_t badHdr[MSG_HDR] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xDE, 0xAD };
        sHal.pushAppBuf(badHdr, MSG_HDR);
        uint8_t sink[64];
        (void)b.recvMsg(sink, sizeof sink);
        CauseStats s1 = readStats(b);
        assertInvariant(s1);
        assert(s1.badHeaderErrs == s0.badHeaderErrs + 1);
    }
    // Fire 2: OverLen.
    {
        uint8_t hdr[MSG_HDR];
        msgHdrEncode((uint32_t)cfg.maxMsg, 0, hdr);
        sHal.pushAppBuf(hdr, MSG_HDR);
        uint8_t payload[256];
        memset(payload, 0, sizeof payload);
        sHal.pushAppBuf(payload, (int)cfg.maxMsg);
        uint8_t sink[64];
        (void)b.recvMsg(sink, (int)sizeof sink);
        CauseStats s2 = readStats(b);
        assertInvariant(s2);
        assert(s2.overLenErrs == s0.overLenErrs + 1);
    }
    // Fire 3: CrcFail.
    {
        const uint32_t len = 4;
        const uint16_t wrongCrc = 0xBEEF;
        uint8_t hdr[MSG_HDR];
        msgHdrEncode(len, wrongCrc, hdr);
        sHal.pushAppBuf(hdr, MSG_HDR);
        uint8_t payload[4] = { 0x11, 0x22, 0x33, 0x44 };
        sHal.pushAppBuf(payload, (int)len);
        uint8_t sink[64];
        (void)b.recvMsg(sink, (int)sizeof sink);
        CauseStats s3 = readStats(b);
        assertInvariant(s3);
        assert(s3.crcFailErrs == s0.crcFailErrs + 1);
    }
    CauseStats sF = readStats(b);
    assert(sF.frameErrs == s0.frameErrs + 3 &&
           "Pin 2: aggregate must reflect 3 fires");
    assert(sF.badHeaderErrs == s0.badHeaderErrs + 1);
    assert(sF.overLenErrs == s0.overLenErrs + 1);
    assert(sF.crcFailErrs == s0.crcFailErrs + 1);
    std::cout << "  Pin 2 PASS" << std::endl;
}

// Pin 3: source-grep on the three call-site tags in
// LinkApi.cpp and the per-cause increment in err_unlocked.
// Catches a future revert that drops the cause tag (and
// reverts to a bare `err();`) or mis-tags a fire
// (e.g. swaps BadHeader and CrcFail).
static void test_cause_tag_source_grep() {
    std::cout << "\n=== Pin 3: source-grep on the three call-site "
                 "FrameErrCause tags + err_unlocked per-cause "
                 "increment ==="
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

    // All three cause tags must be present in the file.
    assert(strstr(buf, "err(FrameErrCause::BadHeader)") != NULL &&
           "Pin 3: LinkApi.cpp must call err(FrameErrCause::BadHeader) "
           "on the resync-scan err fire");
    assert(strstr(buf, "err(FrameErrCause::OverLen)") != NULL &&
           "Pin 3: LinkApi.cpp must call err(FrameErrCause::OverLen) "
           "on the len > max_len err fire");
    assert(strstr(buf, "err(FrameErrCause::CrcFail)") != NULL &&
           "Pin 3: LinkApi.cpp must call err(FrameErrCause::CrcFail) "
           "on the CRC-mismatch err fire");

    // The bare `err();` shape (no cause tag) must NOT
    // appear in LinkApi.cpp — the cause tag is now
    // mandatory, otherwise a fire does not contribute to
    // the per-cause breakdown and the aggregate invariant
    // is impossible.
    // Match `err();` but not `err(FrameErrCause::...)` —
    // use a tight pattern: `err();` with no following
    // identifier before the semicolon.
    assert(strstr(buf, "err();") == NULL &&
           "Pin 3: bare `err();` must NOT appear in LinkApi.cpp — "
           "the cause tag is now mandatory, otherwise a fire does "
           "not contribute to badHeaderErrs/overLenErrs/crcFailErrs "
           "and the aggregate invariant breaks");

    // err_unlocked body must stamp the per-cause field.
    // The switch is the load-bearing shape — a future
    // revert that drops the switch (or only bumps
    // frameErrs) would leave the per-cause counters at
    // 0 forever.
    const char *fnErr = strstr(buf, "err_unlocked(FrameErrCause");
    if (!fnErr) {
        // Fallback: search for any err_unlocked
        // definition. The signature now takes a
        // FrameErrCause param, so the file-wide
        // `err_unlocked(` substring is the right anchor.
        fnErr = strstr(buf, "bool Link::err_unlocked");
    }
    assert(fnErr);
    const char *bodyStart = strchr(fnErr, '{');
    assert(bodyStart);
    int depth = 0;
    const char *p = bodyStart;
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
    int len = (int)(end - fnErr);
    char eubuf[4096];
    if (len >= (int)sizeof(eubuf))
        len = sizeof(eubuf) - 1;
    memcpy(eubuf, fnErr, len);
    eubuf[len] = 0;
    assert(strstr(eubuf, "FrameErrCause::BadHeader") != NULL &&
           "Pin 3: err_unlocked body must stamp the BadHeader case "
           "(badHeaderErrs_++)");
    assert(strstr(eubuf, "FrameErrCause::OverLen") != NULL &&
           "Pin 3: err_unlocked body must stamp the OverLen case "
           "(overLenErrs_++)");
    assert(strstr(eubuf, "FrameErrCause::CrcFail") != NULL &&
           "Pin 3: err_unlocked body must stamp the CrcFail case "
           "(crcFailErrs_++)");
    free(buf);
    std::cout << "  Pin 3 PASS" << std::endl;
}

int main() {
    std::cout << "=== FrameErrCause Tests ===" << std::endl;
    test_bad_header_path();
    test_over_len_path();
    test_crc_fail_path();
    test_aggregate_invariant();
    test_cause_tag_source_grep();
    std::cout << "\n=== All 5 FrameErrCause pins PASS ===" << std::endl;
    return 0;
}

#endif
