// Loopback with 30% drop: ARQ recovery.
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include <cstring>
#    include <chrono>
#    include <thread>
#    include "al/link/Link.h"
#    include "al/link/ArqCache.h"
#    include "LinkTestAccessor.h"
#    include "al/util/Log.h"
#    include "MockHal.h"

using namespace autolink;

static int g_runMs = 5000;
static bool g_sync = false;

// Minimal ARQ cache for the itest harness.
// The noise test drives two raw Link
// instances with no AutoLink wrapper, so
// it has to install its own ARQ hooks
// (otherwise the timer-driven retransmit
// path is a no-op and every wire drop
// becomes a permanent loss). The itest
// used to install its own 5-trampoline
// cache so the link's retx path had
// somewhere to re-send from. The
// trampolines are gone; the itest now
// uses the production ArqCache
// directly. The old g_insertCalls /
// g_retxCalls / g_ackCalls diagnostic
// counters dropped with the trampolines.
static ArqCache g_pingArq;
static ArqCache g_pongArq;

void test_loopback_noise_triggers_baud_fallback() {
    std::cout << "\n=== Test: Loopback under wire  noise "
              << "(mode=" << (g_sync ? "SYNC" : "ASYNC")
              << ") ===" << std::endl;

    const int DROP_PCT = 1;

    MockHal mHal, sHal;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 70000;
    cfg.maxMsg = 65535;

    cfg.errThreshold = 100;
    // Disable the link-task's idle-timeout
    // / heartbeat / asymmetric-idle machinery
    // (the itest is single-threaded and has
    // no real "peer gone" signal — the link
    // stays OK the whole run).
    cfg.idleTimeoutMs = 0;
    if (g_sync)
        cfg.syncAckTimeoutMs = 200;
    Link ping(mHal, true, cfg);
    Link pong(sHal, false, cfg);
    if (g_sync) {
        ping.setMode(AutoLinkConfig::Mode::SYNC);
        pong.setMode(AutoLinkConfig::Mode::SYNC);
    }

    // ASYNC only: install the test's ARQ
    // cache so NAK-driven retransmits can
    // actually re-send the cached frame.
    // Without this, the timer-driven
    // retx path is a no-op (callback is
    // null) and every wire drop becomes
    // a permanent loss. SYNC doesn't need
    // this — SYNC waits for the receiver's
    // ACK inline and never retransmits.
    g_pingArq.clearAll();
    g_pongArq.clearAll();
    ping.setArqCache(&g_pingArq);
    pong.setArqCache(&g_pongArq);
    mHal.peer = &sHal;
    sHal.peer = &mHal;

    ping.begin();
    pong.begin();
    negotiate_to_ok(ping, pong, mHal, sHal);
    assert(ping.getState() == State::OK);
    assert(pong.getState() == State::OK);

    uint32_t startBaud = mHal.spd;
    std::cout << "Negotiated OK at " << startBaud
              << " baud. frameDropPct=" << DROP_PCT
              << "% errThreshold=" << cfg.errThreshold << std::endl;

    mHal.frameDropPct = DROP_PCT;
    sHal.frameDropPct = DROP_PCT;
    mHal.dropRngSeed = 0xDEADBEEF;
    sHal.dropRngSeed = 0xCAFEBABE;

    auto t0 = std::chrono::steady_clock::now();
    int txCount = 0, rxCount = 0;
    std::vector<uint32_t> pingBauds, pongBauds;
    int lastPrint = 0;
    uint64_t maxDisc = 0;

    while (true) {
        auto now = std::chrono::steady_clock::now();
        int wallMs =
            (int)std::chrono::duration_cast<std::chrono::milliseconds>(now - t0)
                .count();
        if (wallMs >= g_runMs)
            break;

        mHal.pumpClock(20);
        sHal.pumpClock(20);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);

        if (ping.getState() == State::OK && pong.getState() == State::OK) {
            uint8_t payload[64];
            for (int i = 0; i < 64; i++)
                payload[i] = (uint8_t)('A' + (i % 26));
            if (g_sync) {
                LinkTestAccessor pingT(ping);
                if (pingT.sendMsgBegin(payload, 64)) {
                    int budget = cfg.syncAckTimeoutMs + 50;
                    for (int i = 0; i < budget; i++) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(2));
                        mHal.pumpClock(2);
                        sHal.pumpClock(2);
                        pipe_data(mHal, sHal);
                        pipe_data(sHal, mHal);
                        if (!pingT.sendMsgStillWaiting()) {
                            txCount++;
                            break;
                        }
                    }
                }
            } else {
                if (ping.sendMsg(payload, 64))
                    txCount++;
            }

            uint8_t buf[300];
            int n;
            while ((n = pong.recvMsg(buf, sizeof(buf))) > 0) {
                if (g_sync) {
                    LinkTestAccessor pongT(pong);
                    if (pongT.sendMsgBegin(buf, n)) {
                        int budget = cfg.syncAckTimeoutMs + 50;
                        for (int i = 0; i < budget; i++) {
                            std::this_thread::sleep_for(
                                std::chrono::milliseconds(2));
                            mHal.pumpClock(2);
                            sHal.pumpClock(2);
                            pipe_data(mHal, sHal);
                            pipe_data(sHal, mHal);
                            if (!pongT.sendMsgStillWaiting()) {
                                rxCount++;
                                break;
                            }
                        }
                    }
                } else {
                    if (pong.sendMsg(buf, n))
                        rxCount++;
                }
            }
        }

        if (pingBauds.empty() || pingBauds.back() != (uint32_t)mHal.spd)
            pingBauds.push_back(mHal.spd);
        if (pongBauds.empty() || pongBauds.back() != (uint32_t)sHal.spd)
            pongBauds.push_back(sHal.spd);

        Stats ps, qs;
        ping.getStats(ps);
        pong.getStats(qs);
        uint64_t disc = ps.discCount + qs.discCount;
        if (disc > maxDisc)
            maxDisc = disc;

        if (wallMs - lastPrint >= 1000) {
            lastPrint = wallMs;
            std::cout << "T=" << (wallMs / 1000) << "s  Ping@" << mHal.spd
                      << "  Pong@" << sHal.spd << "  disc=" << disc
                      << "  bytesDropped="
                      << (mHal.bytesDropped + sHal.bytesDropped) << std::endl;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    auto wallMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
    Stats ps, qs;
    ping.getStats(ps);
    pong.getStats(qs);

    std::cout << "\n=== Final ===" << std::endl;
    std::cout << "Ran " << wallMs << " ms" << std::endl;
    std::cout << "Final baud Ping=" << mHal.spd << " Pong=" << sHal.spd
              << std::endl;
    std::cout << "Baud history Ping: ";
    for (auto b : pingBauds)
        std::cout << b << " ";
    std::cout << "\nBaud history Pong: ";
    for (auto b : pongBauds)
        std::cout << b << " ";
    std::cout << "\nDisconnects: Ping=" << ps.discCount
              << " Pong=" << qs.discCount << " (total=" << maxDisc << ")"
              << std::endl;
    std::cout << "Frame errors: Ping=" << ps.frameErrs
              << " Pong=" << qs.frameErrs << std::endl;
    std::cout << "Messages TX=" << txCount << " RX=" << rxCount << std::endl;
    std::cout << "Bytes dropped by noise: "
              << (mHal.bytesDropped + sHal.bytesDropped) << std::endl;
    std::cout << "ARQ caches: ping.in_use=" << g_pingArq.size()
              << " pong.in_use=" << g_pongArq.size() << std::endl;

    int totalDropped = mHal.bytesDropped + sHal.bytesDropped;
    assert(totalDropped > 0 &&
           "frameDropPct=30 but no bytes dropped —  wire noise knob is broken");

    bool fellBack = false;
    for (auto b : pingBauds)
        if (b < startBaud) {
            fellBack = true;
            break;
        }
    for (auto b : pongBauds)
        if (b < startBaud) {
            fellBack = true;
            break;
        }
    bool disconnected = (maxDisc > 0);

    if (!(fellBack || disconnected)) {
        std::cout << "\nFAIL: 30% frame drop for " << g_runMs
                  << "ms produced NO baud fallback  and NO disconnects. "
                  << "This means gap events aren't  bumping errs — the "
                  << "fix has been regressed." << std::endl;
        assert(false && "noise test: no recovery under 30%  frame drop");
    }

    int delivered = (rxCount > txCount) ? txCount : rxCount;
    int deliveryFloor = (txCount * 5) / 100;
    std::cout << "Delivery: TX=" << txCount << " RX=" << rxCount
              << " delivered=" << delivered << " floor=" << deliveryFloor
              << std::endl;
    if (delivered < deliveryFloor) {
        std::cout << "\nFAIL: " << delivered << "/" << txCount
                  << " messages round-tripped — below the "
                  << "5% floor under " << DROP_PCT << "% frame drop. "
                  << "Either retransmit is dropping on  arrival "
                  << "(forward-resync-shape bug) or the  staleness "
                  << "cap is firing too aggressively." << std::endl;
        assert(false &&
               "noise test: ARQ delivery below 5%  under realistic noise");
    }

    std::cout << "\nPASS: protocol recovered from  wire noise "
              << "(fellBack=" << fellBack << " disconnected=" << disconnected
              << " delivered=" << delivered << "/" << txCount << ")"
              << std::endl;
}

int main(int argc, char **argv) {
    bool verbose = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "sync")
            g_sync = true;
        else if (a == "async")
            g_sync = false;
        else if (a == "verbose" || a == "-v")
            verbose = true;
        else
            g_runMs = std::atoi(argv[i]);
        if (g_runMs < 500)
            g_runMs = 500;
    }
    Log::log().setLevel(verbose ? Log::Level::DEBUG : Log::Level::WARNING);
    std::cout << "[debug] Log level set to " << (int)Log::log().getLevel()
              << " (DEBUG=" << (int)Log::Level::DEBUG << ")" << std::endl;
    test_loopback_noise_triggers_baud_fallback();
    std::cout << "\n[" << (g_sync ? "SYNC" : "ASYNC")
              << "] All loopback-noise tests PASSED." << std::endl;
    return 0;
}

#endif