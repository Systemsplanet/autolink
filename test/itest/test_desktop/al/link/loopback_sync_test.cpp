// SYNC-mode two-Link loopback end-to-end.
//
// Same pipe as loopback_test.cpp, but uses
// the test-only split API
// (LinkTestAccessor::sendMsgBegin /
// sendMsgStillWaiting) because the SYNC
// wait blocks the calling thread waiting
// for the link task to deliver the ACK,
// and the itest is single-threaded.
#include "al/link/Link.h"
#include "LinkTestAccessor.h"
#include "al/util/Log.h"
#include "al/util/UtilCrc.h"
#include "MockHal.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <atomic>
#include <chrono>
#include <thread>

using namespace autolink;
using clk = std::chrono::steady_clock;

static MockHal *g_pingHal = nullptr;
static MockHal *g_pongHal = nullptr;
static std::atomic<bool> g_pumpStop{ false };

static void pump_thread() {
    auto lastTick = clk::now();
    while (!g_pumpStop.load()) {
        auto now = clk::now();
        auto deltaMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - lastTick)
                           .count();
        if (deltaMs < 0)
            deltaMs = 0;
        if (deltaMs > 50)
            deltaMs = 50;
        lastTick = now;
        if (g_pingHal && g_pongHal) {
            g_pingHal->pumpClock((uint32_t)deltaMs);
            g_pongHal->pumpClock((uint32_t)deltaMs);
            if (!g_pingHal->txBuf.empty()) {
                std::vector<uint8_t> b = g_pingHal->txBuf;
                g_pingHal->clearTx();
                if (g_pongHal->link)
                    g_pongHal->link->onRx(b.data(), (int)b.size());
            }
            if (!g_pongHal->txBuf.empty()) {
                std::vector<uint8_t> b = g_pongHal->txBuf;
                g_pongHal->clearTx();
                if (g_pingHal->link)
                    g_pingHal->link->onRx(b.data(), (int)b.size());
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

static int run_loopback_sync(int seconds) {
    AutoLinkConfig cfg;
    cfg.maxMsg = 256;
    cfg.idleTimeoutMs = 5000;
    cfg.errThreshold = 20;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 115200;
    cfg.syncAckTimeoutMs = 200;

    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    g_pingHal = &mHal;
    g_pongHal = &sHal;

    Link ping(mHal, true, cfg);
    Link pong(sHal, false, cfg);
    ping.setMode(AutoLinkConfig::Mode::SYNC);
    pong.setMode(AutoLinkConfig::Mode::SYNC);

    Log::log().setLevel(Log::Level::WARNING);
    ping.begin();
    pong.begin();

    g_pumpStop.store(false);
    std::thread pump(pump_thread);

    auto t0 = clk::now();
    int sawOkMs = -1;
    int txCount = 0;
    int rxCount = 0;
    uint8_t payload[64];
    uint8_t buf[256];
    int lastSendMs = -1000;
    int sendEveryMs = 50;

    while (true) {
        auto wallMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                          clk::now() - t0)
                          .count();
        if (wallMs >= seconds * 1000)
            break;

        if (ping.getState() == State::OK && pong.getState() == State::OK &&
            sawOkMs < 0)
            sawOkMs = (int)wallMs;

        if (ping.getState() == State::OK && pong.getState() == State::OK &&
            wallMs - lastSendMs >= sendEveryMs) {
            for (int i = 0; i < 64; i++)
                payload[i] = (uint8_t)(wallMs + i);
            LinkTestAccessor pingT(ping);
            if (pingT.sendMsgBegin(payload, 64)) {
                int budget = cfg.syncAckTimeoutMs + 50;
                for (int i = 0; i < budget; i++) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    if (!pingT.sendMsgStillWaiting()) {
                        txCount++;
                        lastSendMs = (int)wallMs;
                        break;
                    }
                }
            }
        }

        if (pong.getState() == State::OK) {
            int n = pong.recvMsg(buf, sizeof buf);
            if (n > 0)
                rxCount++;
        }
    }

    g_pumpStop.store(true);
    pump.join();

    if (sawOkMs < 0) {
        std::cerr << "\nFAIL: link never reached OK" << std::endl;
        return 1;
    }
    if (rxCount < 1) {
        std::cerr << "\nFAIL: no messages received in SYNC mode "
                  << "(txCount=" << txCount << ", rxCount=" << rxCount
                  << ", stateA=" << (int)ping.getState()
                  << ", stateB=" << (int)pong.getState() << ")" << std::endl;
        return 1;
    }
    std::cout << "=== SYNC loopback PASS ===\n"
              << "  messages TX=" << txCount << " RX=" << rxCount << " in "
              << seconds << "s" << std::endl;
    return 0;
}

int main(int argc, char **argv) {
    int seconds = 5;
    for (int i = 1; i < argc; i++) {
        seconds = std::atoi(argv[i]);
        if (seconds < 1)
            seconds = 1;
        if (seconds > 60)
            seconds = 60;
    }
    return run_loopback_sync(seconds);
}