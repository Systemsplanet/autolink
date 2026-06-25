// Loopback with 30% frame drop: ARQ must recover, link
// must stay up.


#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include <chrono>
#    include <thread>
#    include "al/link/Link.h"
#    include "al/util/Log.h"
#    include "MockHal.h"

using namespace autolink;

void test_loopback_noise_triggers_baud_fallback()
{
    std::cout << "\n=== Test: Loopback under wire "
                 "noise triggers baud fallback ==="
              << std::endl;


    const int DROP_PCT = 1;
    const int RUN_MS = 5000;

    MockHal mHal, sHal;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 70000;
    cfg.maxMsg = 65535;


    cfg.errThreshold = 100;
    Link ping(mHal, true, cfg);
    Link pong(sHal, false, cfg);
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
              << "% errThreshold=" << cfg.errThreshold
              << std::endl;


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
            (int)std::chrono::duration_cast<
                std::chrono::milliseconds>(now - t0)
                .count();
        if (wallMs >= RUN_MS)
            break;


        mHal.pumpClock(20);
        sHal.pumpClock(20);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);

        if (ping.getState() == State::OK &&
            pong.getState() == State::OK) {
            uint8_t payload[64];
            for (int i = 0; i < 64; i++)
                payload[i] = (uint8_t)('A' + (i % 26));
            if (ping.sendMsg(payload, 64))
                txCount++;


            uint8_t buf[300];
            int n;
            while ((n = pong.recvMsg(
                        buf, sizeof(buf))) > 0) {
                if (pong.sendMsg(buf, n))
                    rxCount++;
            }
        }


        if (pingBauds.empty() ||
            pingBauds.back() != (uint32_t)mHal.spd)
            pingBauds.push_back(mHal.spd);
        if (pongBauds.empty() ||
            pongBauds.back() != (uint32_t)sHal.spd)
            pongBauds.push_back(sHal.spd);

        Stats ps, qs;
        ping.getStats(ps);
        pong.getStats(qs);
        uint64_t disc = ps.discCount + qs.discCount;
        if (disc > maxDisc)
            maxDisc = disc;

        if (wallMs - lastPrint >= 1000) {
            lastPrint = wallMs;
            std::cout << "T=" << (wallMs / 1000)
                      << "s  Ping@" << mHal.spd
                      << "  Pong@" << sHal.spd
                      << "  disc=" << disc
                      << "  bytesDropped="
                      << (mHal.bytesDropped +
                          sHal.bytesDropped)
                      << std::endl;
        }


        std::this_thread::sleep_for(
            std::chrono::milliseconds(2));
    }

    auto wallMs =
        (int)std::chrono::duration_cast<
            std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0)
            .count();
    Stats ps, qs;
    ping.getStats(ps);
    pong.getStats(qs);

    std::cout << "\n=== Final ===" << std::endl;
    std::cout << "Ran " << wallMs << " ms"
              << std::endl;
    std::cout << "Final baud Ping=" << mHal.spd
              << " Pong=" << sHal.spd << std::endl;
    std::cout << "Baud history Ping: ";
    for (auto b : pingBauds)
        std::cout << b << " ";
    std::cout << "\nBaud history Pong: ";
    for (auto b : pongBauds)
        std::cout << b << " ";
    std::cout << "\nDisconnects: Ping=" << ps.discCount
              << " Pong=" << qs.discCount
              << " (total=" << maxDisc << ")"
              << std::endl;
    std::cout << "Frame errors: Ping=" << ps.frameErrs
              << " Pong=" << qs.frameErrs << std::endl;
    std::cout << "Messages TX=" << txCount
              << " RX=" << rxCount << std::endl;
    std::cout << "Bytes dropped by noise: "
              << (mHal.bytesDropped +
                  sHal.bytesDropped)
              << std::endl;


    int totalDropped =
        mHal.bytesDropped + sHal.bytesDropped;
    assert(totalDropped > 0 &&
           "frameDropPct=30 but no bytes dropped — "
           "wire noise knob is broken");


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
        std::cout << "\nFAIL: 30% frame drop for "
                  << RUN_MS
                  << "ms produced NO baud fallback "
                     "and NO disconnects. "
                  << "This means gap events aren't "
                     "bumping errs — the "
                  << "fix has been regressed."
                  << std::endl;
        assert(false &&
               "noise test: no recovery under 30% "
               "frame drop");
    }


    int delivered =
        (rxCount > txCount) ? txCount : rxCount;
    int deliveryFloor = (txCount * 5) / 100;
    std::cout << "Delivery: TX=" << txCount
              << " RX=" << rxCount
              << " delivered=" << delivered
              << " floor=" << deliveryFloor
              << std::endl;
    if (delivered < deliveryFloor) {
        std::cout
            << "\nFAIL: " << delivered << "/"
            << txCount
            << " messages round-tripped — below the "
            << "5% floor under " << DROP_PCT
            << "% frame drop. "
            << "Either retransmit is dropping on "
               "arrival "
            << "(forward-resync-shape bug) or the "
               "staleness "
            << "cap is firing too aggressively."
            << std::endl;
        assert(false &&
               "noise test: ARQ delivery below 5% "
               "under realistic noise");
    }

    std::cout << "\nPASS: protocol recovered from "
                 "wire noise "
              << "(fellBack=" << fellBack
              << " disconnected=" << disconnected
              << " delivered=" << delivered << "/"
              << txCount << ")" << std::endl;
}

int main()
{
    Log::log().setLevel(Log::Level::WARNING);
    test_loopback_noise_triggers_baud_fallback();
    std::cout << "\nAll loopback-noise tests PASSED."
              << std::endl;
    return 0;
}

#endif
