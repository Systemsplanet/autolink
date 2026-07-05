// Regression: the OK-state timer sweep (LinkTimers.cpp)
// owns two drop paths that no host suite exercised —
// gcov showed both as dead lines even though they fire
// on a live wire.
//
// Pins:
//   1. ASYNC ARQ maxRetx exhaustion. A pending seq that
//      the peer never ACKs is retransmitted once per RTO
//      until retxCount reaches maxRetx, then the sweep
//      drops the link and resweeps (reset + BREAK). The
//      wire shows retransmits first (the Retx arm), then
//      the state flips OK -> SWP with discCount bumped
//      (the Drop arm). Toggle-off (Drop arm removed) ->
//      the link stalls in OK forever -> red.
//   2. Reorder-slot expiry on the OK tick. A held out-of-
//      order slot older than reorderHoldMs is dropped by
//      the timer's dropExpired backstop (not just the
//      onPayload path): lostMsgs bumps and the slot frees.
//      Toggle-off (timer dropExpired block removed) ->
//      lostMsgs stays flat and the slot leaks -> red.
#include <iostream>
#include <cassert>
#include <cstdint>
#include "MockHal.h"
#include "LinkTestAccessor.h"
#include "al/link/arq/ArqCache.h"
#include "al/AutoLinkConfig.h"
#include "al/util/Log.h"

using namespace autolink;

static const int kBauds[] = { 115200, 57600, 38400, 19200, 9600 };
static const int kNumBauds = (int)(sizeof(kBauds) / sizeof(kBauds[0]));

static void mkCfg(AutoLinkConfig &cfg) {
    for (int i = 0; i < kNumBauds; i++)
        cfg.allowedBauds[i] = (uint32_t)kBauds[i];
    cfg.allowedBaudsCount = kNumBauds;
    cfg.pingSamplesPerBaud = 1;
    cfg.streamBufferSize = 131072;
    cfg.maxMsg = 2048;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
}

// Pin 1: maxRetx exhaustion on the OK sweep drops + resweeps.
static void test_maxretx_drop_resweeps() {
    std::cout << "\n=== Pin 1: ASYNC maxRetx exhaustion drops + resweeps ==="
              << std::endl;
    AutoLinkConfig cfg;
    mkCfg(cfg);
    // Short RTO, small retx budget, idle watchdog well
    // clear of the few ms this test spans so the drop
    // is unambiguously the maxRetx arm.
    cfg.syncAckTimeoutMs = 20;
    cfg.maxRetx = 3;
    cfg.idleTimeoutMs = 10000;
    ArqCache pingArq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
    ArqCache pongArq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
    MockHal mHal, sHal;
    Link ping(mHal, pingArq, true, cfg);
    Link pong(sHal, pongArq, false, cfg);
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    negotiate_to_ok(ping, pong, mHal, sHal);

    LinkTestAccessor pa(ping);
    uint8_t base = 0xFF;
    bool sent = ping.sendMsg((const uint8_t *)"hi", 2, &base);
    assert(sent && "fresh ASYNC send should be accepted");
    assert(pa.arq().isPending(base) && "seq must be ack-pending after send");

    Stats before;
    ping.getStats(before);

    // The peer never ACKs. Drive the ping's OK tick by
    // hand, advancing past the RTO each pass so decideSlot
    // returns Retx up to maxRetx, then Drop. No pumpClock:
    // manual ticks keep the RTO cadence exact and isolate
    // the sweep from the sHal side.
    bool sawRetx = false;
    int ticks = 0;
    for (; ticks <= (int)cfg.maxRetx + 2; ticks++) {
        mHal.now += (uint32_t)cfg.syncAckTimeoutMs + 1;
        mHal.clearTx();
        ping.onTimer();
        if (ping.getState() != State::OK)
            break;
        if (!mHal.txBuf.empty())
            sawRetx = true; // the Retx arm put a frame on the wire
    }

    assert(sawRetx &&
           "a pending seq must be retransmitted before it is dropped");
    assert(ping.getState() == State::SWP &&
           "maxRetx exhaustion must drop the link back into a resweep");
    Stats after;
    ping.getStats(after);
    assert(after.discCount == before.discCount + 1 &&
           "the resweep must count as one disconnect");
    assert(!pa.arq().isPending(base) &&
           "the dropped seq must be cleared by the reset");
    std::cout << "  retx ticks=" << ticks
              << " state=SWP discCount+1 pending cleared\n  PASS" << std::endl;
}

// Pin 2: an expired reorder slot is dropped by the OK tick.
static void test_reorder_expiry_dropped_on_tick() {
    std::cout << "\n=== Pin 2: OK tick drops an expired reorder slot ==="
              << std::endl;
    AutoLinkConfig cfg;
    mkCfg(cfg);
    cfg.reorderHoldMs = 1500;
    cfg.idleTimeoutMs = 10000;
    ArqCache pingArq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
    ArqCache pongArq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
    MockHal mHal, sHal;
    Link ping(mHal, pingArq, true, cfg);
    Link pong(sHal, pongArq, false, cfg);
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    negotiate_to_ok(ping, pong, mHal, sHal);

    LinkTestAccessor pa(ping);
    const uint8_t seq = 42;
    const uint8_t payload[] = { 1, 2, 3, 4 };
    bool held = pa.reorderHold(seq, payload, (int)sizeof(payload), mHal.now);
    assert(held && "reorder hold should reserve a slot");
    assert(pa.reorderSlotInUse(seq) && "slot must be in use after hold");

    Diag before;
    ping.getDiag(before);

    // Age the slot past reorderHoldMs, then fire a single
    // OK tick. No RX is fed, so the only path that can
    // reap the slot is the timer's dropExpired backstop.
    mHal.now += (uint32_t)cfg.reorderHoldMs + 1;
    ping.onTimer();

    assert(ping.getState() == State::OK &&
           "the reorder-expiry tick must not drop the link");
    Diag after;
    ping.getDiag(after);
    assert(after.lostMsgs == before.lostMsgs + 1 &&
           "an expired reorder slot must count as one lost message");
    assert(!pa.reorderSlotInUse(seq) &&
           "the expired slot must be freed by the tick");
    std::cout << "  lostMsgs " << before.lostMsgs << " -> " << after.lostMsgs
              << " slot freed\n  PASS" << std::endl;
}

int main() {
    Log::log().setLevel(Log::NONE);
    test_maxretx_drop_resweeps();
    test_reorder_expiry_dropped_on_tick();
    std::cout << "\nLinkTimersTest: all pins passed" << std::endl;
    return 0;
}
