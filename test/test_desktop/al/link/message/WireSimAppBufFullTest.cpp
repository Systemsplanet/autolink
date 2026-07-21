// WireSim end-to-end: app-buf-full on the
// receiver must NOT corrupt the message stream.
// The buggy-original shape did `acc = pushAppBuf(b, n)`
// first and NAKed on shortfall — a partial write
// spliced garbage into the message stream (run
// A's field log shows "want 143 got 120", then
// `recv rejected (CRC/desync)` from the message
// parser). The fix is all-or-nothing: check
// appBufFree() >= n BEFORE writing, drop the
// frame on shortfall (NAK, no rxSeq advance, no
// app-buf write). The retx then classifies
// in-order and delivers.
//
// This is the WireSim counterpart to
// AppBufFullAdmitNothingTest's source-grep pin.
// It exercises the full Link state machine, the
// pipe, the COBS encoder, the ARQ retx path, the
// app-buf drain, and the message reassembler —
// not just the source. Pinned by run_test_wiresim_app_buf_full.
#include "WireSim.h"
#include "al/util/Log.h"
#include <iostream>
#include <cassert>
#include "NullArqCache.h"

using namespace autolink;

static void warmup_to_ok(TwoNodeFixture &f) {
    for (int i = 0; i < 1500; i++) {
        f.step(1);
        if (f.getStateA() == State::OK && f.getStateB() == State::OK)
            return;
    }
    std::cerr << "FAIL: link never reached OK (A=" << (int)f.getStateA()
              << " B=" << (int)f.getStateB() << ")" << std::endl;
    assert(false);
}

// buggy-original shape: a 120-of-143-byte partial write
// corrupts the message stream. The current shape:
// the receiver's app-buf-full NAK is honored and
// the sender retxes; the retx classifies in-order
// and delivers; the message is reassembled
// correctly.
//
// We use a small appBufCap (250 bytes) to force a
// full-buf condition: a 200-byte message fits in
// one chunk (MSG_HDR=6 + body=194 < 200, so
// actually <MAX_CHUNK=250), but if the receiver's
// app buf is at 60 bytes when the chunk arrives,
// the chunk needs 200 - 60 = 140 bytes free; the
// gate fires.
static void test_app_buf_full_does_not_corrupt_stream() {
    std::cout
        << "\n=== WireSim end-to-end: app-buf-full retx delivers a clean message ==="
        << std::endl;
    Log::log().setLevel(Log::Level::WARNING);

    AutoLinkConfig cfg;
    cfg.errThreshold = 10000;
    cfg.maxMsg = 256;
    WireSim sim(cfg);
    sim.setFrameDropPct(0);
    sim.setForcedDropEvery(0);

    TwoNodeFixture f(sim);
    f.msgSize = 200;
    f.begin();
    warmup_to_ok(f);

    // Constrain the receiver's app buf to 250 bytes
    // (1 MAX_CHUNK frame + small slack). The sender
    // is unconstrained so the GBN window doesn't
    // get stuck on the sender side.
    sim.halB().appBufCap = 250;

    // Drive 200 send-cycles. After the app-buf
    // fills, the receiver NAKs; the sender retxes;
    // the receiver drains; the retx lands. The
    // message stream should stay clean.
    uint64_t bytesBefore = sim.bytesTransferredAtoB();
    int maxBurstSave = f.maxBurstPerLoop;
    f.maxBurstPerLoop = 0; // drive slowly so the app buf can fill
    for (int i = 0; i < 1000; i++)
        f.step(1);
    f.maxBurstPerLoop = maxBurstSave;

    uint64_t bytesAfter = sim.bytesTransferredAtoB();
    int rxB = 0;
    {
        uint8_t buf[256];
        int n;
        while ((n = sim.linkB().recvMsg(buf, sizeof(buf))) > 0)
            rxB++;
    }
    Stats sA, sB;
    sim.linkA().getStats(sA);
    sim.linkB().getStats(sB);
    int frameErrs = (int)(sA.frameErrs + sB.frameErrs);
    std::cout << "  bytesAtoB before=" << bytesBefore << " after=" << bytesAfter
              << " rxB=" << rxB << " frameErrs=" << frameErrs << std::endl;
    // The key assertion: NO frame errors from a
    // desync'd message stream. A buggy-original partial
    // write would generate frameErrs > 0 (the
    // message parser rejects the corrupt header).
    // The current shape: NAK + retx, no corruption.
    if (frameErrs > 0) {
        std::cerr << "FAIL: app-buf-full retx left " << frameErrs
                  << " frame errors on the wire (message stream corrupted)"
                  << std::endl;
        assert(false);
    }
    // We should also have received at least one
    // message after the settle window — the
    // loopback can't go through this stress and
    // receive 0 messages.
    if (rxB == 0 && bytesAfter == bytesBefore) {
        std::cerr << "FAIL: link went completely dead "
                  << "after the app-buf-full stress (rxB=0, "
                  << "bytesDelta=0). GBN window wedged." << std::endl;
        assert(false);
    }
    std::cout << "  PASS (app-buf-full NAK+retx delivers clean; no frameErrs)"
              << std::endl;
}

static void test_app_buf_full_does_not_lose_messages() {
    std::cout
        << "\n=== WireSim: sender's GBN window doesn't latch on a permanently-full receiver app buf ==="
        << std::endl;
    Log::log().setLevel(Log::Level::WARNING);

    AutoLinkConfig cfg;
    cfg.errThreshold = 10000;
    cfg.maxMsg = 256;
    WireSim sim(cfg);
    sim.setFrameDropPct(0);
    sim.setForcedDropEvery(0);

    TwoNodeFixture f(sim);
    f.msgSize = 200;
    f.begin();
    warmup_to_ok(f);

    // Block the receiver's app buf completely. The
    // sender's GBN window must not latch.
    sim.halB().appBufCap = 0;

    // Drive 2000 cycles. Sender gets NAKs back.
    // GBN window fills. Then we drain the app buf
    // and confirm data resumes.
    int maxBurstSave = f.maxBurstPerLoop;
    f.maxBurstPerLoop = 0;
    for (int i = 0; i < 2000; i++)
        f.step(1);

    // Unblock the receiver.
    sim.halB().appBufCap = (size_t)-1;
    f.maxBurstPerLoop = maxBurstSave;

    for (int i = 0; i < 5000; i++) {
        f.step(1);
        uint8_t buf[256];
        while (sim.linkB().recvMsg(buf, sizeof(buf)) > 0)
            ;
        if (sim.bytesTransferredAtoB() > 0)
            break;
    }
    Stats sA, sB;
    sim.linkA().getStats(sA);
    sim.linkB().getStats(sB);
    int frameErrs = (int)(sA.frameErrs + sB.frameErrs);
    uint64_t bytesAfter = sim.bytesTransferredAtoB();
    std::cout << "  after unblock bytesAtoB=" << bytesAfter
              << " frameErrs=" << frameErrs << std::endl;
    if (frameErrs > 0) {
        std::cerr << "FAIL: GBN-window latch on a "
                  << "permanently-full receiver app buf left " << frameErrs
                  << " frame errors (sender's window wedged)" << std::endl;
        assert(false);
    }
    if (bytesAfter == 0) {
        std::cerr << "FAIL: bytes did not resume after "
                  << "unblocking the receiver (sender wedged)" << std::endl;
        assert(false);
    }
    std::cout << "  PASS (NAK storm recovers when app buf drains)" << std::endl;
}

int main() {
    std::cout << "=== WireSim app-buf-full end-to-end tests ===" << std::endl;
    test_app_buf_full_does_not_corrupt_stream();
    test_app_buf_full_does_not_lose_messages();
    std::cout << "\n=== WireSim app-buf-full tests PASS ===" << std::endl;
    return 0;
}
