// Random-size multi-chunk ASYNC under 1% frame loss.
// Closes the integration gap that let the retransmit
// throttle ship: loopback_noise_test only sends 64-byte
// single-frame payloads, so multi-chunk reassembly under
// loss had no end-to-end coverage. Every payload here is
// 300-3000 bytes (2-13 chunks at MAX_CHUNK=250), content
// is byte-keyed per message index, and the receiver
// verifies every delivered byte -- a cross-wired
// reassembly or stale-chunk splice fails loudly.
// Reverting the inline NAK retransmit / full RTO sweep
// collapses delivery to ~2% and turns the floor red.
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include <cstring>
#    include <chrono>
#    include <thread>
#    include <vector>
#    include "al/link/Link.h"
#    include "al/link/arq/ArqCache.h"
#    include "LinkTestAccessor.h"
#    include "al/util/Log.h"
#    include "MockHal.h"
#    include "NullArqCache.h"

using namespace autolink;

static int g_runMs = 5000;

static ArqCache g_pingArq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
static ArqCache g_pongArq{ AUTOLINK_ARQ_PIPELINE_WINDOW };

static uint32_t g_rng = 0x5EED1234;
static uint32_t rnd() {
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng >> 8;
}

// [0..1]=idx LE, [2..3]=len LE, body[i]=idx*31+i.
static void fillMsg(std::vector<uint8_t> &m, uint16_t idx, uint16_t len) {
    m.resize(len);
    m[0] = (uint8_t)idx;
    m[1] = (uint8_t)(idx >> 8);
    m[2] = (uint8_t)len;
    m[3] = (uint8_t)(len >> 8);
    for (uint16_t i = 4; i < len; i++)
        m[i] = (uint8_t)(idx * 31 + i);
}
static bool checkMsg(const uint8_t *b, int n) {
    if (n < 4)
        return false;
    uint16_t idx = (uint16_t)(b[0] | (b[1] << 8));
    uint16_t len = (uint16_t)(b[2] | (b[3] << 8));
    if ((int)len != n)
        return false;
    for (uint16_t i = 4; i < len; i++)
        if (b[i] != (uint8_t)(idx * 31 + i))
            return false;
    return true;
}

void test_multichunk_async_under_loss() {
    std::cout << "\n=== Test: multi-chunk ASYNC under 1% frame loss ==="
              << std::endl;

    const int DROP_PCT = 1;
    const int MIN_LEN = 300, MAX_LEN = 3000;

    MockHal mHal, sHal;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 70000;
    cfg.maxMsg = 65535;
    // This test pins ARQ recovery, not the err-rate
    // resweep policy: at 1% drop on 250-byte chunks the
    // frame-error rate would trip the default gates and
    // the run would measure sweep re-lock, not retx.
    cfg.errThreshold = 1 << 30;
    cfg.errRateWindow = 0;
    // Large enough that idle watchdogs stay out of a 5 s
    // run, but > 0 so the OK-timer RTO sweep (the lost-NAK
    // backstop) actually ticks.
    cfg.idleTimeoutMs = 30000;
    g_pingArq.clearAll();
    g_pongArq.clearAll();
    Link ping(mHal, g_pingArq, true, cfg);
    Link pong(sHal, g_pongArq, false, cfg);
    mHal.peer = &sHal;
    sHal.peer = &mHal;

    ping.begin();
    pong.begin();
    negotiate_to_ok(ping, pong, mHal, sHal);
    assert(ping.getState() == State::OK);
    assert(pong.getState() == State::OK);

    mHal.frameDropPct = DROP_PCT;
    sHal.frameDropPct = DROP_PCT;
    mHal.dropRngSeed = 0xDEADBEEF;
    sHal.dropRngSeed = 0xCAFEBABE;

    auto t0 = std::chrono::steady_clock::now();
    int txCount = 0, rxCount = 0, badCount = 0, multiChunkRx = 0;
    uint16_t nextIdx = 0;
    std::vector<uint8_t> msg;
    int lastPrint = 0;

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
            uint16_t len =
                (uint16_t)(MIN_LEN + (rnd() % (MAX_LEN - MIN_LEN + 1)));
            fillMsg(msg, nextIdx, len);
            if (ping.sendMsg(msg.data(), (int)msg.size())) {
                txCount++;
                nextIdx++;
            }

            uint8_t buf[4096];
            int n;
            while ((n = pong.recvMsg(buf, sizeof(buf))) > 0) {
                if (checkMsg(buf, n)) {
                    rxCount++;
                    if (chunksForMsgLen(n) > 1)
                        multiChunkRx++;
                } else {
                    badCount++;
                }
            }
        }

        if (wallMs - lastPrint >= 1000) {
            lastPrint = wallMs;
            Stats ps, qs;
            ping.getStats(ps);
            pong.getStats(qs);
            std::cout << "T=" << (wallMs / 1000) << "s  TX=" << txCount
                      << " RX=" << rxCount << " bad=" << badCount
                      << " disc=" << (ps.discCount + qs.discCount)
                      << " dropped=" << (mHal.bytesDropped + sHal.bytesDropped)
                      << std::endl;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    Stats ps, qs;
    ping.getStats(ps);
    pong.getStats(qs);
    int totalDropped = mHal.bytesDropped + sHal.bytesDropped;

    std::cout << "\n=== Final ===" << std::endl;
    std::cout << "Messages TX=" << txCount << " RX=" << rxCount
              << " (multi-chunk=" << multiChunkRx << ") corrupted=" << badCount
              << std::endl;
    std::cout << "Disconnects=" << (ps.discCount + qs.discCount)
              << " bytesDropped=" << totalDropped << std::endl;

    assert(totalDropped > 0 && "wire noise knob is broken — nothing dropped");
    assert(txCount > 10 && "sender wedged — almost nothing sent");

    if (badCount != 0) {
        std::cout << "\nFAIL: " << badCount
                  << " delivered messages had corrupted content — "
                  << "reassembly spliced chunks across messages or "
                  << "delivered a stale retransmit into the wrong slot."
                  << std::endl;
        assert(false && "multichunk: silent corruption");
    }
    if (multiChunkRx == 0) {
        std::cout << "\nFAIL: no multi-chunk message ever reassembled — "
                  << "the exact pre-fix failure shape (retransmit "
                  << "throttled below the reorder expiry)." << std::endl;
        assert(false && "multichunk: zero multi-chunk deliveries");
    }
    int floorPct = 30;
    if (rxCount * 100 < txCount * floorPct) {
        std::cout << "\nFAIL: delivery " << rxCount << "/" << txCount
                  << " below the " << floorPct
                  << "% floor at 1% frame loss — NAK fast-retransmit or "
                  << "the RTO sweep has been regressed." << std::endl;
        assert(false && "multichunk: delivery below floor");
    }

    std::cout << "\nPASS: multi-chunk ASYNC delivered " << rxCount << "/"
              << txCount << " with zero corruption under " << DROP_PCT
              << "% loss." << std::endl;
}

int main(int argc, char **argv) {
    bool verbose = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "verbose" || a == "-v")
            verbose = true;
        else
            g_runMs = std::atoi(argv[i]);
        if (g_runMs < 500)
            g_runMs = 500;
    }
    Log::log().setLevel(verbose ? Log::Level::DEBUG : Log::Level::WARNING);
    test_multichunk_async_under_loss();
    std::cout << "\nAll multichunk loopback tests PASSED." << std::endl;
    return 0;
}

#endif
