// AL89 pin 1 / SyncMultiChunkDrainRemovedTest. AL-D1: converted from
// a source-grep (checking the identifier "fullChunks" is absent
// from LinkApi.cpp) to a real behavioral test. A source-grep here
// is unusually weak: renaming the same reservation logic to any
// other identifier would pass the old test while reintroducing the
// exact bug. This test instead sizes the TX ring to a value the OLD
// whole-burst-upfront reservation could never satisfy but the NEW
// per-chunk drain succeeds against easily, and drives a REAL
// blocking SYNC sendMsg() across two real Link+MockHal nodes with a
// background pumper thread doing the ACK exchange — the actual
// production code path, not a re-declaration of its logic.
#include "FieldWedgeFixes89Common.h"

#include <atomic>
#include <thread>
#include <chrono>

using namespace autolink;
using namespace autolink::field89;

namespace {
void pumpBothWhile(MockHal &a, MockHal &b, std::atomic<bool> &stop) {
    while (!stop.load()) {
        a.pumpClock(5);
        b.pumpClock(5);
        pipe_data(a, b);
        pipe_data(b, a);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}
} // namespace

// Pin 1 (AL89-1): sendMsg's SYNC multi-chunk branch no longer
// pre-reserves the full chunk set before writing any byte. The
// field capture's >2000 B messages were deterministically dropped
// because the old reservation demanded
// kWorstCaseCobsFrame*(msgChunks+1) bytes of TX-ring headroom ALL
// AT ONCE before the first byte went out, and the ring was sized
// exactly to that floor — unsatisfiable by construction. Toggle off
// (re-add a whole-burst upfront reservation check) -> red: this
// test forces a ring far too small for that reservation but large
// enough for one frame at a time, and the real blocking sendMsg()
// must still complete.
void test_SyncMultiChunkDrainRemovedTest() {
    std::cout << "\n=== Pin 1: SYNC multi-chunk pre-drain "
                 "reservation removed ==="
              << std::endl;
    ArqCache pingArq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
    ArqCache pongArq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
    AutoLinkConfig cfg;
    testBaseCfg(cfg);
    cfg.mode = AutoLinkConfig::Mode::SYNC;
    cfg.maxMsg = 2048;
    cfg.syncAckTimeoutMs = 500;

    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link ping(mHal, pingArq, true, cfg);
    Link pong(sHal, pongArq, false, cfg);
    ping.setMode(AutoLinkConfig::Mode::SYNC);
    pong.setMode(AutoLinkConfig::Mode::SYNC);
    ping.begin();
    pong.begin();
    lockPair(ping, pong, mHal, sHal);
    assert(ping.getState() == State::OK);
    assert(pong.getState() == State::OK);
    mHal.txBuf.clear();
    sHal.txBuf.clear();

    // len=600 -> chunksForMsgLen(600) = 4 (1 header chunk + 3 data
    // chunks). kWorstCaseCobsFrame = 262. The OLD whole-burst
    // reservation demanded 262*(4+1) = 1310 B all at once, before
    // writing a single byte. Force the ring to 400 B — enough for
    // one frame (262 B) plus a little, nowhere near 1310 B. Set
    // AFTER begin() (which would otherwise size the ring off cfg's
    // own, larger, per-message floor) — setTxCapForTest's
    // txCapUserSet_ flag stops any later internal resize from
    // overriding this value.
    mHal.setTxCapForTest(400);

    const int msgLen = 600;
    std::vector<uint8_t> payload(msgLen);
    for (int i = 0; i < msgLen; i++)
        payload[i] = (uint8_t)(i & 0xFF);

    std::atomic<bool> stop{ false };
    std::thread pumper(pumpBothWhile, std::ref(mHal), std::ref(sHal),
                       std::ref(stop));
    bool sent = ping.sendMsg(payload.data(), msgLen);
    stop.store(true);
    pumper.join();

    std::cout << "  sendMsg(600 B, 4 chunks) against a 400 B ring "
                 "(old reservation needed 1310 B): sent=" << sent
              << std::endl;
    if (!sent) {
        std::cerr << "\nFAIL: sendMsg failed against a 400 B ring that "
                     "easily holds one frame (262 B) at a time. If the "
                     "whole-burst upfront reservation (262*(msgChunks+1) "
                     "= 1310 B for this message) is back, this is exactly "
                     "how the field capture's >2000 B messages were "
                     "deterministically dropped — the ring can never "
                     "satisfy that much headroom at once."
                  << std::endl;
        assert(false);
    }

    std::vector<uint8_t> rx(msgLen + 64);
    int got = pong.recvMsg(rx.data(), (int)rx.size());
    if (got != msgLen) {
        std::cerr << "\nFAIL: pong reassembled " << got
                  << " B, expected " << msgLen << std::endl;
        assert(false);
    }
    for (int i = 0; i < msgLen; i++) {
        if (rx[i] != payload[i]) {
            std::cerr << "\nFAIL: byte mismatch at offset " << i
                      << std::endl;
            assert(false);
        }
    }
    std::cout << "  PASS (600 B / 4 chunks delivered byte-for-byte "
                 "against a ring far too small for a whole-burst "
                 "reservation)"
              << std::endl;
}
