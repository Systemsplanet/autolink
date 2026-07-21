// ASYNC delivery-floor sweep at 0.1% / 1% / 5% true
// per-frame loss (MockHal wholeFrameDropPermille drops
// whole 0x00-delimited COBS frames, unlike the legacy
// 5-byte block knob whose effective per-frame rate
// scales with frame size). Pins the todo target of at
// least 99% delivery at 1% frame loss, zero disconnects
// at 0.1% and 1%, and survival at 5%.
// Pins the delivery floor per loss rate now that the
// receiver's reorder hold is derived from the sender's
// retransmit budget (reorderHoldEffectiveMs = (maxRetx+2)
// x RTO) instead of a fixed 1500 ms. With the fixed hold,
// losses at 1% trace to reorder slots expiring before the
// retransmit closes the gap; reverting the derived hold
// (drop paths back to cfg.reorderHoldMs) sags delivery
// below the pinned floors and turns this suite red.
//
// Also pins pool headroom under sustained flood: at 0.1%
// and 1% the run must complete with zero disconnects — a
// pool-exhaustion or watchdog reset under flood is a
// regression even if delivery recovers after the resweep.
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

static uint32_t g_rng = 0x5EED4321;
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

struct RateResult {
    int tx = 0;
    int rx = 0;
    int bad = 0;
    uint64_t disc = 0;
    int dropped = 0;
    std::vector<bool> got;       // by tx idx
    std::vector<uint16_t> txLen; // by tx idx
};

// One measured run at dropPermille (per-mille of 5-byte
// wire blocks). Mixed sizes: mostly single-frame with a
// steady multi-chunk share so both the merged path and
// the hdr+chunks path see the loss.
static RateResult runRate(int dropPermille) {
    std::cout << "--- rate " << dropPermille << " permille ---" << std::endl;
    MockHal mHal, sHal;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 70000;
    cfg.maxMsg = 65535;
    // Pin ARQ recovery, not the err-rate resweep policy.
    cfg.errThreshold = 1 << 30;
    cfg.errRateWindow = 0;
    // Idle watchdogs stay out of the run; > 0 so the
    // OK-timer RTO sweep (lost-NAK backstop) ticks.
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

    // Disc baseline: a re-lock during negotiation is
    // sweep-phase behaviour, not flood behaviour —
    // the zero-disc pin measures the flood window
    // only.
    Stats base0, base1;
    ping.getStats(base0);
    pong.getStats(base1);
    uint64_t discBase = base0.discCount + base1.discCount;

    mHal.wholeFrameDropPermille = dropPermille;
    sHal.wholeFrameDropPermille = dropPermille;
    mHal.dropRngSeed = 0xDEADBEEF;
    sHal.dropRngSeed = 0xCAFEBABE;

    auto t0 = std::chrono::steady_clock::now();
    RateResult r;
    uint16_t nextIdx = 0;
    std::vector<uint8_t> msg;

    while (true) {
        auto now = std::chrono::steady_clock::now();
        int wallMs =
            (int)std::chrono::duration_cast<std::chrono::milliseconds>(now - t0)
                .count();
        if (wallMs >= g_runMs)
            break;

        mHal.pumpClock(20);
        sHal.pumpClock(20);
        Log::log().drainPending();
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);

        if (ping.getState() == State::OK && pong.getState() == State::OK) {
            // 1-in-4 multi-chunk (300-1200 B), else
            // single-frame (16-200 B).
            uint16_t len = (rnd() % 4 == 0) ? (uint16_t)(300 + (rnd() % 901))
                                            : (uint16_t)(16 + (rnd() % 185));
            fillMsg(msg, nextIdx, len);
            if (ping.sendMsg(msg.data(), (int)msg.size())) {
                r.tx++;
                r.got.push_back(false);
                r.txLen.push_back(len);
                nextIdx++;
            }

            uint8_t buf[4096];
            int n;
            while ((n = pong.recvMsg(buf, sizeof(buf))) > 0) {
                if (checkMsg(buf, n)) {
                    r.rx++;
                    uint16_t idx = (uint16_t)(buf[0] | (buf[1] << 8));
                    if (idx < r.got.size())
                        r.got[idx] = true;
                } else
                    r.bad++;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // Drain: stop sending, keep the wire and clocks
    // running so in-flight tail messages land. The
    // floors measure delivery, not cutoff luck.
    for (int i = 0; i < 400; i++) {
        mHal.pumpClock(20);
        sHal.pumpClock(20);
        Log::log().drainPending();
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
        uint8_t buf[4096];
        int n;
        while ((n = pong.recvMsg(buf, sizeof(buf))) > 0) {
            if (checkMsg(buf, n)) {
                r.rx++;
                uint16_t idx = (uint16_t)(buf[0] | (buf[1] << 8));
                if (idx < r.got.size())
                    r.got[idx] = true;
            } else
                r.bad++;
        }
    }
    for (size_t i = 0; i < r.got.size(); i++)
        if (!r.got[i])
            std::cout << "  MISSING idx=" << i << " len=" << r.txLen[i]
                      << std::endl;

    Stats ps, qs;
    ping.getStats(ps);
    pong.getStats(qs);
    Diag pd, qd;
    ping.getDiag(pd);
    pong.getDiag(qd);
    std::cout << "  diag pong: gaps=" << qd.gaps << " stale=" << qd.stale
              << " lostMsgs=" << qd.lostMsgs << " frameErrs=" << qs.frameErrs
              << " | ping pend-retired via sweep, frameErrs=" << ps.frameErrs
              << std::endl;
    r.disc = ps.discCount + qs.discCount - discBase;
    r.dropped = mHal.bytesDropped + sHal.bytesDropped;
    return r;
}

static void checkFloor(const char *label, const RateResult &r, int floorPct,
                       bool pinZeroDisc) {
    std::cout << label << ": TX=" << r.tx << " RX=" << r.rx << " bad=" << r.bad
              << " disc=" << r.disc << " dropped=" << r.dropped << " ("
              << (r.tx ? (r.rx * 100 / r.tx) : 0) << "%)" << std::endl;
    assert(r.tx > 10 && "sender wedged — almost nothing sent");
    assert(r.bad == 0 && "silent corruption under loss");
    if (r.rx * 100 < r.tx * floorPct) {
        std::cout << "FAIL: delivery below the " << floorPct << "% floor — "
                  << "the reorder hold no longer covers the retransmit "
                  << "budget (or retx has been regressed)." << std::endl;
        assert(false && "losssweep: delivery below floor");
    }
    if (pinZeroDisc && r.disc != 0) {
        std::cout << "FAIL: " << r.disc << " disconnect(s) under flood — "
                  << "pool headroom regression (exhaustion or watchdog "
                  << "reset mid-run)." << std::endl;
        assert(false && "losssweep: disconnect under flood");
    }
}

void test_loss_sweep() {
    std::cout << "\n=== Test: ASYNC delivery-floor sweep ===" << std::endl;

    RateResult r01 = runRate(1); // 0.1% frame loss
    RateResult r1 = runRate(10); // 1% frame loss
    RateResult r5 = runRate(50); // 5% frame loss

    assert(r01.dropped + r1.dropped + r5.dropped > 0 &&
           "wire noise knob is broken — nothing dropped");

    std::cout << "\n=== Floors ===" << std::endl;
    checkFloor("0.1%", r01, 99, true);
    checkFloor("  1%", r1, 99, true);
    // 5%: heavy sustained loss — pin a delivery
    // floor and zero corruption; disconnects
    // (resweeps) are tolerated.
    checkFloor("  5%", r5, 80, false);
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
    test_loss_sweep();
    std::cout << "\nAll loss-sweep loopback tests PASSED." << std::endl;
    return 0;
}

#endif
