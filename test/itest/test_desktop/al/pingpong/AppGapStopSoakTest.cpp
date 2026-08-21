// Ping/Pong cannot be host-instantiated: their constructors take
// uart_port_t + GPIO pin numbers and own an EspHal internally
// (src/al/pingpong/Ping.h/Pong.h), so no test in this suite
// constructs the real application classes — every existing pin is
// either a source-grep on Ping.h's text or a direct test of
// decideGapTransition() in isolation (PingGapTransitionTest,
// PingGapLatchTest). This does not construct a real Ping/Pong
// either — that needs Ping.h refactored to accept an injectable
// IHal first, which is a testability change, not a fix for the six
// field bugs. What it does instead: reimplements Ping's gap-stop
// wiring exactly (same decideGapTransition() call, same gapCand
// selection, same isAcked()-based pending check —
// al/pingpong/PingGap.h and the Link NAK/ACK API are the only
// things under test), sits it on top of two real Link objects, and
// drives real traffic under real wire loss (MockHal::
// frameDropPermille) across all four mode x fill-shape cells long
// enough to cross several 256-seq cobsSeq wraps.
//
// What this pins: general app-layer soundness under sustained loss
// across cells that had zero coverage at this layer before —
// bounded delivery, no crash, no permanent stall, in both SYNC and
// ASYNC and both fill shapes. It is NOT a regression pin for the
// gap-stop latch itself: reverting the lastNakSeq_ release (the
// fix in LinkRx.cpp) was verified by hand to change nothing
// observable here — under this traffic/loss shape, NAKs arrive
// often enough that lastNakSeq_ is refreshed to a recent value
// before a stale one survives long enough to matter, so the latch
// bug does not reproduce at this loss rate and message cadence.
// PingGapLatchTest is the actual toggle-red-verified regression
// pin for that mechanism; this file is soak coverage alongside it,
// not a replacement for it.
//
// Nor is it a regression pin for the SYNC NAK fast-path
// (LinkArq::noteNakWake): this harness drives SYNC's retry ladder
// itself on its own timer (test_syncRtoStep on a syncAckTimeoutMs
// cadence) rather than calling the blocking sendMsg()/
// syncAwaitAcked_unlocked() that noteNakWake speeds up, so it never
// exercises waitForAck's internals where that fix lives — verified
// by hand, reverting it changes nothing observable here either.
// SyncNakFastRetxTest is the toggle-red-verified pin for that fix.
//
// SYNC's sendMsg blocks until acked, which this single-threaded
// harness cannot service (nothing can pipe the peer's ACK back
// while the test thread is stuck inside sendMsg). Follows
// loopback_test.cpp's established pattern instead:
// test_sendMsgBegin/test_sendMsgStillWaiting split the blocking
// call into begin + poll so the harness keeps pumping the clock
// and piping data while a SYNC send is outstanding.
//
// Assertion: no gap-stop episode may run past a bounded stall.
#include "al/link/Link.h"
#include "al/link/arq/ArqCache.h"
#include "al/pingpong/PingGap.h"
#include "MockHal.h"
#include "LinkTestAccessor.h"
#include "al/util/log/Log.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

using namespace autolink;

namespace {

// Mirrors Ping::loop()'s gap-stop wiring (PingGap.h + Link's
// lastNakSeq()/isAcked() are the real production calls; this
// struct is just the small amount of bookkeeping Ping.h itself
// keeps around them).
struct GapStopSide {
    Link *link;
    uint8_t gapSeq = PING_GAP_NO_GAP;
    uint32_t gapStopEnteredMs = 0;
    uint32_t worstGapStopMs = 0;
    int enters = 0;
    bool inGap = false;

    void tick(uint32_t nowMs) {
        uint8_t lastNak = link->lastNakSeq();
        uint8_t gapCand = (gapSeq == PING_GAP_NO_GAP) ? lastNak : gapSeq;
        bool gapPending =
            (gapCand != PING_GAP_NO_GAP) && !link->isAcked(gapCand);
        uint8_t next = gapSeq;
        GapAction a = decideGapTransition(gapSeq, lastNak, gapPending, next);
        if (a == GapAction::Enter) {
            gapStopEnteredMs = nowMs;
            enters++;
            inGap = true;
        }
        if (a == GapAction::Resume && inGap) {
            uint32_t dur = nowMs - gapStopEnteredMs;
            if (dur > worstGapStopMs)
                worstGapStopMs = dur;
            inGap = false;
        }
        gapSeq = next;
    }

    // Current stall length if still inside a gap-stop episode right
    // now (covers an episode still open at the end of the sim).
    uint32_t openStallMs(uint32_t nowMs) const {
        return inGap ? (nowMs - gapStopEnteredMs) : 0;
    }

    bool paused() const { return gapSeq != PING_GAP_NO_GAP; }
};

int pickLen(bool random, int maxMsg, uint32_t *rngState) {
    if (!random)
        return 8; // serial/fixed-size fill
    *rngState = (*rngState * 1103515245u + 12345u);
    int r = (int)((*rngState >> 8) % (uint32_t)maxMsg);
    return r < 1 ? 1 : r;
}

// AL92-19: the soak's only pass criterion was the
// gap-stop stall bound — no floor on sent/delivered
// and no check that a gap-stop episode actually
// closed. A 46% ASYNC/random throughput regression
// (the AL92-17 bug: a NAK-driven resend no longer
// reseated the RTO, so a just-resent slot could fire
// a duplicate sweep retx) and a gap-stop episode left
// open at sim end both printed "PASSED (4/4 cells)"
// under the old criteria. CellResult exposes what the
// stall-only return threw away so main() can assert on
// it too.
struct CellResult {
    uint32_t worstGapStopMs;
    int sent;
    int delivered;
    bool episodeStillOpen;
};

CellResult runCell(const char *name, AutoLinkConfig::Mode mode,
                   bool randomFill, int lossPermille, uint32_t simMs) {
    ArqCache pingArq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
    ArqCache pongArq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
    AutoLinkConfig cfg;
    cfg.mode = mode;
    cfg.maxMsg = 128;
    cfg.syncAckTimeoutMs = 80;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;

    Link ping(mHal, pingArq, true, cfg);
    Link pong(sHal, pongArq, false, cfg);
    LinkTestAccessor pingT(ping);
    Log::log().setLevel(Log::ERROR);
    // Negotiate to OK with loss off — sweep/BREAK negotiation isn't
    // what this test is about, and 5% loss during it can prevent
    // lock inside the sim budget entirely (matches
    // loopback_losssweep_test's established pattern).
    negotiate_to_ok(ping, pong, mHal, sHal);
    mHal.frameDropPermille = lossPermille;
    sHal.frameDropPermille = lossPermille;

    GapStopSide pingGap{ &ping };
    GapStopSide pongGap{ &pong };
    uint32_t rng = 0xC0FFEEu;
    uint8_t sendBuf[128];
    uint8_t recvBuf[512];
    for (uint32_t i = 0; i < sizeof(sendBuf); i++)
        sendBuf[i] = (uint8_t)i;

    int sent = 0, delivered = 0;
    bool syncWaiting = false;
    uint32_t lastRtoStepMs = 0;
    for (uint32_t t = 0; t < simMs; t++) {
        mHal.pumpClock(1);
        sHal.pumpClock(1);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);

        pingGap.tick(t);
        pongGap.tick(t);

        int n = pong.recvMsg(recvBuf, sizeof(recvBuf));
        if (n > 0)
            delivered++;

        if (mode == AutoLinkConfig::Mode::SYNC) {
            if (syncWaiting) {
                if (!pingT.sendMsgStillWaiting()) {
                    syncWaiting = false; // acked
                } else if ((t - lastRtoStepMs) >=
                           (uint32_t)cfg.syncAckTimeoutMs) {
                    // Mirrors syncAwaitAcked_unlocked exactly: on
                    // RTO expiry, retransmit verbatim and start a
                    // fresh RTO window. A single-shot send with a
                    // passive wait (this loop's earlier shape)
                    // abandons a lost frame after one timeout
                    // instead of retrying it — understating real
                    // sendMsg()'s recovery and its own throughput
                    // under loss.
                    if (!pingT.syncRtoStep()) {
                        syncWaiting = false; // ladder exhausted, give up
                    }
                    lastRtoStepMs = t;
                }
            } else if (ping.getState() == State::OK && !pingGap.paused()) {
                int len = pickLen(randomFill, cfg.maxMsg, &rng);
                if (pingT.sendMsgBegin(sendBuf, len)) {
                    sent++;
                    syncWaiting = true;
                    lastRtoStepMs = t;
                }
            }
        } else if (ping.getState() == State::OK && !pingGap.paused()) {
            int len = pickLen(randomFill, cfg.maxMsg, &rng);
            if (ping.sendMsg(sendBuf, len))
                sent++;
        }
    }

    uint32_t worst = pingGap.worstGapStopMs > pongGap.worstGapStopMs
        ? pingGap.worstGapStopMs
        : pongGap.worstGapStopMs;
    uint32_t open = pingGap.openStallMs(simMs) > pongGap.openStallMs(simMs)
        ? pingGap.openStallMs(simMs)
        : pongGap.openStallMs(simMs);
    if (open > worst)
        worst = open;
    printf("  %-16s sent=%-5d delivered=%-5d ping.enters=%-3d "
           "pong.enters=%-3d worstGapStop=%u ms%s\n",
           name, sent, delivered, pingGap.enters, pongGap.enters, worst,
           open > 0 ? " (episode still open at sim end)" : "");
    return CellResult{ worst, sent, delivered, open > 0 };
}

} // namespace

int main() {
    printf("=== App-layer gap-stop soak: mode x fill-shape matrix ===\n");
    // 50 permille (5%) loss is well above anything in the field
    // logs — chosen to force NAK bursts and repeated cobsSeq wraps
    // inside a short sim, not to model a realistic line.
    const int kLossPermille = 50;
    const uint32_t kSimMs = 20000; // several hundred msgs => several wraps

    struct Cell {
        const char *name;
        AutoLinkConfig::Mode mode;
        bool randomFill;
        // AL92-19: floors measured on a healthy
        // build with the AL92-17 fix applied
        // (ASYNC/serial 434/425, ASYNC/random
        // 102/85, SYNC/serial 972/971, SYNC/random
        // 161/161), each with margin. minSent is the
        // load-bearing check: the AL92-17 regression
        // (a NAK-driven resend no longer reseating the
        // RTO) collapsed ASYNC/random's SENT count from
        // 102 to 46 — the delivery RATIO on what little
        // got sent (31/46 = 67%) stayed inside any
        // reasonable percentage floor, so only an
        // absolute sent floor catches it. minDeliveredPct
        // is a secondary check against a regression that
        // keeps sending but stops delivering.
        int minSent;
        int minDeliveredPct;
    };
    const Cell cells[] = {
        { "ASYNC/serial", AutoLinkConfig::Mode::ASYNC, false, 300, 85 },
        { "ASYNC/random", AutoLinkConfig::Mode::ASYNC, true, 70, 60 },
        { "SYNC/serial", AutoLinkConfig::Mode::SYNC, false, 650, 90 },
        { "SYNC/random", AutoLinkConfig::Mode::SYNC, true, 110, 90 },
    };

    // This harness includes no force-resume timer (that lives in
    // Ping.h, not under test here) — the bound enforced is general
    // soundness: a working gap-stop clears within a handful of
    // RTOs regardless of mode or fill shape, under sustained loss.
    const uint32_t kMaxAcceptableStallMs = 2000;

    bool allOk = true;
    for (const Cell &c : cells) {
        CellResult r =
            runCell(c.name, c.mode, c.randomFill, kLossPermille, kSimMs);
        if (r.worstGapStopMs > kMaxAcceptableStallMs) {
            printf("  FAIL: %s worst gap-stop %u ms exceeds %u ms\n", c.name,
                   r.worstGapStopMs, kMaxAcceptableStallMs);
            allOk = false;
        }
        // A gap-stop episode still open at sim end is
        // NOT by itself a failure — it is already
        // covered by worstGapStopMs (open-so-far counts
        // toward worst above), and forcing full closure
        // by an arbitrary sim cutoff is sensitive to
        // exactly where the RNG-driven traffic happened
        // to land, not to link health. Print it as a
        // diagnostic only.
        if (r.episodeStillOpen) {
            printf("  NOTE: %s ended the sim with a gap-stop episode "
                   "still open (already reflected in worstGapStop)\n",
                   c.name);
        }
        if (r.sent < c.minSent) {
            printf("  FAIL: %s sent=%d, below the %d-message floor — "
                   "throughput regression (an admission-gate wedge, a "
                   "resend loop, etc.)\n",
                   c.name, r.sent, c.minSent);
            allOk = false;
        }
        if (r.sent > 0) {
            int pct = (r.delivered * 100) / r.sent;
            if (pct < c.minDeliveredPct) {
                printf("  FAIL: %s delivered %d/%d (%d%%), below the "
                       "%d%% floor\n",
                       c.name, r.delivered, r.sent, pct, c.minDeliveredPct);
                allOk = false;
            }
        }
    }

    if (!allOk) {
        printf("=== App-layer gap-stop soak FAILED ===\n");
        return 1;
    }
    printf("=== App-layer gap-stop soak PASSED (4/4 cells) ===\n");
    return 0;
}
