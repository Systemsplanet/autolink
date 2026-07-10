// Regression: todo item 1 (07/07 bench log) — random-content payloads
// triggered cascading SYNC resyncs on the wire. The bench showed
// ~3 disconnects in 35 s immediately after switching Ping's fill
// mode from sequential to random, each a full 5-attempt SYNC
// retx-ladder exhaustion. The prior ~2.5 min on sequential fill ran
// clean.
//
// Root cause was wire framing: the OK-state CTRL-scan claimed any
// `0xAA 0x55 X Y` 5-byte run as a CTRL frame candidate; random
// payload content collided with the sentinel at ~1/65536 per byte
// (every multi-chunk Ping payload hits 1% chance per chunk) and a
// CRC8 fail on a non-CTRL collision called err_unlocked() ->
// frameErrs++ -> errThreshold drop. The old design was safe only
// for low-entropy sequential fill.
//
// This itest mirrors Ping's actual fill behaviour:
//   - size: uniform [1, maxMsg] (Ping::pickMsgSize_ RANDOM)
//   - content: random bytes 0-255 (Ping::fillRandom_)
//
// Both ends at 115200 baud, SYNC mode, no noise injection. Default
// run length is 10 s (CLI override). The pin measures
// STEADY-STATE disconnects only — the bring-up sweep cycle (PING
// / LOCK-CMD / sendBreak) legitimately bumps discCount once on
// pong as the slave first locks. Toggle-off (revert the
// `UtilCrc::crc8` pre-check in LinkRx.cpp's OK-state scan) -> the
// frameErrs baseline reaches errThreshold within seconds and the
// link drops.
//
// Wire format unchanged. The pre-check only ever rejects bytes
// that would have failed `processCtrlFrame_unlocked`'s CRC step
// anyway, so legitimate CTRL frames still process normally.
#include "al/link/Link.h"
#include "LinkTestAccessor.h"
#include "al/util/Log.h"
#include "al/util/UtilCobs.h"
#include "MockHal.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>
#include <random>
#include "NullArqCache.h"

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
            {
                std::vector<uint8_t> b = g_pingHal->drainTx();
                if (!b.empty() && g_pongHal->events())
                    g_pongHal->events()->onRx(b.data(), (int)b.size());
            }
            {
                std::vector<uint8_t> b = g_pongHal->drainTx();
                if (!b.empty() && g_pingHal->events())
                    g_pingHal->events()->onRx(b.data(), (int)b.size());
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

static int run_loopback_random_fill(int seconds) {
    NullArqCache cache;
    AutoLinkConfig cfg;
    // Pin a deterministic config (matches what the bench ran with
    // before the multi-mode toggle). maxMsg stays at the
    // AutoLinkConfig default; streamBufferSize uses the host-test
    // MockHal (unbounded app buf) so this test isolates the link
    // layer from app-side buffering concerns.
    cfg.maxMsg = 5120;
    cfg.idleTimeoutMs = 10000;
    cfg.errThreshold = 20;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 115200;
    cfg.syncAckTimeoutMs = 200;

    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    g_pingHal = &mHal;
    g_pongHal = &sHal;

    Link ping(mHal, cache, true, cfg);
    Link pong(sHal, cache, false, cfg);
    ping.setMode(AutoLinkConfig::Mode::SYNC);
    pong.setMode(AutoLinkConfig::Mode::SYNC);

    Log::log().setLevel(Log::Level::WARNING);
    ping.begin();
    pong.begin();

    g_pumpStop.store(false);
    std::thread pump(pump_thread);

    // Wait for bring-up to OK before starting the steady-state
    // traffic window. Sweep-phase resyncs (PING/LOCK-CMD/sendBreak
    // during the lock handshake) are intentional and legitimately
    // bump pong's discCount once.
    auto bringupT0 = clk::now();
    while (ping.getState() != State::OK || pong.getState() != State::OK) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           clk::now() - bringupT0)
                           .count();
        if (elapsed > 2000) {
            std::cerr << "FAIL: link never reached OK in 2 s" << std::endl;
            g_pumpStop.store(true);
            pump.join();
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // STEADY-STATE baseline (post bring-up).
    Stats baseA, baseB;
    ping.getStats(baseA);
    pong.getStats(baseB);

    auto t0 = clk::now();
    int txCount = 0;
    int rxCount = 0;
    int stuckAcks = 0;
    int maxAckMs = 0;
    std::vector<uint8_t> payload;
    payload.resize(cfg.maxMsg);
    std::vector<uint8_t> buf(cfg.maxMsg + 64);

    // Mirror Ping::pickMsgSize_: uniform [1, maxMsg].
    std::mt19937 rng(0xC0FFEE42u);
    auto pickSize = [&]() -> int {
        std::uniform_int_distribution<int> d(1, (int)cfg.maxMsg);
        return d(rng);
    };

    int lastSendMs = 0;
    int sendEveryMs = 20; // ~50 msgs/s — below the SYNC retx budget
                          // so each begin lands a fresh frame.

    while (true) {
        auto now = clk::now();
        auto wallMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - t0)
                .count();
        if (wallMs >= seconds * 1000)
            break;

        bool linkOk =
            ping.getState() == State::OK && pong.getState() == State::OK;
        if (linkOk && wallMs - lastSendMs >= sendEveryMs) {
            int n = pickSize();
            // Mirror Ping::fillRandom_: random bytes 0-255.
            for (int i = 0; i < n; i++)
                payload[i] = (uint8_t)(rng() & 0xFF);

            LinkTestAccessor pingT(ping);
            if (pingT.sendMsgBegin(payload.data(), n)) {
                int budget = cfg.syncAckTimeoutMs * cfg.maxRetx + 200;
                auto sendStart = clk::now();
                bool gotAck = false;
                for (int i = 0; i < budget && !g_pumpStop.load(); i++) {
                    std::this_thread::sleep_for(std::chrono::microseconds(500));
                    if (!pingT.sendMsgStillWaiting()) {
                        auto ackMs = std::chrono::duration_cast<
                                         std::chrono::milliseconds>(clk::now() -
                                                                    sendStart)
                                         .count();
                        if ((int)ackMs > maxAckMs)
                            maxAckMs = (int)ackMs;
                        gotAck = true;
                        txCount++;
                        lastSendMs = (int)wallMs;
                        break;
                    }
                }
                if (!gotAck) {
                    stuckAcks++;
                    lastSendMs = (int)wallMs;
                }
            } else {
                lastSendMs = (int)wallMs;
            }
        }

        if (pong.getState() == State::OK) {
            int n = pong.recvMsg(buf.data(), (int)buf.size());
            if (n > 0)
                rxCount++;
        }
    }

    g_pumpStop.store(true);
    pump.join();

    Stats ps, qs;
    ping.getStats(ps);
    pong.getStats(qs);

    // Bring-up consumes the sweep-phase disc reset (see comment
    // above); measure only the steady-state deltas.
    int steadyDropsA = (int)(ps.discCount - baseA.discCount);
    int steadyDropsB = (int)(qs.discCount - baseB.discCount);
    int steadyErrsA = (int)(ps.frameErrs - baseA.frameErrs);
    int steadyErrsB = (int)(qs.frameErrs - baseB.frameErrs);

    std::cout << "=== Random-fill SYNC itest (" << seconds
              << " s) ===" << std::endl;
    std::cout << "  msgs TX=" << txCount << " RX=" << rxCount
              << " stuckAcks=" << stuckAcks << " maxAckMs=" << maxAckMs
              << " steadyDrops A=" << steadyDropsA << " B=" << steadyDropsB
              << " frameErrs A=" << steadyErrsA << " B=" << steadyErrsB
              << std::endl;

    if (rxCount < 1) {
        std::cerr << "FAIL: no RX" << std::endl;
        return 1;
    }
    if (steadyDropsA + steadyDropsB > 0) {
        std::cerr << "FAIL: " << (steadyDropsA + steadyDropsB)
                  << " steady-state disconnects during random fill"
                  << " (the bench wedge)" << std::endl;
        return 1;
    }
    if (steadyErrsA > 0 || steadyErrsB > 0) {
        std::cerr << "FAIL: framing errors during steady-state random fill"
                  << " (fix the 0xAA 0x55 CTRL pre-check in LinkRx.cpp)"
                  << std::endl;
        return 1;
    }
    std::cout << "  PASS (random fill: zero steady-state disconnects, zero"
              << " framing errors, " << rxCount << "/" << txCount
              << " delivered)" << std::endl;
    return 0;
}

int main(int argc, char **argv) {
    int seconds = 10;
    for (int i = 1; i < argc; i++) {
        seconds = std::atoi(argv[i]);
        if (seconds < 1)
            seconds = 1;
        if (seconds > 60)
            seconds = 60;
    }
    return run_loopback_random_fill(seconds);
}
