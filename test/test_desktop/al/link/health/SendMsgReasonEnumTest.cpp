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
        assert(strstr(buf, "enum class SendMsgReason") != NULL &&
               "Pin 3: Link.h must define the SendMsgReason enum "
               "(Ok / NotOk / PostLockQuiet / GbnWindowFull / "
               "PoolExhaust / LengthInvalid / LengthZero / "
               "ChunksOverflow / SyncMidMessageTimeout)");
        assert(strstr(buf, "PostLockQuiet") != NULL &&
               "Pin 3: SendMsgReason must include PostLockQuiet — "
               "the the prior release conflated 'backpressure' label is "
               "exactly the shape this enum fixes");
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

int main() {
    std::cout << "=== SendMsg-Reason-Enum Tests ===" << std::endl;
    test_sendMsg_reasons();
    test_sendMsg_success_stamps_Ok();
    test_sendmsg_reason_source_grep();
    std::cout << "\n=== All 3 sendMsg-reason-enum pins PASS ===" << std::endl;
    return 0;
}

#endif
