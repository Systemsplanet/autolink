// SYNC mode: stop-and-wait send.
// Regression for v5.3.46.
//
// SYNC mode must:
//   1. Use zero ARQ pool slots (no cache insert).
//   2. Deliver the message to the receiver.
//   3. Run in finite time (no deadlock).
//   4. Recover after a forced wire drop.
//   5. Time out cleanly if the receiver never ACKs.
//
// The Arduino build's send() blocks in a
// wait loop while the FreeRTOS link task
// concurrently delivers the ACK. The host
// build has no concurrent link task, so
// the SYNC path is exercised through the
// split test_sendMsgBeginForTest /
// test_sendMsgStillWaitingForTest API:
// begin writes the frame and sets
// ackedPending_, then the test loop calls
// step() to drive time forward until the
// receiver ACKs.
#ifndef AUTOLINK_HOST_TEST
#    error "Build with -DAUTOLINK_HOST_TEST"
#endif

#include "WireSim.h"
#include "AutoLink.h"
#include "al/util/Log.h"

#include <iostream>
#include <cassert>
#include <cstring>

using namespace autolink;

namespace
{
bool waitLink(WireSim &sim, int budgetMs)
{
    for (int i = 0; i < budgetMs; i++) {
        sim.step(1);
        if (sim.getStateA() == State::OK && sim.getStateB() == State::OK)
            return true;
    }
    std::cout << " TIMEOUT stateA=" << (int)sim.getStateA()
              << " stateB=" << (int)sim.getStateB() << std::endl;
    return false;
}

// Send one SYNC-mode message by
// splitting the production send() into
// begin + step loop. Returns true if the
// ACK arrived within the timeout.
bool sendSyncDriven(WireSim &sim, AutoLink &node, const uint8_t *payload,
                    int len)
{
    if (!node.test_sendMsgBeginForTest(payload, len))
        return false;
    int budget = node.syncAckTimeoutMsForTest() + 50;
    for (int i = 0; i < budget; i++) {
        sim.step(1);
        if (!node.test_sendMsgStillWaitingForTest())
            return true;
    }
    return !node.test_sendMsgStillWaitingForTest();
}
} // namespace

void test_sync_default_mode_is_compile_time_correct()
{
    std::cout << "\n=== Test: default mode is compile-time correct ==="
              << std::endl;
    AutoLinkConfig cfg;
#ifdef AUTOLINK_HOST_TEST
    // Host build defaults to ASYNC so
    // tests don't hang in the SYNC wait
    // loop (no link task to deliver ACKs).
    assert(cfg.mode == AutoLinkConfig::Mode::ASYNC);
    std::cout << "  host build default = ASYNC"
              << " (test-build friendly)" << std::endl;
#else
    // Arduino build defaults to SYNC so
    // users get the boring, reliable
    // stop-and-wait path out of the box.
    assert(cfg.mode == AutoLinkConfig::Mode::SYNC);
    std::cout << "  Arduino build default = SYNC"
              << " (boring and reliable)" << std::endl;
#endif
    std::cout << "PASS" << std::endl;
}

void test_sync_mode_clean_wire_zero_pool_use()
{
    std::cout
        << "\n=== Test: SYNC mode on clean wire uses zero ARQ pool slots ==="
        << std::endl;
    Log::log().setLevel(Log::Level::WARNING);

    AutoLinkConfig cfg;
    cfg.errThreshold = 10000;
    WireSim sim(cfg);
    sim.setFrameDropPct(0);
    sim.setForcedDropEvery(0);
    // The test_sendMsgBegin / StillWaiting
    // hooks only work in SYNC mode. If
    // setMode(SYNC) is removed, the begin
    // hook returns false and the test fails
    // immediately. This pins the v5.3.46
    // contract: SYNC mode is reachable
    // through the runtime API.
    sim.nodeAForTest().setMode(AutoLinkConfig::Mode::SYNC);
    sim.nodeBForTest().setMode(AutoLinkConfig::Mode::SYNC);
    assert(sim.nodeAForTest().mode() == AutoLinkConfig::Mode::SYNC);
    assert(sim.nodeBForTest().mode() == AutoLinkConfig::Mode::SYNC);

    sim.linkA().begin();
    sim.linkB().begin();
    if (!waitLink(sim, 5000))
        assert(false);

    uint8_t payload[64];
    for (int i = 0; i < 64; i++)
        payload[i] = (uint8_t)(i ^ 0xA5);

    int delivered = 0;
    (void)delivered;
    for (int msg = 0; msg < 10; msg++) {
        if (!sendSyncDriven(sim, sim.nodeAForTest(), payload, 64)) {
            std::cerr << "\nFAIL: SYNC send #" << msg << " did not complete"
                      << std::endl;
            assert(false);
        }
        int poolUsed = sim.pendingCountA();
        if (poolUsed != 0) {
            std::cerr << "\nFAIL: SYNC mode inserted into ARQ pool "
                      << "(poolUsed=" << poolUsed << " for msg #" << msg
                      << ") — pool must stay empty in SYNC" << std::endl;
            assert(false);
        }
        sim.step(40);
    }
    sim.step(500);

    int received = 0;
    uint8_t buf[64];
    for (int i = 0; i < 200 && received < 10; i++) {
        int got = sim.nodeBForTest().recv(buf, sizeof buf);
        if (got > 0) {
            if (got != 64 || memcmp(buf, payload, 64) != 0) {
                std::cerr << "\nFAIL: corrupted message at idx " << received
                          << " (got " << got << " bytes)" << std::endl;
                assert(false);
            }
            received++;
        } else {
            sim.step(20);
        }
    }
    if (received != 10) {
        std::cerr << "\nFAIL: only received " << received
                  << "/10 messages in SYNC mode" << std::endl;
        assert(false);
    }
    if (sim.pendingCountA() != 0 || sim.pendingCountB() != 0) {
        std::cerr << "\nFAIL: ARQ pool not drained after "
                  << "successful SYNC exchange "
                  << "(A=" << sim.pendingCountA()
                  << ", B=" << sim.pendingCountB() << ")" << std::endl;
        assert(false);
    }
    std::cout << "  10 messages exchanged, "
              << "poolUsed=0 throughout" << std::endl;
    std::cout << "PASS" << std::endl;
}

void test_sync_mode_recovers_from_forced_drop()
{
    std::cout << "\n=== Test: SYNC mode survives a single forced drop ==="
              << std::endl;
    Log::log().setLevel(Log::Level::WARNING);

    AutoLinkConfig cfg;
    cfg.errThreshold = 10000;
    cfg.syncAckTimeoutMs = 800;
    WireSim sim(cfg);
    sim.setFrameDropPct(0);
    sim.setForcedDropEvery(0);
    sim.nodeAForTest().setMode(AutoLinkConfig::Mode::SYNC);
    sim.nodeBForTest().setMode(AutoLinkConfig::Mode::SYNC);
    sim.linkA().begin();
    sim.linkB().begin();
    if (!waitLink(sim, 5000))
        assert(false);

    uint8_t payload[32] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x12, 0x34, 0x56, 0x78 };
    // Send 3 messages successfully.
    for (int i = 0; i < 3; i++) {
        if (!sendSyncDriven(sim, sim.nodeAForTest(), payload, 32)) {
            std::cerr << "\nFAIL: pre-drop send #" << i << " failed"
                      << std::endl;
            assert(false);
        }
        sim.step(40);
    }
    // Drop the link once.
    sim.nodeAForTest().dropLink();
    sim.nodeBForTest().dropLink();
    sim.step(200);
    // Wait for re-sweep to OK.
    bool recovered = false;
    for (int i = 0; i < 5000; i++) {
        sim.step(10);
        if (sim.getStateA() == State::OK && sim.getStateB() == State::OK) {
            recovered = true;
            break;
        }
    }
    if (!recovered) {
        std::cerr << "\nFAIL: link didn't recover after single drop "
                  << "(A=" << (int)sim.getStateA()
                  << ", B=" << (int)sim.getStateB() << ")" << std::endl;
        assert(false);
    }
    // Now send again, must succeed.
    if (!sendSyncDriven(sim, sim.nodeAForTest(), payload, 32)) {
        std::cerr << "\nFAIL: post-drop send failed" << std::endl;
        assert(false);
    }
    sim.step(500);
    int received = 0;
    uint8_t buf[64];
    for (int i = 0; i < 200 && received < 1; i++) {
        int got = sim.nodeBForTest().recv(buf, sizeof buf);
        if (got > 0)
            received++;
        else
            sim.step(20);
    }
    if (received < 1) {
        std::cerr << "\nFAIL: post-drop message didn't arrive "
                  << "after recovery" << std::endl;
        assert(false);
    }
    std::cout << "  link dropped once, re-swept to OK, "
              << "1 message exchanged post-recovery" << std::endl;
    std::cout << "PASS" << std::endl;
}

void test_sync_mode_sender_timeout_returns_false()
{
    std::cout << "\n=== Test: SYNC mode returns false on link-not-OK ==="
              << std::endl;
    Log::log().setLevel(Log::Level::WARNING);

    AutoLinkConfig cfg;
    cfg.errThreshold = 10000;
    cfg.syncAckTimeoutMs = 200;
    WireSim sim(cfg);
    sim.setFrameDropPct(0);
    sim.setForcedDropEvery(0);
    sim.nodeAForTest().setMode(AutoLinkConfig::Mode::SYNC);
    sim.nodeBForTest().setMode(AutoLinkConfig::Mode::SYNC);
    sim.linkA().begin();
    sim.linkB().begin();
    if (!waitLink(sim, 5000))
        assert(false);

    sim.nodeAForTest().dropLink();
    for (int i = 0; i < 200 && sim.getStateA() == State::OK; i++)
        sim.step(20);

    uint8_t payload[16] = { 1, 2, 3, 4 };
    bool gotAck = sendSyncDriven(sim, sim.nodeAForTest(), payload, 16);
    if (gotAck) {
        std::cerr << "\nFAIL: SYNC send returned true after "
                  << "link drop (expected false)" << std::endl;
        assert(false);
    }
    std::cout << "  sendSyncDriven returned false when link not OK"
              << std::endl;
    std::cout << "PASS" << std::endl;
}

int main()
{
    Log::log().setLevel(Log::Level::WARNING);
    std::cout << "=== SYNC Mode Tests (v5.3.46 stop-and-wait) ===" << std::endl;
    test_sync_default_mode_is_compile_time_correct();
    test_sync_mode_clean_wire_zero_pool_use();
    test_sync_mode_recovers_from_forced_drop();
    test_sync_mode_sender_timeout_returns_false();
    std::cout << "\n=== SYNC Mode Tests Completed ===" << std::endl;
    return 0;
}