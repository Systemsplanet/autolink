// loopback_noise_test.cpp — host regression test for protocol-level
// auto-baud fallback under wire noise.
//
// Bug history:
//   v5.0.0–v5.1.27: ALink::onPayload logged cobsSeq gaps (counted toward
//   gaps/lostMsgs for diagnostics) but did NOT bump the errs counter.
//   The error threshold (default 20) therefore only tripped on frame
//   CRC failures or app-buffer overflows, not on the common case of
//   "ARQ retransmit itself got lost on a noisy wire". The link stayed
//   at 115200 baud even though it was unreliable; the application layer
//   (PingPong) eventually broke the link manually, bypassing the
//   protocol's baud-sweep recovery.
//
// Fix: v5.1.28 makes a gap event call err_unlocked() to bump errs.
//   When errs exceeds cfg.errThreshold, the link drops and the SWP
//   baud-sweep re-runs at the next lower baud.
//
// What this test verifies:
//   1. Negotiate two ALinks into State::OK at 115200 baud.
//   2. Inject frame drops into the wire (via MockHal::frameDropPct).
//   3. Drive the loop for several seconds with messages flowing.
//   4. Assert: the link either re-sweeps to a lower baud (final spd <
//      115200) OR has disconnected at least once AND recovered to OK
//      again (the second case proves the protocol actually tried to
//      recover, even if it ended up back at the same baud).
//
// Why both conditions: at 115200 the noise rate that triggers a gap
// event is rare, so a single sweep may not reach the threshold during
// the short test window. We accept either proof of recovery.
//
// Reverting the v5.1.28 fix (re-introducing the gap-doesn't-count-errs
// behavior) makes this test fail because errs never climbs high enough
// to trip the threshold.

#ifndef ARDUINO

#include <iostream>
#include <cassert>
#include <chrono>
#include <thread>
#include "al/protocol/ALink.h"
#include "al/util/Log.h"
#include "MockHal.h"

using namespace autolink;

void test_loopback_noise_triggers_baud_fallback() {
    std::cout << "\n=== Test: Loopback under wire noise triggers baud fallback ===" << std::endl;

    // 30% frame drop. High enough to regularly lose a frame and cause
    // cobsSeq gaps at the receiver, low enough to leave room for the
    // link to actually do work between drops.
    const int DROP_PCT = 30;
    const int RUN_MS = 5000;

    MockHal mHal, sHal;
    AutoLinkConfig cfg;
    cfg.reliableMode = true;
    cfg.streamBufferSize = 70000;
    cfg.maxMsg = 65535;
    cfg.errThreshold = 10; // lower threshold so the test runs quickly
    ALink ping(mHal, true, cfg);
    ALink pong(sHal, false, cfg);
    mHal.peer = &sHal;
    sHal.peer = &mHal;

    ping.begin();
    pong.begin();
    negotiate_to_ok(ping, pong, mHal, sHal);
    assert(ping.getState() == State::OK);
    assert(pong.getState() == State::OK);

    uint32_t startBaud = mHal.spd;
    std::cout << "Negotiated OK at " << startBaud << " baud. frameDropPct="
              << DROP_PCT << "% errThreshold=" << cfg.errThreshold << std::endl;

    // Wire noise enabled NOW (post-negotiation). The pre-negotiation
    // PING/REQ/IDX exchange needs to succeed clean so we know OK is
    // stable; noise only affects post-OK data frames.
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
        int wallMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count();
        if (wallMs >= RUN_MS) break;

        // v5.1.40: pumpClock drives both sides from the link's
        // scheduled deadline. We still iterate in wall-clock
        // chunks (RUN_MS total) so the test stays real-time.
        mHal.pumpClock(20);
        sHal.pumpClock(20);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);

        if (ping.getState() == State::OK && pong.getState() == State::OK) {
            uint8_t payload[64];
            for (int i = 0; i < 64; i++) payload[i] = (uint8_t)('A' + (i % 26));
            if (ping.sendMsg(payload, 64)) txCount++;
            // echo pong -> ping (above pipe_data already moved pong's
            // echo bytes back, but we need pong to actually emit them)
            uint8_t buf[300];
            int n;
            while ((n = pong.recvMsg(buf, sizeof(buf))) > 0) {
                if (pong.sendMsg(buf, n)) rxCount++;
            }
        }

        // Track every baud change on both sides. SWP re-sweep will
        // walk cfg.allowedBauds[] downward; a healthy noisy link
        // settles at the highest baud that can survive the threshold.
        if (pingBauds.empty() || pingBauds.back() != (uint32_t)mHal.spd)
            pingBauds.push_back(mHal.spd);
        if (pongBauds.empty() || pongBauds.back() != (uint32_t)sHal.spd)
            pongBauds.push_back(sHal.spd);

        Stats ps, qs;
        ping.getStats(ps);
        pong.getStats(qs);
        uint64_t disc = ps.discCount + qs.discCount;
        if (disc > maxDisc) maxDisc = disc;

        if (wallMs - lastPrint >= 1000) {
            lastPrint = wallMs;
            std::cout << "T=" << (wallMs/1000) << "s  Ping@"
                      << mHal.spd << "  Pong@" << sHal.spd
                      << "  disc=" << disc
                      << "  bytesDropped=" << (mHal.bytesDropped + sHal.bytesDropped)
                      << std::endl;
        }

        // Real-time pacing so MockHal's nowMs advances and ARQ
        // retransmit timers actually fire. MockHal::now is wall-clock
        // based; we need to give the system clock time to move.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    auto wallMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    Stats ps, qs;
    ping.getStats(ps);
    pong.getStats(qs);

    std::cout << "\n=== Final ===" << std::endl;
    std::cout << "Ran " << wallMs << " ms" << std::endl;
    std::cout << "Final baud Ping=" << mHal.spd << " Pong=" << sHal.spd << std::endl;
    std::cout << "Baud history Ping: ";
    for (auto b : pingBauds) std::cout << b << " ";
    std::cout << "\nBaud history Pong: ";
    for (auto b : pongBauds) std::cout << b << " ";
    std::cout << "\nDisconnects: Ping=" << ps.discCount << " Pong=" << qs.discCount
              << " (total=" << maxDisc << ")" << std::endl;
    std::cout << "Frame errors: Ping=" << ps.frameErrs << " Pong=" << qs.frameErrs << std::endl;
    std::cout << "Messages TX=" << txCount << " RX=" << rxCount << std::endl;
    std::cout << "Bytes dropped by noise: " << (mHal.bytesDropped + sHal.bytesDropped) << std::endl;

    // The wire is dropping 30% of frames, so the link SHOULD see noise.
    // Sanity check: noise actually fired (otherwise the test is just
    // running clean and passing trivially).
    int totalDropped = mHal.bytesDropped + sHal.bytesDropped;
    assert(totalDropped > 0 && "frameDropPct=30 but no bytes dropped — wire noise knob is broken");

    // PASS condition: at least ONE of these is true:
    //   (a) the link ended up at a slower baud (>=1 baud change that
    //       didn't go back up), OR
    //   (b) the link disconnected at least once (protocol tried to
    //       recover, even if it landed back at the same baud after the
    //       sweep returned).
    //
    // Reverting the v5.1.28 fix makes errs not climb on gaps, so the
    // threshold doesn't trip, so neither (a) nor (b) happens reliably.
    bool fellBack = false;
    for (auto b : pingBauds) if (b < startBaud) { fellBack = true; break; }
    for (auto b : pongBauds) if (b < startBaud) { fellBack = true; break; }
    bool disconnected = (maxDisc > 0);

    if (fellBack || disconnected) {
        std::cout << "\nPASS: protocol recovered from wire noise "
                  << "(fellBack=" << fellBack << " disconnected=" << disconnected << ")"
                  << std::endl;
        return;
    }

    std::cout << "\nFAIL: 30% frame drop for " << RUN_MS
              << "ms produced NO baud fallback and NO disconnects. "
              << "This means gap events aren't bumping errs — the v5.1.28 "
              << "fix has been regressed." << std::endl;
    assert(false && "noise test: no recovery under 30% frame drop");
}

int main() {
    Log::log().setLevel(Log::Level::WARNING); // keep stdout readable
    test_loopback_noise_triggers_baud_fallback();
    std::cout << "\nAll loopback-noise tests PASSED." << std::endl;
    return 0;
}

#endif // !ARDUINO
