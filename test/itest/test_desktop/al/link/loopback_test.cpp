// Two-Link host loopback end-to-end smoke.
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

// Minimal ARQ cache for the itest harness.
// Same shape as AutoLink's internal pool
// but kept here so the itest can drive raw
// Link instances. The Link's timer-driven
// retransmit path calls arqRetxCallback_,
// which is null by default — without
// these hooks installed, every wire drop
// becomes a permanent loss in the itest.
struct ItestArq {
    Link *owner;
    uint8_t buf[256][260];
    int len[256];
    bool in_use[256];
    ItestArq() : owner(nullptr)
    {
        memset(len, 0, sizeof len);
        memset(in_use, 0, sizeof in_use);
    }
};
static ItestArq g_pingArq;
static ItestArq g_pongArq;
static void itest_arq_insert(uint8_t seq, const uint8_t *b, int n, uint8_t,
                             void *ctx)
{
    ItestArq *a = (ItestArq *)ctx;
    if (n <= 0 || n > 256)
        return;
    if (a->in_use[seq])
        a->in_use[seq] = false;
    memcpy(a->buf[seq], b, n);
    a->len[seq] = n;
    a->in_use[seq] = true;
}
static bool itest_arq_retx(uint8_t seq, void *ctx)
{
    ItestArq *a = (ItestArq *)ctx;
    if (!a->in_use[seq])
        return false;
    a->owner->resendCobsFrame(seq, a->buf[seq], a->len[seq]);
    return false;
}
static void itest_arq_clear(void *ctx)
{
    ItestArq *a = (ItestArq *)ctx;
    for (int i = 0; i < 256; i++) {
        a->in_use[i] = false;
        a->len[i] = 0;
    }
}
static bool itest_arq_ack(uint8_t seq, void *ctx)
{
    ItestArq *a = (ItestArq *)ctx;
    a->in_use[seq] = false;
    a->len[seq] = 0;
    return false;
}

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

static int run_loopback(int seconds, bool verbose, int mode, bool reliable,
                        bool sync)
{
    AutoLinkConfig cfg;
    cfg.maxMsg = 256;
    cfg.idleTimeoutMs = 0;
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

    if (sync) {
        ping.setMode(AutoLinkConfig::Mode::SYNC);
        pong.setMode(AutoLinkConfig::Mode::SYNC);
    }

    // ASYNC only: install the test's ARQ
    // cache so NAK-driven retransmits can
    // actually re-send the cached frame.
    // SYNC doesn't need this — it waits
    // for the receiver's ACK inline.
    g_pingArq = ItestArq();
    g_pongArq = ItestArq();
    g_pingArq.owner = &ping;
    g_pongArq.owner = &pong;
    ping.setArqCacheHooks(&itest_arq_ack, &itest_arq_retx, nullptr,
                          &itest_arq_insert, &itest_arq_clear, &g_pingArq);
    pong.setArqCacheHooks(&itest_arq_ack, &itest_arq_retx, nullptr,
                          &itest_arq_insert, &itest_arq_clear, &g_pongArq);

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
                b[i] = (uint8_t)HEX_DIGITS[(seqCount + i) % 36];
            }
            seqCount = (seqCount + n) % 36;
        } else {
            for (int i = 0; i < n; i++)
                b[i] = nextByte();
        }
    };

    const char *modeName = (mode == 0) ? "sequential" : "random";
    const char *relName = reliable ? "reliable (COBS+ARQ)" : "unreliable (raw)";
    printf("=== AutoLink Loopback Test ===\n");
    printf("Run for %d s, %s mode, %s link\n", seconds, modeName, relName);
    if (verbose)
        printf("(verbose: full library debug logging)\n");
    printf("Ping -> Pong on MockHal pipe; both at  115200 baud\n");
    printf("Time   | Ping st | Pong st | msg TX | msg  RX | arq retx | errs\n");
    printf(
        "-------|---------|---------|--------|----- ---|----------|------\n");

    for (;;) {
        auto now = clk::now();
        auto wallMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - t0)
                .count();
        if (wallMs >= seconds * 1000)
            break;
        auto deltaMs = std::chrono::duration_cast<std::chrono::milliseconds>(
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

        if (ping.getState() == State::OK && pong.getState() == State::OK) {
            if (!sawOk) {
                sawOk = true;
                okHoldStartMs = (int)wallMs;
            }
            okHoldMs = (int)wallMs - okHoldStartMs;
            int wallMod = (int)(wallMs % sendEveryMs);
            if (wallMod < (int)deltaMs + 1 &&
                (wallMs - lastSendMs) >= sendEveryMs) {
                fillPayload(payload, 64);
                if (sync) {
                    // SYNC: split the blocking
                    // send into begin + step loop
                    // so the test thread can
                    // pump time for the link
                    // task to deliver the ACK.
                    if (ping.test_sendMsgBegin(payload, 64)) {
                        int budget = cfg.syncAckTimeoutMs + 50;
                        bool gotAck = false;
                        for (int i = 0; i < budget; i++) {
                            std::this_thread::sleep_for(
                                std::chrono::milliseconds(2));
                            g_pingHal->pumpClock(2);
                            g_pongHal->pumpClock(2);
                            pipe_now();
                            if (!ping.test_sendMsgStillWaiting()) {
                                gotAck = true;
                                break;
                            }
                        }
                        if (gotAck) {
                            lastSendMs = (int)wallMs;
                            txCount++;
                        }
                    }
                } else {
                    if (ping.sendMsg(payload, 64)) {
                        lastSendMs = (int)wallMs;
                        txCount++;
                    }
                }
            }
        }

        if (pong.getState() == State::OK) {
            uint8_t buf[256];
            int cap = (int)sizeof(buf);
            int n;

            while ((n = pong.recvMsg(buf, cap)) > 0) {
                rxCount++;
                if (sync) {
                    if (pong.test_sendMsgBegin(buf, n)) {
                        int budget = cfg.syncAckTimeoutMs + 50;
                        for (int i = 0; i < budget; i++) {
                            std::this_thread::sleep_for(
                                std::chrono::milliseconds(2));
                            g_pingHal->pumpClock(2);
                            g_pongHal->pumpClock(2);
                            pipe_now();
                            if (!pong.test_sendMsgStillWaiting())
                                break;
                        }
                    }
                } else {
                    pong.sendMsg(buf, n);
                }
            }
        }

        Stats ps, qs;
        ping.getStats(ps);
        pong.getStats(qs);
        arqRetx = (int)ps.discCount + (int)qs.discCount;
        frameErrs = (int)ps.frameErrs + (int)qs.frameErrs;

        auto sincePrint = std::chrono::duration_cast<std::chrono::milliseconds>(
                              now - lastPrint)
                              .count();
        if (sincePrint >= 1000) {
            lastPrint = now;
            int dt = (int)sincePrint;
            int dtx = txCount - lastTxCount;
            int drx = rxCount - lastRxCount;
            lastTxCount = txCount;
            lastRxCount = rxCount;
            auto stp = [](State s) {
                return s == State::OK ? "OK" : s == State::SWP ? "SWP" : "LCK";
            };
            printf("T=%4ds | %-7s | %-7s | %4d/s |  %4d/s | %8d | %4d\n",
                   (int)(wallMs / 1000), stp(ping.getState()),
                   stp(pong.getState()), (dtx * 1000) / dt, (drx * 1000) / dt,
                   arqRetx, frameErrs);
            fflush(stdout);
        }
    }

    auto wallMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - t0)
            .count();
    printf("\n=== Final ===\n");
    printf("Total: %d ms simulated (real time %d ms)\n", (int)wallMs,
           (int)wallMs);
    printf("Ping state: %s, Pong state: %s\n",
           ping.getState() == State::OK        ? "OK"
               : ping.getState() == State::SWP ? "SWP"
                                               : "LCK",
           pong.getState() == State::OK        ? "OK"
               : pong.getState() == State::SWP ? "SWP"
                                               : "LCK");
    printf("Messages TX: %d, RX: %d\n", txCount, rxCount);
    Stats ps, qs;
    ping.getStats(ps);
    pong.getStats(qs);
    printf("Ping: tx=%llu rx=%llu disc=%llu  frameErrs=%llu\n",
           (unsigned long long)ps.tx, (unsigned long long)ps.rx,
           (unsigned long long)ps.discCount, (unsigned long long)ps.frameErrs);
    printf("Pong: tx=%llu rx=%llu disc=%llu  frameErrs=%llu\n",
           (unsigned long long)qs.tx, (unsigned long long)qs.rx,
           (unsigned long long)qs.discCount, (unsigned long long)qs.frameErrs);

    if (sawOk && okHoldMs >= 1000 && rxCount > 0) {
        printf("\nPASS: link reached OK, held >= 1s,  received data\n");
        return 0;
    }
    if (sawOk) {
        printf(
            "\nPARTIAL: link reached OK but held < 1s  (okHoldMs=%d) or no data (rxCount=%d)\n",
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
    bool sync = false;
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
        } else if (a == "sync") {
            sync = true;
        } else if (a == "async") {
            sync = false;
        } else {
            seconds = std::atoi(a.c_str());
        }
    }
    if (seconds < 1)
        seconds = 1;
    if (seconds > 600)
        seconds = 600;
    return run_loopback(seconds, verbose, mode, reliable, sync);
}