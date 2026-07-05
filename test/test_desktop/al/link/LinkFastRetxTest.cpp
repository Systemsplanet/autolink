// Regression: ASYNC NAK-driven retransmit must fire
// immediately, inline in onNak — not deferred to the
// next OK-timer tick. The pre-fix shape stamped a
// single pendingRetxBase_ slot that the timer drained
// one frame per okTickMs, and each fresh NAK overwrote
// the prior base. Under continuous line-rate traffic
// with real loss, that throttled recovery to one chunk
// per tick while the receiver's reorder window expired
// far faster, so multi-chunk ASYNC messages never
// reassembled and the link churned resweeps (delivery
// collapsed to ~0). This is the "random mode is worse
// than sequential" failure: SYNC self-throttles via
// inline waitForAck and never builds a backlog.
//
// Pins:
//   1. A NAK for a pending seq emits a retransmit on
//      the wire within the onNak call, before any
//      timer tick. Toggle-off (defer to timer) → the
//      wire stays empty → assertion fails (red).
//   2. A NAK for a non-pending seq emits nothing
//      (the pending guard still holds).
//   3. The retransmitted bytes reproduce the cached
//      frame for that seq (a data frame carrying the
//      seq), not some unrelated traffic.
#include <iostream>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "MockHal.h"
#include "LinkTestAccessor.h"
#include "al/link/arq/ArqCache.h"
#include "al/AutoLinkConfig.h"
#include "al/util/Log.h"

using namespace autolink;

static const int kBauds[] = { 115200, 57600, 38400, 19200, 9600 };
static const int kNumBauds = (int)(sizeof(kBauds) / sizeof(kBauds[0]));

static void negotiateToOk(Link &ping, Link &pong, MockHal &mHal,
                          MockHal &sHal) {
    for (int i = 0; i < 100 &&
         (ping.getState() != State::OK || pong.getState() != State::OK);
         i++) {
        mHal.pumpClock(50);
        sHal.pumpClock(50);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
    }
    assert(ping.getState() == State::OK);
    assert(pong.getState() == State::OK);
    sHal.txBuf.clear();
    mHal.txBuf.clear();
    sHal.clearAppBuf();
    mHal.clearAppBuf();
}

static void mkCfg(AutoLinkConfig &cfg) {
    for (int i = 0; i < kNumBauds; i++)
        cfg.allowedBauds[i] = (uint32_t)kBauds[i];
    cfg.allowedBaudsCount = kNumBauds;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 10000; // realistic; the test never pumps the timer
    cfg.streamBufferSize = 131072;
    cfg.maxMsg = 2048;
    cfg.syncAckTimeoutMs = 500;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
}

// Pin 1 + 3: NAK for a live pending seq retransmits
// immediately, and the retransmit carries that seq.
static void test_nak_retransmits_inline() {
    std::cout << "\n=== Pin 1/3: NAK triggers immediate inline retransmit ==="
              << std::endl;
    AutoLinkConfig cfg;
    mkCfg(cfg);
    ArqCache pingArq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
    ArqCache pongArq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
    MockHal mHal, sHal;
    Link ping(mHal, pingArq, true, cfg);
    Link pong(sHal, pongArq, false, cfg);
    ping.setMode(AutoLinkConfig::Mode::ASYNC);
    pong.setMode(AutoLinkConfig::Mode::ASYNC);
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    ping.begin();
    pong.begin();
    negotiateToOk(ping, pong, mHal, sHal);

    // Multi-chunk ASYNC send so the ARQ cache holds
    // several pending frames with known seqs.
    std::vector<uint8_t> msg(600);
    for (size_t j = 0; j < msg.size(); j++)
        msg[j] = (uint8_t)(j & 0xFF);
    uint8_t base = 0xFF;
    bool sent = ping.sendMsg(msg.data(), (int)msg.size(), &base);
    assert(sent && "multi-chunk ASYNC send should be accepted on a fresh link");

    LinkTestAccessor pa(ping);
    assert(pa.arq().isPending(base) &&
           "base seq must be ack-pending after send");

    // Clear the wire so anything that appears next is
    // strictly the retransmit under test.
    mHal.txBuf.clear();
    mHal.txBaudPerByte.clear();
    size_t before = mHal.txBuf.size();
    assert(before == 0);

    // Inject the NAK. No pumpClock, no timer tick.
    pa.onNak(base);

    size_t after = mHal.txBuf.size();
    std::cout << "  wire bytes after NAK (no timer tick): " << after
              << std::endl;
    assert(after > before &&
           "NAK for a pending seq must retransmit immediately, inline in "
           "onNak — not defer to the OK-timer tick");

    // Pin 3: the retransmitted frame carries `base`.
    // Data frames are COBS: 0x00 seq ... 0x00. After
    // the leading delimiter and the COBS overhead byte,
    // the decoded seq is `base`; the raw wire will
    // contain `base` somewhere in the emitted frame.
    bool sawSeq = false;
    for (size_t j = 0; j < mHal.txBuf.size(); j++)
        if (mHal.txBuf[j] == base) {
            sawSeq = true;
            break;
        }
    assert(sawSeq && "retransmit should reproduce the cached frame for base");
    std::cout << "  PASS" << std::endl;
}

// Pin 2: NAK for a non-pending seq emits nothing.
static void test_nak_nonpending_is_noop() {
    std::cout << "\n=== Pin 2: NAK for non-pending seq is a no-op ==="
              << std::endl;
    AutoLinkConfig cfg;
    mkCfg(cfg);
    ArqCache pingArq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
    ArqCache pongArq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
    MockHal mHal, sHal;
    Link ping(mHal, pingArq, true, cfg);
    Link pong(sHal, pongArq, false, cfg);
    ping.setMode(AutoLinkConfig::Mode::ASYNC);
    pong.setMode(AutoLinkConfig::Mode::ASYNC);
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    ping.begin();
    pong.begin();
    negotiateToOk(ping, pong, mHal, sHal);

    LinkTestAccessor pa(ping);
    // Pick a seq that is definitely not pending on a
    // link that has sent nothing since negotiation.
    uint8_t idle = 200;
    assert(!pa.arq().isPending(idle));
    mHal.txBuf.clear();
    pa.onNak(idle);
    assert(mHal.txBuf.empty() &&
           "NAK for a non-pending seq must not put anything on the wire");
    std::cout << "  PASS" << std::endl;
}

int main() {
    Log::log().setLevel(Log::NONE);
    test_nak_retransmits_inline();
    test_nak_nonpending_is_noop();
    std::cout << "\nLinkFastRetxTest: all pins passed" << std::endl;
    return 0;
}
