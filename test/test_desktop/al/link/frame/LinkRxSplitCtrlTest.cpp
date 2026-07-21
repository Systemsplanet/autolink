// Regression: OK-state CTRL frames (0xAA 0x55 X Y Z, 5 bytes) split
// across two onRx() delivery chunks must be held and reassembled,
// not fall through to the COBS framer — that costs a frameErr and
// loses the CTRL frame. MockHal's single-shot tx() delivery never
// exercises this on its own; real UART reads land on arbitrary byte
// boundaries. okCarry_ holds a trailing 0xAA (or 0xAA 0x55...)
// candidate across the onRx() boundary until it either completes
// (CRC pass -> processed as CTRL) or is disqualified (CRC fail ->
// fed to the COBS framer as payload).
//
// Pins:
//   1. A valid PING CTRL frame delivered split at every byte
//      boundary (1..4) in OK state is processed (pong replies
//      with a pong-ack), zero frameErrs. Toggle off the okCarry_
//      path -> red (CTRL frame lost, no pong-ack observed).
//   2. A 64-byte payload of alternating 0xAA 0x55 delivered split
//      right after every payload 0xAA (hold-then-disqualify path)
//      still reassembles byte-for-byte at zero frameErrs.
#include <iostream>
#include <cassert>
#include <cstring>
#include <vector>
#include "MockHal.h"
#include "TestCfg.h"
#include "LinkTestAccessor.h"
#include "al/link/Link.h"
#include "al/link/arq/ArqCache.h"
#include "al/AutoLinkConfig.h"
#include "al/util/Log.h"
#include "al/util/UtilCrc.h"

using namespace autolink;

namespace {

void test_split_ping_ctrl_all_boundaries() {
    std::cout << "\n=== Pin 1: split PING CTRL frame at every byte boundary "
                 "reassembles cleanly ==="
              << std::endl;
    Log::log().setLevel(Log::Level::ERROR);

    for (int split = 1; split < CTRL_FRAME_SIZE; split++) {
        AutoLinkConfig cfg;
        testBaseCfg(cfg);
        cfg.mode = AutoLinkConfig::Mode::ASYNC;

        ArqCache aArq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
        ArqCache bArq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
        MockHal mHal, sHal;
        mHal.peer = &sHal;
        sHal.peer = &mHal;
        Link ping(mHal, aArq, true, cfg);
        Link pong(sHal, bArq, false, cfg);
        ping.begin();
        pong.begin();
        lockPair(ping, pong, mHal, sHal);
        if (ping.getState() != State::OK || pong.getState() != State::OK) {
            std::cerr << "\nFAIL (split=" << split << "): link never reached OK"
                      << std::endl;
            assert(false);
        }
        sHal.clearTx();
        mHal.clearTx();

        uint8_t frame[CTRL_FRAME_SIZE];
        frame[0] = 0xAA;
        frame[1] = 0x55;
        // Use the latched peerSweepEpoch_ so the epoch-mismatch
        // check in processCtrlFrame_unlocked treats this as a
        // genuine keepalive PING, not as a peer that restarted.
        // (The fix carries a session epoch in the seq byte of
        // every sweep frame; an OK-state PING with a non-matching
        // epoch forces a resync, which the test does not want.)
        frame[CTRL_FRAME_SEQ_IDX] =
            LinkTestAccessor(pong).peerSweepEpochForTest();
        frame[CTRL_FRAME_PAYLOAD_IDX] = PING_CMD;
        frame[CTRL_FRAME_CRC_IDX] = UtilCrc::crc8(frame, CTRL_FRAME_SIZE - 1);

        Stats base;
        pong.getStats(base);

        pong.onRx(frame, split);
        pong.onRx(frame + split, CTRL_FRAME_SIZE - split);

        Stats after;
        pong.getStats(after);
        if (after.frameErrs != base.frameErrs) {
            std::cerr << "\nFAIL (split=" << split << "): frameErrs "
                      << base.frameErrs << " -> " << after.frameErrs
                      << std::endl;
            assert(false);
        }
        bool gotPongAck = sHal.txBuf.size() >= CTRL_FRAME_SIZE &&
            sHal.txBuf[0] == 0xAA && sHal.txBuf[1] == 0x55 &&
            sHal.txBuf[CTRL_FRAME_PAYLOAD_IDX] == PONG_CMD;
        if (!gotPongAck) {
            std::cerr << "\nFAIL (split=" << split
                      << "): no pong-ack observed on wire (CTRL frame lost)"
                      << std::endl;
            assert(false);
        }
    }
    std::cout << "  PASS (splits 1..4 all reassemble the CTRL frame, "
                 "pong-ack observed, zero frameErrs)"
              << std::endl;
}

void test_split_payload_ctrl_collision_disqualifies_cleanly() {
    std::cout << "\n=== Pin 2: 64-byte 0xAA/0x55 payload split after every "
                 "payload 0xAA reassembles byte-for-byte ==="
              << std::endl;
    Log::log().setLevel(Log::Level::ERROR);

    AutoLinkConfig cfg;
    testBaseCfg(cfg);
    cfg.mode = AutoLinkConfig::Mode::ASYNC;

    ArqCache aArq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
    ArqCache bArq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link ping(mHal, aArq, true, cfg);
    Link pong(sHal, bArq, false, cfg);
    ping.begin();
    pong.begin();
    lockPair(ping, pong, mHal, sHal);
    assert(ping.getState() == State::OK && pong.getState() == State::OK);
    sHal.clearTx();
    mHal.clearTx();

    constexpr int kLen = 64;
    std::vector<uint8_t> tx(kLen);
    for (int i = 0; i < kLen; i++)
        tx[i] = (i % 2 == 0) ? 0xAA : 0x55;

    bool ok = ping.sendMsg(tx.data(), kLen);
    assert(ok && "sendMsg should accept a 64-byte fill");

    // The 70-byte (MSG_HDR + payload) merged frame lands in
    // mHal.txBuf synchronously (single-chunk ASYNC path). Capture
    // it and hand-deliver to pong in controlled fragments instead
    // of the normal single-shot pipe_data().
    std::vector<uint8_t> wire = mHal.drainTx();
    assert(!wire.empty());

    Stats base;
    pong.getStats(base);

    // Split right after every wire 0xAA — reproduces UART reads
    // landing mid-CTRL-candidate on every payload byte, forcing
    // the hold-then-disqualify path repeatedly.
    size_t start = 0;
    for (size_t i = 0; i < wire.size(); i++) {
        if (wire[i] != 0xAA)
            continue;
        pong.onRx(wire.data() + start, (int)(i + 1 - start));
        start = i + 1;
    }
    if (start < wire.size())
        pong.onRx(wire.data() + start, (int)(wire.size() - start));

    Stats after;
    pong.getStats(after);
    if (after.frameErrs != base.frameErrs) {
        std::cerr << "\nFAIL: frameErrs " << base.frameErrs << " -> "
                  << after.frameErrs << std::endl;
        assert(false);
    }

    std::vector<uint8_t> rx(kLen + 16);
    int got = pong.recvMsg(rx.data(), (int)rx.size());
    if (got != kLen || memcmp(rx.data(), tx.data(), kLen) != 0) {
        std::cerr << "\nFAIL: got=" << got << " want=" << kLen << std::endl;
        assert(false);
    }
    std::cout << "  PASS (64-byte 0xAA/0x55 fill reassembles byte-for-byte, "
                 "zero frameErrs, split after every payload 0xAA)"
              << std::endl;
}

} // namespace

int main() {
    std::cout << "=== LinkRx Split-CTRL Tests ===" << std::endl;
    test_split_ping_ctrl_all_boundaries();
    test_split_payload_ctrl_collision_disqualifies_cleanly();
    std::cout << "\n=== LinkRx Split-CTRL Tests Completed ===" << std::endl;
    return 0;
}
