// Two-Link host loopback: end-to-end smoke for
// protocol changes pre-flash.


#include "al/link/Link.h"
#include "MockHal.h"
#include "al/util/Log.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>

using namespace autolink;
using clk = std::chrono::steady_clock;

static MockHal *g_pingHal = nullptr;
static MockHal *g_pongHal = nullptr;

static void pipe_now()
{
    if (g_pingHal && !g_pingHal->txBuf.empty()) {
        std::vector<uint8_t> b = g_pingHal->txBuf;
        g_pingHal->clearTx();
        g_pongHal->link->onRx(b.data(), (int)b.size());
    }
    if (g_pongHal && !g_pongHal->txBuf.empty()) {
        std::vector<uint8_t> b = g_pongHal->txBuf;
        g_pongHal->clearTx();
        g_pingHal->link->onRx(b.data(), (int)b.size());
    }
}

static int run_loopback(int seconds, bool verbose,
                        int mode, bool reliable)
{
    AutoLinkConfig cfg;
    cfg.maxMsg = 256;
    cfg.idleTimeoutMs = 5000;
    cfg.errThreshold = 20;
    cfg.allowedBaudsCount = 1;
    cfg.allowedBauds[0] = 115200;

    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    g_pingHal = &mHal;
    g_pongHal = &sHal;

    Link ping(mHal, true, cfg);
    Link pong(sHal, false, cfg);

    if (verbose) {
        Log::log().setLevel(Log::DEBUG);
    } else {
        Log::log().setLevel(Log::WARNING);
    }

    ping.begin();
    pong.begin();


    auto t0 = clk::now();
    auto lastTick = t0;
    auto lastPrint = t0;
    int txCount = 0;
    int rxCount = 0;
    int arqRetx = 0;
    int frameErrs = 0;
    bool sawOk = false;
    int okHoldMs = 0;
    int okHoldStartMs = 0;
    int sendEveryMs = 50;

    int lastTxCount = 0;
    int lastRxCount = 0;
    int lastSendMs = 0;
    uint8_t payload[64];


    uint32_t seed = 0x12345;
    auto nextByte = [&]() {
        seed = seed * 1103515245u + 12345u;
        return (uint8_t)(seed >> 16);
    };
    auto fillPayload = [&](uint8_t *b, int n) {
        if (mode == 0) {
            static const char HEX_DIGITS[] =
                "0123456789abcdefghijklmnopqrstuvwxyz";
            static int seqCount = 0;
            for (int i = 0; i < n; i++) {
                b[i] = (uint8_t)
                    HEX_DIGITS[(seqCount + i) % 36];
            }
            seqCount = (seqCount + n) % 36;
        } else {
            for (int i = 0; i < n; i++)
                b[i] = nextByte();
        }
    };

    const char *modeName =
        (mode == 0) ? "sequential" : "random";
    const char *relName = reliable
        ? "reliable (COBS+ARQ)"
        : "unreliable (raw)";
    printf("=== AutoLink Loopback Test ===\n");
    printf("Run for %d s, %s mode, %s link\n", seconds,
           modeName, relName);
    if (verbose)
        printf(
            "(verbose: full library debug logging)\n");
    printf("Ping -> Pong on MockHal pipe; both at "
           "115200 baud\n");
    printf("Time   | Ping st | Pong st | msg TX | msg "
           "RX | arq retx | errs\n");
    printf("-------|---------|---------|--------|-----"
           "---|----------|------\n");

    for (;;) {
        auto now = clk::now();
        auto wallMs =
            std::chrono::duration_cast<
                std::chrono::milliseconds>(now - t0)
                .count();
        if (wallMs >= seconds * 1000)
            break;
        auto deltaMs = std::chrono::duration_cast<
                           std::chrono::milliseconds>(
                           now - lastTick)
                           .count();
        lastTick = now;
        if (deltaMs < 0)
            deltaMs = 0;
        if (deltaMs > 100)
            deltaMs = 100;


        {
            uint32_t pre = (uint32_t)deltaMs;

            g_pingHal->pumpClock(pre);
            g_pongHal->pumpClock(pre);
        }
        pipe_now();


        if (ping.getState() == State::OK &&
            pong.getState() == State::OK) {
            if (!sawOk) {
                sawOk = true;
                okHoldStartMs = (int)wallMs;
            }
            okHoldMs = (int)wallMs - okHoldStartMs;
            int wallMod = (int)(wallMs % sendEveryMs);
            if (wallMod < (int)deltaMs + 1 &&
                (wallMs - lastSendMs) >= sendEveryMs) {
                fillPayload(payload, 64);
                ping.sendMsg(payload, 64);
                lastSendMs = (int)wallMs;
                txCount++;
            }
        }


        if (pong.getState() == State::OK) {
            uint8_t buf[256];
            int cap = (int)sizeof(buf);
            int n;


            while ((n = pong.recvMsg(buf, cap)) > 0) {
                rxCount++;
                if (!pong.sendMsg(buf, n)) {
                }
            }
        }


        Stats ps, qs;
        ping.getStats(ps);
        pong.getStats(qs);
        arqRetx =
            (int)ps.discCount + (int)qs.discCount;
        frameErrs =
            (int)ps.frameErrs + (int)qs.frameErrs;


        auto sincePrint =
            std::chrono::duration_cast<
                std::chrono::milliseconds>(now -
                                           lastPrint)
                .count();
        if (sincePrint >= 1000) {
            lastPrint = now;
            int dt = (int)sincePrint;
            int dtx = txCount - lastTxCount;
            int drx = rxCount - lastRxCount;
            lastTxCount = txCount;
            lastRxCount = rxCount;
            auto stp = [](State s) {
                return s == State::OK ? "OK"
                    : s == State::SWP ? "SWP"
                                      : "LCK";
            };
            printf("T=%4ds | %-7s | %-7s | %4d/s | "
                   "%4d/s | %8d | %4d\n",
                   (int)(wallMs / 1000),
                   stp(ping.getState()),
                   stp(pong.getState()),
                   (dtx * 1000) / dt,
                   (drx * 1000) / dt, arqRetx,
                   frameErrs);
            fflush(stdout);
        }
    }

    auto wallMs =
        std::chrono::duration_cast<
            std::chrono::milliseconds>(clk::now() - t0)
            .count();
    printf("\n=== Final ===\n");
    printf(
        "Total: %d ms simulated (real time %d ms)\n",
        (int)wallMs, (int)wallMs);
    printf("Ping state: %s, Pong state: %s\n",
           ping.getState() == State::OK        ? "OK"
               : ping.getState() == State::SWP ? "SWP"
                                               : "LCK",
           pong.getState() == State::OK ? "OK"
               : pong.getState() == State::SWP
               ? "SWP"
               : "LCK");
    printf("Messages TX: %d, RX: %d\n", txCount,
           rxCount);
    Stats ps, qs;
    ping.getStats(ps);
    pong.getStats(qs);
    printf("Ping: tx=%llu rx=%llu disc=%llu "
           "frameErrs=%llu\n",
           (unsigned long long)ps.tx,
           (unsigned long long)ps.rx,
           (unsigned long long)ps.discCount,
           (unsigned long long)ps.frameErrs);
    printf("Pong: tx=%llu rx=%llu disc=%llu "
           "frameErrs=%llu\n",
           (unsigned long long)qs.tx,
           (unsigned long long)qs.rx,
           (unsigned long long)qs.discCount,
           (unsigned long long)qs.frameErrs);

    if (sawOk && okHoldMs >= 1000 && rxCount > 0) {
        printf("\nPASS: link reached OK, held >= 1s, "
               "received data\n");
        return 0;
    }
    if (sawOk) {
        printf(
            "\nPARTIAL: link reached OK but held < 1s "
            "(okHoldMs=%d) or no data (rxCount=%d)\n",
            okHoldMs, rxCount);
        return 1;
    }
    printf("\nFAIL: link never reached OK\n");
    return 2;
}

int main(int argc, char **argv)
{
    int seconds = 30;
    bool verbose = false;
    int mode = 0;
    bool reliable = true;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "verbose" || a == "-v") {
            verbose = true;
        } else if (a == "sequential" || a == "seq") {
            mode = 0;
        } else if (a == "random" || a == "rand") {
            mode = 1;
        } else if (a == "reliable" || a == "rel") {
            reliable = true;
        } else if (a == "unreliable" || a == "raw") {
            reliable = false;
        } else {
            seconds = std::atoi(a.c_str());
        }
    }
    if (seconds < 1)
        seconds = 1;
    if (seconds > 600)
        seconds = 600;
    return run_loopback(seconds, verbose, mode,
                        reliable);
}
