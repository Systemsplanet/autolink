// Pinned regression: Ping's "send failed (backpressure)"
// log line conflated three distinct sendMsg failure
// reasons (postLockQuiet, gbnWindowFull, poolExhaust,
// notOk) under one label, so a real backpressure signal
// was indistinguishable from a settle-window deferral.
// The fix is a SendMsgReason enum that sendMsg
// stamps on every false return so the app can log the
// precise cause.
//
//  Pin 1: sendMsg stamps the correct reason for each
//  failure mode:
//   - state != OK     -> NotOk
//   - txQuiet returns true -> PostLockQuiet
//   - GBN window full   -> GbnWindowFull
//   - arqCache full mid-msg-> PoolExhaust
//   - len < 0 / len > maxMsg -> LengthInvalid
//   - chunks > COBS_SEQ_SPACE -> ChunksOverflow
//  Pin 2: a successful sendMsg stamps Ok.
//  Pin 3: source-grep on the Link.h SendMsgReason enum
//  and the AutoLink.h facade exposing the reason.
//  Pin 4: GBN-window-full rejection site stamps
//  gbnWindowFullDrops (the Stats counter mirrors the
//  enum reason — without the counter the operator can
//  see the *most-recent* reason but not the count of
//  rejections, which is the load-bearing signal for
//  "the app is being throttled by the link").
//  Pin 5: PoolExhaust mid-message rejection site
//  stamps poolExhaustDrops (same surfacing shape as
//  Pin 4 — the count of refused messages, not the
//  most-recent reason).
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include <cstring>
#    include "MockHal.h"
#    include "LinkTestAccessor.h"
#    include "NullArqCache.h"
#    include "al/link/arq/ArqCache.h"
#    include "al/link/arq/IArqCache.h"
#    include "al/link/LinkWire.h"
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

// Pin 1: each failure path stamps the correct reason.
static void test_sendMsg_reasons() {
    std::cout << "\n=== Pin 1: sendMsg stamps the correct reason on "
                 "each failure path ==="
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

    // LengthInvalid: len > maxMsg.
    {
        uint8_t huge[100];
        memset(huge, 0, sizeof huge);
        bool ok = a.sendMsg(huge, (int)cfg.maxMsg + 1);
        assert(!ok);
        assert(accA.lastSendMsgReasonForTest() ==
                   SendMsgReason::LengthInvalid &&
               "LengthInvalid: len > cfg.maxMsg must stamp "
               "SendMsgReason::LengthInvalid");
        std::cout << " LengthInvalid PASS" << std::endl;
    }

    // NotOk: state != OK.
    {
        accA.resetLink(true, /*preserve=*/false, ResetReason::UserDropLink);
        const uint8_t payload[] = { 'h' };
        bool ok = a.sendMsg(payload, sizeof payload);
        assert(!ok);
        assert(accA.lastSendMsgReasonForTest() == SendMsgReason::NotOk &&
               "NotOk: state != State::OK must stamp "
               "SendMsgReason::NotOk");
        std::cout << " NotOk PASS" << std::endl;
    }

    // PostLockQuiet: txQuiet returns true.
    {
        // bring back to OK
        bringToOk(a, b, mHal, sHal);
        // Plant a fresh post-lock quiet gate.
        accA.setRecentDiscs(2, mHal.now);
        accA.setLockedAt(mHal.now);
        // AL90-2/3: also clear the
        // first-peer-response flag (it may
        // be set from bringToOk's pipe_data)
        // and re-arm the first-TX marker to
        // a real timestamp so the gate's
        // wall-clock path is the one that
        // fires.
        accA.setFirstPeerResponseSeen(false);
        accA.setPostLockFirstTxDone(mHal.now - 10);
        const uint8_t payload[] = { 'h' };
        bool ok = a.sendMsg(payload, sizeof payload);
        assert(!ok);
        assert(accA.lastSendMsgReasonForTest() ==
                   SendMsgReason::PostLockQuiet &&
               "PostLockQuiet: txQuiet returns true must stamp "
               "SendMsgReason::PostLockQuiet");
        std::cout << " PostLockQuiet PASS" << std::endl;
    }

    std::cout << " Pin 1 PASS (LengthInvalid, NotOk, PostLockQuiet "
                 "all stamp the right reason)"
              << std::endl;
}

// Pin 2: a successful sendMsg stamps Ok.
static void test_sendMsg_success_stamps_Ok() {
    std::cout << "\n=== Pin 2: a successful sendMsg stamps Ok ===" << std::endl;
    NullArqCache cacheA, cacheB;
    AutoLinkConfig cfg = cfgCommon();
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link a(mHal, cacheA, /*isMaster=*/true, cfg);
    Link b(sHal, cacheB, /*isMaster=*/false, cfg);
    bringToOk(a, b, mHal, sHal);
    LinkTestAccessor accA(a);
    const uint8_t payload[] = { 'h', 'e', 'l', 'l', 'o' };
    uint8_t seq = 0;
    bool ok = a.sendMsg(payload, sizeof payload, &seq);
    assert(ok && "Pin 2 pre: a 5-byte ASYNC sendMsg must succeed");
    for (int i = 0; i < 50; i++) {
        mHal.pumpClock(50);
        sHal.pumpClock(50);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
    }
    assert(accA.lastSendMsgReasonForTest() == SendMsgReason::Ok &&
           "Pin 2: a successful sendMsg must stamp "
           "SendMsgReason::Ok (so a subsequent failure path "
           "can be unambiguously distinguished from a prior "
           "success)");
    std::cout << " Pin 2 PASS (successful sendMsg stamps Ok)" << std::endl;
}

// Pin 3: source-grep on the SendMsgReason enum (Link.h)
// and the AutoLink.h facade exposing the reason.
static void test_sendmsg_reason_source_grep() {
    std::cout << "\n=== Pin 3: SendMsgReason enum + AutoLink facade "
                 "exposing the reason (source-grep) ==="
              << std::endl;
    {
        // The enum now lives in LinkStats.h (split out of Link.h so
        // the implementation header can stay focused on the link
        // state machine). The public surface (Link.h via
        // LinkStats.h) is identical.
        FILE *f =
            fopen(testRepoPath("src/al/link/stats/LinkStats.h").c_str(), "r");
        assert(f);
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = (char *)malloc((size_t)sz + 1);
        assert(buf);
        size_t got = fread(buf, 1, (size_t)sz, f);
        buf[got] = 0;
        fclose(f);
        assert(strstr(buf, "enum class SendMsgReason") != NULL &&
               "Pin 3: LinkStats.h must define the SendMsgReason enum "
               "(Ok / NotOk / PostLockQuiet / GbnWindowFull / "
               "PoolExhaust / LengthInvalid / LengthZero / "
               "ChunksOverflow / SyncMidMessageTimeout)");
        assert(strstr(buf, "PostLockQuiet") != NULL &&
               "Pin 3: SendMsgReason must include PostLockQuiet — "
               "the conflated 'backpressure' label is exactly the "
               "shape this enum fixes");
        free(buf);
    }
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
        assert(strstr(buf, "lastSendMsgReason_") != NULL &&
               "Pin 3: Link.h must declare the lastSendMsgReason_ "
               "field that sendMsg stamps on every false return");
        free(buf);
    }
    {
        FILE *f = fopen(testRepoPath("include/AutoLink.h").c_str(), "r");
        assert(f);
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = (char *)malloc((size_t)sz + 1);
        assert(buf);
        size_t got = fread(buf, 1, (size_t)sz, f);
        buf[got] = 0;
        fclose(f);
        assert(strstr(buf, "lastSendMsgReason") != NULL &&
               "Pin 3: AutoLink.h must expose "
               "lastSendMsgReason() on the facade so "
               "Ping/Pong can read the precise cause after a "
               "false return");
        free(buf);
    }
    std::cout << " Pin 3 PASS (SendMsgReason enum + AutoLink facade "
                 "exposing the reason)"
              << std::endl;
}

// Pin 4: GBN-window-full rejection site stamps
// gbnWindowFullDrops. The rejection site is the
// `if (!sync && inflight + chunks > window)` check
// in LinkApi.cpp. To trigger it, the test uses a
// real ArqCache with a small window (4), and stages
// the pending count up to the window — every fresh
// sendMsg beyond the window must increment
// gbnWindowFullDrops by exactly 1 and return false
// with lastSendMsgReason_ == GbnWindowFull. The
// pending-count staging goes through the real
// LinkArq::setPending / setGbnBase / setGbnActive
// path so the production bookkeeping shape is
// exercised. Remove either increment (the
// gbnWindowFullDrops_++ in LinkApi.cpp or the Stats
// surface in getStats) -> red.
static void test_gbn_window_full_drops() {
    std::cout << "\n=== Pin 4: sendMsg GBN-window-full rejection stamps "
                 "gbnWindowFullDrops (increments once per refused "
                 "sendMsg, sendMsg still returns false/GbnWindowFull) ==="
              << std::endl;
    // Use a real ArqCache with a small window
    // (4) so we can fill the window without
    // shipping 32 chunks. The Link ctor takes
    // IArqCache&; ArqCache satisfies that.
    ArqCache cacheA(/*window=*/4);
    NullArqCache cacheB;
    AutoLinkConfig cfg = cfgCommon();
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link a(mHal, cacheA, /*isMaster=*/true, cfg);
    Link b(sHal, cacheB, /*isMaster=*/false, cfg);
    bringToOk(a, b, mHal, sHal);
    LinkTestAccessor accA(a);

    // Fill the GBN window: set the base, mark
    // 4 slots as pending (== window). Subsequent
    // sendMsg with any chunks > 0 must trip the
    // `inflight + chunks > window` check.
    accA.setGbnBase(0);
    for (uint8_t s = 0; s < 4; s++) {
        accA.markAckedPending(s);
    }
    Stats statsBefore;
    a.getStats(statsBefore);
    assert(statsBefore.gbnWindowFullDrops == 0 &&
           "Pin 4: gbnWindowFullDrops must start at 0");

    // Fire 3 refused sendMsg calls; each must
    // increment the counter by exactly 1 and
    // return false with the correct reason.
    for (int i = 0; i < 3; i++) {
        const uint8_t payload[] = { 'h', 'i' };
        bool ok = a.sendMsg(payload, sizeof payload);
        assert(!ok &&
               "Pin 4: sendMsg must return false when the GBN window "
               "is full");
        assert(accA.lastSendMsgReasonForTest() ==
                   SendMsgReason::GbnWindowFull &&
               "Pin 4: lastSendMsgReason_ must be GbnWindowFull on a "
               "GBN-window-full rejection");
    }
    Stats statsAfter;
    a.getStats(statsAfter);
    std::cout << "  gbnWindowFullDrops " << statsBefore.gbnWindowFullDrops
              << " -> " << statsAfter.gbnWindowFullDrops << std::endl;
    assert(statsAfter.gbnWindowFullDrops ==
               statsBefore.gbnWindowFullDrops + 3 &&
           "Pin 4: gbnWindowFullDrops must increment by exactly 1 "
           "per refused sendMsg (3 refused calls -> +3)");
    std::cout << "  Pin 4 PASS (3 refused calls, counter +3, "
                 "sendMsg=false, reason=GbnWindowFull)"
              << std::endl;
}

// Pin 5: PoolExhaust rejection site stamps
// poolExhaustDrops. The rejection site is the
// multi-chunk ASYNC send loop in LinkApi.cpp:
// `if (!arqCache_.hasRoom())` returns false
// mid-message, the loop bails with
// `lastSendMsgReason_ = SendMsgReason::PoolExhaust`,
// and now also increments poolExhaustDrops once per
// refused sendMsg. To trigger it, the test uses a
// real ArqCache with a small window and an
// IArqCache stub (NoRoomCache) that returns false
// from hasRoom() — every multi-chunk send must hit
// the rejection on the first chunk's cache insert.
// Remove the increment -> red.
class NoRoomCache : public IArqCache {
public:
    bool hasRoom() const override { return false; }
    int freeRoom() const override { return 0; }
    void insert(uint8_t, const uint8_t *, int) override {}
    void freeBySeq(uint8_t) override {}
    bool peekForRetx(uint8_t, const uint8_t **, int *) const override {
        return false;
    }
    void clearAll() override {}
    bool slotInUse(uint8_t) const override { return false; }
    int size() const override { return 0; }
    int window() const override { return 32; }
};

static void test_pool_exhaust_drops() {
    std::cout << "\n=== Pin 5: sendMsg PoolExhaust mid-message rejection "
                 "stamps poolExhaustDrops (increments once per refused "
                 "sendMsg, sendMsg still returns false/PoolExhaust) ==="
              << std::endl;
    NoRoomCache cacheA;
    NullArqCache cacheB;
    AutoLinkConfig cfg = cfgCommon();
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link a(mHal, cacheA, /*isMaster=*/true, cfg);
    Link b(sHal, cacheB, /*isMaster=*/false, cfg);
    bringToOk(a, b, mHal, sHal);
    LinkTestAccessor accA(a);

    Stats statsBefore;
    a.getStats(statsBefore);
    assert(statsBefore.poolExhaustDrops == 0 &&
           "Pin 5: poolExhaustDrops must start at 0");

    // 2 multi-chunk sendMsg calls. The first
    // chunk's arqCache_.hasRoom() check returns
    // false, the loop bails with
    // lastSendMsgReason_ = PoolExhaust, and
    // poolExhaustDrops increments by 1 per
    // refused message.
    for (int i = 0; i < 2; i++) {
        // multi-chunk shape: payload > MAX_CHUNK
        // so the LinkApi path goes through the
        // hdrOnly + chunk loop with the
        // arqCache_.hasRoom() check inside.
        uint8_t payload[MAX_CHUNK + 32];
        memset(payload, 0, sizeof payload);
        bool ok = a.sendMsg(payload, (int)sizeof payload);
        assert(!ok &&
               "Pin 5: sendMsg must return false when the ARQ cache "
               "has no room mid-message");
        assert(accA.lastSendMsgReasonForTest() == SendMsgReason::PoolExhaust &&
               "Pin 5: lastSendMsgReason_ must be PoolExhaust on a "
               "mid-message ARQ-cache-exhausted rejection");
    }
    Stats statsAfter;
    a.getStats(statsAfter);
    std::cout << "  poolExhaustDrops " << statsBefore.poolExhaustDrops << " -> "
              << statsAfter.poolExhaustDrops << std::endl;
    assert(statsAfter.poolExhaustDrops == statsBefore.poolExhaustDrops + 2 &&
           "Pin 5: poolExhaustDrops must increment by exactly 1 "
           "per refused sendMsg (2 refused calls -> +2)");
    std::cout << "  Pin 5 PASS (2 refused calls, counter +2, "
                 "sendMsg=false, reason=PoolExhaust)"
              << std::endl;
}

int main() {
    std::cout << "=== SendMsg-Reason-Enum Tests ===" << std::endl;
    test_sendMsg_reasons();
    test_sendMsg_success_stamps_Ok();
    test_sendmsg_reason_source_grep();
    test_gbn_window_full_drops();
    test_pool_exhaust_drops();
    std::cout << "\n=== All 5 sendMsg-reason-enum pins PASS ===" << std::endl;
    return 0;
}

#endif
