// Fill every byte 0x00-0xFF into a buffer, push it through the
// loopback, and verify the receiver reassembles it byte-for-byte.
// Pins todo item 1's framing fix: the OK-state CTRL scan used to
// claim any `0xAA 0x55` 5-byte run as a CTRL frame, which collided
// with random payload bytes. With `0xAA 0x55` being the worst-case
// fill, the pre-fix build's framer ate the first 5 bytes of
// payload as if they were a CTRL frame, called err_unlocked() on
// the CRC8 fail, and after errThreshold of these across the suite
// dropped the link.
//
// Test layout:
//   Pin 1 — every byte 0x00-0xFF as a 2 KB single-frame-ish fill
//           (MSG_HDR + 2046 bytes lands in the SYNC-merged single-
//           frame path; large enough to step cleanly into multi-
//           chunk territory when MSG_HDR is added).
//   Pin 2 — random-sized buffer >2 KB filled with each byte,
//           exercising sizes that straddle 2 KB and the multi-
//           chunk frame boundary.
//
// Both ASYNC (non-blocking) so the test stays subsecond. Built
// directly on Link + MockHal + ArqCache so we can drive byte-fill
// sequences end-to-end without depending on TwoNodeFixture's
// drivePong_ echo (which would corrupt the byte-keyed
// verification when payload size grew past default msgSize).
#ifndef AUTOLINK_HOST_TEST
#    error "Build with -DAUTOLINK_HOST_TEST (see test/test_desktop/Makefile)"
#endif

#include <iostream>
#include <cassert>
#include <vector>
#include <cstring>
#include "MockHal.h"
#include "LinkTestAccessor.h"
#include "al/link/Link.h"
#include "al/link/arq/ArqCache.h"
#include "al/AutoLinkConfig.h"
#include "al/util/Log.h"

using namespace autolink;

namespace {

void fillAll(std::vector<uint8_t> &buf, uint8_t b) {
    for (size_t i = 0; i < buf.size(); i++)
        buf[i] = b;
}

// Bring two MockHal-backed Links to OK in ASYNC mode.
bool bringToOk(Link &a, Link &b, MockHal &mA, MockHal &sA) {
    // First sync the baud — MockHal's pipe_data drops bytes whose
    // tx baud doesn't match the dest's spd. Both ends default to
    // 115200 unless the cfg's allowedBauds override them.
    if (mA.spd != sA.spd) {
        if (mA.spd == 0)
            mA.spd = sA.spd;
        else
            sA.spd = mA.spd;
    }
    for (int i = 0; i < 3000; i++) {
        mA.pumpClock(2);
        sA.pumpClock(2);
        pipe_data(mA, sA);
        pipe_data(sA, mA);
        if (a.getState() == State::OK && b.getState() == State::OK)
            return true;
    }
    return false;
}

// Pump a MockHal pair until rxB has received a message matching
// `expected` (size + bytes) or budget runs out.
bool driveUntilReceived(Link &txLink, Link &rxLink, MockHal &mA, MockHal &sA,
                        const std::vector<uint8_t> &expected,
                        std::vector<uint8_t> &out, int budgetCycles,
                        int cycleMs = 2) {
    int got = 0;
    for (int cycle = 0; cycle < budgetCycles; cycle++) {
        mA.pumpClock((uint32_t)cycleMs);
        sA.pumpClock((uint32_t)cycleMs);
        pipe_data(mA, sA);
        pipe_data(sA, mA);
        if (rxLink.getState() == State::OK) {
            got = rxLink.recvMsg(out.data(), (int)out.size());
            if (got == (int)expected.size()) {
                if (memcmp(out.data(), expected.data(), got) == 0)
                    return true;
                std::cerr << "\n    payload mismatch (first diff):";
                for (int i = 0; i < got; i++) {
                    if (out[i] != expected[i]) {
                        std::cerr << " byte " << i << " got 0x" << std::hex
                                  << (int)out[i] << " want 0x"
                                  << (int)expected[i] << std::dec << std::endl;
                        break;
                    }
                }
                return false;
            }
        }
    }
    std::cerr << "\n    timeout: budget " << budgetCycles << " cycles ("
              << budgetCycles * cycleMs << " ms), got " << got
              << " bytes (want " << expected.size() << ")"
              << " txState=" << (int)txLink.getState()
              << " rxState=" << (int)rxLink.getState() << std::endl;
    return false;
}

void test_fill_byte_roundtrip_2k() {
    std::cout << "\n=== Pin 1: every byte 0x00-0xFF as 2 KB fill"
                 " round-trips without framing errors ==="
              << std::endl;
    Log::log().setLevel(Log::Level::ERROR);

    constexpr int kFillLen = 2048;
    std::vector<uint8_t> tx(kFillLen);
    std::vector<uint8_t> rx(kFillLen + 64);

    for (int b = 0; b <= 0xFF; b++) {
        AutoLinkConfig cfg;
        cfg.maxMsg = 5120;
        cfg.errThreshold = 20;
        cfg.idleTimeoutMs = 5000;
        cfg.allowedBaudsCount = 1;
        cfg.allowedBauds[0] = 115200;
        cfg.syncAckTimeoutMs = 50;

        ArqCache aArq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
        ArqCache bArq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
        MockHal mA, sA;
        mA.peer = &sA;
        sA.peer = &mA;
        Link ping(mA, aArq, true, cfg);
        Link pong(sA, bArq, false, cfg);
        ping.setMode(AutoLinkConfig::Mode::ASYNC);
        pong.setMode(AutoLinkConfig::Mode::ASYNC);
        ping.begin();
        pong.begin();
        if (!bringToOk(ping, pong, mA, sA)) {
            std::cerr << "\nFAIL (byte 0x" << std::hex << b << std::dec
                      << "): link never reached OK" << std::endl;
            assert(false);
        }

        Stats baseA, baseB;
        ping.getStats(baseA);
        pong.getStats(baseB);

        fillAll(tx, (uint8_t)b);
        // ASYNC non-blocking send. 22 chunks at MAX_CHUNK=250 →
        // ~22 ARQ entries, but with a 32-deep ARQ pipeline this
        // fits comfortably.
        bool ok = ping.sendMsg(tx.data(), kFillLen);
        assert(ok && "sendMsg should accept a 2 KB fill of a single byte");

        if (!driveUntilReceived(ping, pong, mA, sA, tx, rx,
                                /*budget=*/1500, /*cycleMs=*/2)) {
            std::cerr << "\nFAIL (byte 0x" << std::hex << b << std::dec
                      << ", size 2 KB)" << std::endl;
            assert(false);
        }

        Stats ps, qs;
        ping.getStats(ps);
        pong.getStats(qs);
        int dropsA = (int)(ps.discCount - baseA.discCount);
        int dropsB = (int)(qs.discCount - baseB.discCount);
        int errsA = (int)(ps.frameErrs - baseA.frameErrs);
        int errsB = (int)(qs.frameErrs - baseB.frameErrs);
        if (dropsA + dropsB > 0 || errsA + errsB > 0) {
            std::cerr << "\nFAIL (byte 0x" << std::hex << b << std::dec
                      << "): drops A=" << dropsA << " B=" << dropsB
                      << " frameErrs A=" << errsA << " B=" << errsB
                      << std::endl;
            assert(false);
        }
    }
    std::cout << "  PASS (256/256 byte fills across 2 KB round-trip cleanly)"
              << std::endl;
}

void test_fill_byte_roundtrip_large() {
    std::cout << "\n=== Pin 2: random-sized buffer >2 KB filled with each byte"
                 " round-trips without framing errors ==="
              << std::endl;
    Log::log().setLevel(Log::Level::ERROR);

    // Sizes that straddle 2 KB and the default maxMsg. Each iteration
    // starts a fresh wire so the per-byte diff in payload doesn't
    // accumulate. ASYNC mode at 115200 baud with one in-flight message
    // at a time.
    int sizes[] = { 2050, 2500, 3000, 4000, 5120 };

    for (int b = 0; b <= 0xFF; b++) {
        AutoLinkConfig cfg;
        cfg.maxMsg = 5120;
        cfg.errThreshold = 20;
        cfg.idleTimeoutMs = 5000;
        cfg.allowedBaudsCount = 1;
        cfg.allowedBauds[0] = 115200;
        cfg.syncAckTimeoutMs = 50;

        ArqCache aArq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
        ArqCache bArq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
        MockHal mA, sA;
        mA.peer = &sA;
        sA.peer = &mA;
        Link ping(mA, aArq, true, cfg);
        Link pong(sA, bArq, false, cfg);
        ping.setMode(AutoLinkConfig::Mode::ASYNC);
        pong.setMode(AutoLinkConfig::Mode::ASYNC);
        ping.begin();
        pong.begin();
        if (!bringToOk(ping, pong, mA, sA)) {
            std::cerr << "\nFAIL: link never reached OK for byte 0x" << std::hex
                      << b << std::dec << std::endl;
            assert(false);
        }

        Stats baseA, baseB;
        ping.getStats(baseA);
        pong.getStats(baseB);

        for (int sz : sizes) {
            std::vector<uint8_t> tx(sz);
            std::vector<uint8_t> rx(sz + 64);
            fillAll(tx, (uint8_t)b);

            bool ok = ping.sendMsg(tx.data(), sz);
            assert(ok && "sendMsg should accept a buffer up to maxMsg");

            if (!driveUntilReceived(ping, pong, mA, sA, tx, rx,
                                    /*budget=*/4000, /*cycleMs=*/2)) {
                std::cerr << "\nFAIL (byte 0x" << std::hex << b << std::dec
                          << ", size=" << sz << ")" << std::endl;
                assert(false);
            }

            Stats ps, qs;
            ping.getStats(ps);
            pong.getStats(qs);
            int dropsA = (int)(ps.discCount - baseA.discCount);
            int dropsB = (int)(qs.discCount - baseB.discCount);
            int errsA = (int)(ps.frameErrs - baseA.frameErrs);
            int errsB = (int)(qs.frameErrs - baseB.frameErrs);
            if (dropsA + dropsB > 0 || errsA + errsB > 0) {
                std::cerr << "\nFAIL (byte 0x" << std::hex << b << std::dec
                          << ", size=" << sz << "): drops A=" << dropsA
                          << " B=" << dropsB << " frameErrs A=" << errsA
                          << " B=" << errsB << std::endl;
                assert(false);
            }
        }
    }
    std::cout << "  PASS (256/256 byte fills across 5 sizes >2 KB each)"
              << std::endl;
}

} // namespace

int main() {
    std::cout << "=== Fill-Byte Roundtrip Tests ===" << std::endl;
    test_fill_byte_roundtrip_2k();
    test_fill_byte_roundtrip_large();
    std::cout << "\n=== Fill-Byte Roundtrip Tests Completed ===" << std::endl;
    return 0;
}
